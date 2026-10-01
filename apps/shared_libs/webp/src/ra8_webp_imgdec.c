/**
 * @file ra8_webp_imgdec.c
 * @brief `ra8_imgdec` backend over the vendored libwebp facade (RA8FW-308).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / WebP] {World: NS}
 *
 * @details
 * The vtable, the caps record and the decode hook for ::ra8_webp_imgdec_bind.
 * The hook is a thin adapter: it re-checks the container it was told it holds,
 * pre-flights the geometry and the destination, hands the bound arena to the
 * existing ra8_webp_decode_rgba() call, and reads the source's alpha flag out
 * of the container header. No decoding logic is added or duplicated here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_webp_imgdec.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "ra8_log.h"
#include "ra8_webp.h"

/** @brief Module log tag. */
static const char* const s_tag_webpdec = "ra8_webp_imgdec";

/**
 * @enum ra8_webp_imgdec_const_t
 * @brief Container offsets and sizes this adapter reads (no magic numbers).
 */
typedef enum : uint32_t {
  k_webpdec_chunk_ofs = 12U,   /* First chunk FourCC, past RIFF+size+WEBP. */
  k_webpdec_body_ofs  = 20U,   /* First chunk payload, past FourCC+size.   */
  k_webpdec_vp8x_flag = 20U,   /* VP8X feature-flags byte.                 */
  k_webpdec_vp8x_min  = 21U,   /* Bytes needed to read that flags byte.    */
  k_webpdec_vp8l_min  = 25U,   /* Bytes needed to read VP8L's packed word. */
  k_webpdec_alpha_bit = 0x10U, /* VP8X flags: ALPH present.                */
  k_webpdec_l8_sig    = 0x2FU, /* VP8L signature byte at the payload head. */
  k_webpdec_l8_shift  = 28U,   /* VP8L packed word: alpha_is_used bit.     */
} ra8_webp_imgdec_const_t;

/** @brief The one format this backend opens. */
#define RA8_WEBP_IMGDEC_FORMATS ((uint32_t)k_ra8_imgdec_format_webp)

/** @brief The one layout ra8_webp_decode_rgba() writes. */
#define RA8_WEBP_IMGDEC_PIXELS ((uint32_t)k_ra8_imgdec_pixel_rgba8888)

/* The whole surface is measured in 32 bits, so the widest frame this backend
 * admits must fit there at RGBA8888. */
static_assert(((uint64_t)k_ra8_webp_max_dim * (uint64_t)k_ra8_webp_max_dim *
               (uint64_t)k_ra8_webp_bytes_per_px) <= (uint64_t)UINT32_MAX,
              "a dim_max surface must not overflow a 32-bit byte count");

/* The facade takes its lengths as size_t but converts the stride to int, so a
 * request this adapter accepts must be expressible there. */
static_assert(sizeof(size_t) >= sizeof(uint32_t), "a request length must fit a size_t");

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Read one little-endian 32-bit word out of the container.
 *
 * @param[in] p Four readable bytes.
 * @return uint32_t The assembled word.
 * @pre @p p points at four readable bytes.
 * @post No state is mutated.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_le32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

/**
 * @brief Verify the container and read its declared geometry, in one pass.
 *
 * @details Both duties the fabric leaves to a backend are answered by
 * ::ra8_imgdec_dims, so this asks it once rather than sniffing and then
 * probing. The container check is not ceremony: ra8_webp_get_info() would
 * refuse a PNG anyway, but refusing it here means a consumer routing by format
 * gets the same `not_supported` from every backend rather than one facade's
 * `validation_failed`.
 *
 * @param[in]  req   Validated request.
 * @param[out] out_w Receives the declared width in pixels.
 * @param[out] out_h Receives the declared height in pixels.
 * @return Result code.
 * @retval k_ra8_ok                Container matches and both dims fit.
 * @retval k_ra8_err_not_supported The bytes hold some other container.
 * @retval k_ra8_err_invalid_size  A dimension is past this backend's `dim_max`.
 * @retval other                   Propagated from ::ra8_imgdec_dims.
 * @pre @p req has passed the fabric's guarantees.
 * @post On success both outputs are non-zero and within `dim_max`.
 * @note Thread-safe: the shared probe touches no module state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_probe(const ra8_imgdec_req_t* req, uint32_t* out_w, uint32_t* out_h)
{
  ra8_imgdec_geom_t geom = {};
  const ra8_err_t   err  = ra8_imgdec_dims(req->bytes, req->byte_count, &geom);
  if (err != k_ra8_ok) {
    return err;
  }
  if (geom.format != req->format) {
    return k_ra8_err_not_supported;
  }
  if ((geom.width_px > (uint32_t)k_ra8_webp_max_dim) ||
      (geom.height_px > (uint32_t)k_ra8_webp_max_dim)) {
    return k_ra8_err_invalid_size;
  }
  *out_w = geom.width_px;
  *out_h = geom.height_px;
  return k_ra8_ok;
}

/**
 * @brief Decide whether the request's destination can take the surface.
 *
 * @details The facade decodes straight into `req->dst`, and it demands a
 * capacity of `height * stride` rather than the `(height - 1) * stride +
 * packed` the last row actually occupies. That is the facade's contract, so
 * this adapter applies the same bar instead of passing a smaller capacity and
 * letting the facade refuse: a caller gets `invalid_size` from the backend
 * that knows why, not `range_check_failed` from a layer below it.
 *
 * @param[in]  req      Validated request.
 * @param[in]  rows     Declared height in pixels.
 * @param[in]  packed   Bytes in one packed row of the declared frame.
 * @param[out] out_span Receives the capacity the facade will be handed.
 * @param[out] out_used Receives the bytes the decoded image actually occupies.
 * @return Result code.
 * @retval k_ra8_ok               The destination is large enough.
 * @retval k_ra8_err_invalid_size Too small, or the stride is under one row.
 * @pre @p rows and @p packed describe the frame ::internal_probe read.
 * @post On success `*out_span` is within `req->dst_bytes`.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dst_ok(const ra8_imgdec_req_t* req,
                                              uint32_t                rows,
                                              uint32_t                packed,
                                              uint32_t*               out_span,
                                              uint32_t*               out_used)
{
  const uint32_t stride = (req->dst_stride != 0U) ? req->dst_stride : packed;
  if (stride < packed) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t span = (uint64_t)rows * (uint64_t)stride;
  if (span > (uint64_t)req->dst_bytes) {
    return k_ra8_err_invalid_size;
  }
  *out_span = (uint32_t)span;
  *out_used = (uint32_t)(((uint64_t)(rows - 1U) * (uint64_t)stride) + (uint64_t)packed);
  return k_ra8_ok;
}

/**
 * @brief Report whether the source container declares an alpha channel.
 *
 * @details The facade always writes RGBA8888, so the destination has an alpha
 * byte whatever the source was; `had_alpha` is the fabric's field for whether
 * that byte carries anything. WebP states it in the header rather than
 * implying it: an extended file (`VP8X`) sets the ALPH feature flag, a
 * lossless file (`VP8L`) sets `alpha_is_used` in its packed word, and a plain
 * lossy `VP8 ` stream has no alpha at all. Guessing `true` because the layout
 * is RGBA would tell a consumer to keep a channel it can drop.
 *
 * @param[in] bytes Encoded image bytes.
 * @param[in] count Length of @p bytes.
 * @return bool True only when the container declares transparency.
 * @pre ::internal_probe has already accepted @p bytes as a WebP.
 * @post No state is mutated.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_had_alpha(const uint8_t* bytes, uint32_t count)
{
  if (count < (uint32_t)k_webpdec_body_ofs) {
    return false;
  }
  const uint8_t* const tag = &bytes[k_webpdec_chunk_ofs];
  if ((tag[0] == (uint8_t)'V') && (tag[1] == (uint8_t)'P') && (tag[2] == (uint8_t)'8')) {
    if ((tag[3] == (uint8_t)'X') && (count >= (uint32_t)k_webpdec_vp8x_min)) {
      return (bytes[k_webpdec_vp8x_flag] & (uint8_t)k_webpdec_alpha_bit) != 0U;
    }
    if ((tag[3] == (uint8_t)'L') && (count >= (uint32_t)k_webpdec_vp8l_min) &&
        (bytes[k_webpdec_body_ofs] == (uint8_t)k_webpdec_l8_sig)) {
      const uint32_t packed = internal_le32(&bytes[k_webpdec_body_ofs + 1U]);
      return ((packed >> (uint32_t)k_webpdec_l8_shift) & 1U) != 0U;
    }
  }
  return false;
}

/**
 * @brief Report this backend's static capabilities.
 *
 * @param[in]  ctx Bound arena (unused; caps are static).
 * @param[out] out Capability record to fill completely.
 * @return ra8_err_t ::k_ra8_ok always.
 * @pre @p out is non-NULL (the fabric guarantees it).
 * @post Every field of `*out` is set.
 * @note Thread-safe (writes only the caller's record).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_caps(void* ctx, ra8_imgdec_caps_t* out)
{
  (void)ctx;
  out->formats       = RA8_WEBP_IMGDEC_FORMATS;
  out->pixels        = RA8_WEBP_IMGDEC_PIXELS;
  out->scratch_bytes = 0U; /* the arena came through the binder, not the request */
  out->scratch_align = 0U;
  out->dim_max       = (uint32_t)k_ra8_webp_max_dim;
  out->streams       = false;
  return k_ra8_ok;
}

/**
 * @brief Decode one WebP into the request's destination surface.
 *
 * @details Verifies the container, pre-flights the geometry and the
 * destination, then calls ra8_webp_decode_rgba() with the arena recorded at
 * ::ra8_webp_imgdec_bind time. The facade resets that arena on entry and
 * drains it before returning on every path, so no handle is left holding the
 * previous decode's store.
 *
 * @param[in]  ctx Bound ::ra8_webp_arena_t.
 * @param[in]  req Validated request.
 * @param[out] out Result record to fill completely on success.
 * @return Result code.
 * @retval k_ra8_ok                    The destination holds the surface.
 * @retval k_ra8_err_invalid_state     No arena was bound to this handle.
 * @retval k_ra8_err_invalid_size      A dimension is past `dim_max`, or the
 *                                     destination cannot hold the surface.
 * @retval k_ra8_err_not_supported     The bytes are not the declared container.
 * @retval k_ra8_err_validation_failed Corrupt body, arena exhausted, or the
 *                                     decoded geometry disagrees with the
 *                                     header.
 * @retval other                       Propagated from ra8_webp_decode_rgba().
 * @pre @p req has passed the fabric's guarantees.
 * @post On k_ra8_ok every field of `*out` is set.
 * @note Not thread-safe: libwebp is single-threaded on this target.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decode(void* ctx, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out)
{
  ra8_webp_arena_t* const arena = (ra8_webp_arena_t*)ctx;
  if (arena == nullptr) {
    return k_ra8_err_invalid_state;
  }

  uint32_t        src_w = 0U;
  uint32_t        src_h = 0U;
  const ra8_err_t geom  = internal_probe(req, &src_w, &src_h);
  if (geom != k_ra8_ok) {
    ra8_log_error(s_tag_webpdec, "decode: header rejected");
    return geom;
  }

  const uint32_t  bpp    = ra8_imgdec_pixel_bytes(req->want);
  const uint32_t  packed = src_w * bpp;
  const uint32_t  stride = (req->dst_stride != 0U) ? req->dst_stride : packed;
  uint32_t        span   = 0U;
  uint32_t        used   = 0U;
  const ra8_err_t fits   = internal_dst_ok(req, src_h, packed, &span, &used);
  if (fits != k_ra8_ok) {
    ra8_log_error(s_tag_webpdec, "decode: destination cannot hold the surface");
    return fits;
  }

  uint32_t        got_w = 0U;
  uint32_t        got_h = 0U;
  const ra8_err_t err   = ra8_webp_decode_rgba(req->bytes,
                                             (size_t)req->byte_count,
                                             arena,
                                             req->dst,
                                             (size_t)stride,
                                             (size_t)span,
                                             &got_w,
                                             &got_h);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag_webpdec, "decode: the facade refused the image");
    return err;
  }

  /* The probe read the header; libwebp read it again on its way through the
   * body. A disagreement means the surface just written is not the one the
   * caller sized, so it is refused rather than reported. */
  if ((got_w != src_w) || (got_h != src_h)) {
    ra8_log_error(s_tag_webpdec, "decode: decoded geometry disagrees with the header");
    return k_ra8_err_validation_failed;
  }

  out->width_px   = src_w;
  out->height_px  = src_h;
  out->stride     = stride;
  out->used_bytes = used;
  out->format     = req->format;
  out->pixel      = req->want;
  out->had_alpha  = internal_had_alpha(req->bytes, req->byte_count);
  return k_ra8_ok;
}

/** @brief The one vtable instance; never per handle. */
static const ra8_imgdec_iface_t s_iface = {
  .get_caps = internal_caps,
  .decode   = internal_decode,
};

/* =============================================================================
 * Public entry point
 * =============================================================================
 */

/** @brief Implementation of `ra8_webp_imgdec_bind()`. */
ra8_err_t ra8_webp_imgdec_bind(ra8_imgdec_t* out, ra8_webp_arena_t* scratch)
{
  RA8_CHECK_NULL_PTR(out, s_tag_webpdec, "bind: null handle");
  RA8_CHECK_NULL_PTR(scratch, s_tag_webpdec, "bind: null scratch");
  out->iface = &s_iface;
  out->ctx   = scratch;
  return k_ra8_ok;
}
