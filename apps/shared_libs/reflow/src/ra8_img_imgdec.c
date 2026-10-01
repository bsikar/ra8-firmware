/**
 * @file ra8_img_imgdec.c
 * @brief `ra8_imgdec` backend over the vendored stb_image residue (RA8FW-308).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * The vtable, the caps record and the decode hook for ::ra8_img_imgdec_bind.
 * The hook is a thin adapter: it re-checks the container it was told it holds,
 * pre-flights the geometry, runs the existing `stbi_load_from_memory()` call
 * against the bound arena, and copies the result into the caller's surface.
 * No decoding logic is added or duplicated here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_img_imgdec.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_img_arena.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "ra8_log.h"
#include "stb_image.h"

/** @brief Module log tag. */
static const char* const s_tag_stbdec = "ra8_img_imgdec";

/**
 * @enum ra8_img_imgdec_const_t
 * @brief The numbers this backend publishes about itself.
 */
typedef enum : uint32_t {
  k_stbdec_dim_max = 8192U, /* STBI_MAX_DIMENSIONS in stb_image_impl.c. */
  k_stbdec_alpha_g = 2U,    /* stb channel count: grey + alpha.         */
  k_stbdec_alpha_c = 4U,    /* stb channel count: RGB + alpha.          */
} ra8_img_imgdec_const_t;

/** @brief Formats this backend advertises. See the header for why PNG is here. */
#define RA8_IMG_IMGDEC_FORMATS                                                  \
  ((uint32_t)k_ra8_imgdec_format_png | (uint32_t)k_ra8_imgdec_format_gif | \
   (uint32_t)k_ra8_imgdec_format_bmp)

/** @brief Destination layouts stb can be asked for directly. */
#define RA8_IMG_IMGDEC_PIXELS                                                       \
  ((uint32_t)k_ra8_imgdec_pixel_grey8 | (uint32_t)k_ra8_imgdec_pixel_rgb888 | \
   (uint32_t)k_ra8_imgdec_pixel_rgba8888)

/* The whole surface is measured in 32 bits, so the widest frame this backend
 * admits must fit there at its fattest layout. */
static_assert(((uint64_t)k_stbdec_dim_max * (uint64_t)k_stbdec_dim_max * 4U) <= (uint64_t)UINT32_MAX,
              "a dim_max surface must not overflow a 32-bit byte count");

/* stb_image takes its input length as an int, so the request's byte count has
 * to be refused above INT_MAX rather than truncated into a short read. */
static_assert(sizeof(int) >= 4, "stb_image's int length must hold a real image");

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Verify the container and read its declared geometry, in one pass.
 *
 * @details Both duties the fabric leaves to a backend are answered by
 * ::ra8_imgdec_dims, so this asks it once rather than sniffing and then
 * probing. The container check is not ceremony: `stb_image` decodes JPEG as
 * happily as the three formats above, so bytes that are secretly a JPEG would
 * otherwise decode fine through a handle that does not advertise it, and a
 * consumer routing by format would silently get the wrong decoder.
 *
 * Reading the header first also keeps a refusal cheap. Every reason to say no
 * (wrong container, a dimension past `dim_max`, a destination too small) is
 * decided before a single pixel is bumped out of the arena.
 *
 * `stbi_info_from_memory()` is deliberately NOT the probe here: it reports
 * failure on a GIF this backend decodes without complaint, so pre-flighting
 * through it would refuse a whole advertised format.
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
  if ((geom.width_px > (uint32_t)k_stbdec_dim_max) ||
      (geom.height_px > (uint32_t)k_stbdec_dim_max)) {
    return k_ra8_err_invalid_size;
  }
  *out_w = geom.width_px;
  *out_h = geom.height_px;
  return k_ra8_ok;
}

/**
 * @brief Decide whether the request's destination can take the surface.
 *
 * @details Unlike the first-party JPEG backend, this one is free to honour a
 * padded `dst_stride`: it copies row by row out of stb's own buffer, so a
 * wider stride costs an offset per row and nothing else. Only a stride
 * narrower than one packed row is impossible to satisfy.
 *
 * @param[in]  req      Validated request.
 * @param[in]  rows     Declared height in pixels.
 * @param[in]  packed   Bytes in one packed row of the declared frame.
 * @param[out] out_need Receives the bytes the destination must hold.
 * @return Result code.
 * @retval k_ra8_ok               The destination is large enough.
 * @retval k_ra8_err_invalid_size Too small, or the stride is under one row.
 * @pre @p rows and @p packed describe the frame ::internal_geometry read.
 * @post On success `*out_need` is the exact span the copy will touch.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_dst_ok(const ra8_imgdec_req_t* req, uint32_t rows, uint32_t packed, uint32_t* out_need)
{
  const uint32_t stride = (req->dst_stride != 0U) ? req->dst_stride : packed;
  if (stride < packed) {
    return k_ra8_err_invalid_size;
  }
  /* The last row occupies a packed row, not a strided one: a destination sized
   * exactly to the image must not be refused over padding it never uses. */
  const uint64_t need = ((uint64_t)(rows - 1U) * (uint64_t)stride) + (uint64_t)packed;
  if (need > (uint64_t)req->dst_bytes) {
    return k_ra8_err_invalid_size;
  }
  *out_need = (uint32_t)need;
  return k_ra8_ok;
}

/**
 * @brief Copy stb's packed surface into the caller's (possibly padded) one.
 *
 * @param[in]  pixels Decoded surface, `rows` packed rows of @p packed bytes.
 * @param[out] dst    Destination surface.
 * @param[in]  stride Destination row stride in bytes (>= @p packed).
 * @param[in]  rows   Row count.
 * @param[in]  packed Bytes in one packed row.
 * @return None.
 * @pre The destination holds `(rows - 1) * stride + packed` writable bytes.
 * @post Every row of @p pixels is written at its destination offset.
 * @note Thread-safe (touches only the two buffers).
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_copy_rows(const uint8_t* pixels, uint8_t* dst, uint32_t stride, uint32_t rows, uint32_t packed)
{
  for (uint32_t y = 0U; y < rows; ++y) {
    (void)memcpy(&dst[(size_t)y * (size_t)stride], &pixels[(size_t)y * (size_t)packed], packed);
  }
}

/**
 * @brief Map a failed stb decode to the closest ra8_err_t.
 *
 * @details `stbi_load_from_memory()` returns one null pointer for two quite
 * different events: the arena ran out mid-decode, or the body is corrupt. The
 * reason string is the only thing that tells them apart, and the distinction
 * matters to a consumer: a shortage is fixed by handing the binder a bigger
 * arena, a corrupt body never is. Same classification `ra8_img_decode_blit()`
 * has always done, applied at the seam instead of inside the blit.
 *
 * @return Result code.
 * @retval k_ra8_err_no_mem        The reason carries the "outofmem" tag.
 * @retval k_ra8_err_not_supported Any other reason, or none at all.
 * @pre `stbi_load_from_memory()` has just returned NULL.
 * @post No stb state mutated; the reason string is only read.
 * @note Not thread-safe: stb keeps the reason in module state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode_fail(void)
{
  const char* const reason = stbi_failure_reason();
  /* mcdc-deactivated: stbi sets a reason on every failure, so (reason != nullptr) is always true here. */
  if ((reason != nullptr) && (strstr(reason, "outofmem") != nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_err_not_supported;
}

/**
 * @brief Unbind the arena and return it to empty.
 *
 * @details Called on every path out of the decode once the arena has been
 * bound, so no handle is left pointing at the previous decode's store and no
 * stray allocation can reach a stale buffer.
 *
 * @param[in,out] arena Arena to release. Must not be NULL.
 * @return None.
 * @pre @p arena is the currently bound arena.
 * @post Nothing is bound and @p arena is empty.
 * @note Not thread-safe: the bound slot is module state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_arena_release(ra8_img_arena_t* arena)
{
  ra8_img_arena_unbind();
  arena->offset = 0U;
  arena->live   = 0U;
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
  out->formats       = RA8_IMG_IMGDEC_FORMATS;
  out->pixels        = RA8_IMG_IMGDEC_PIXELS;
  out->scratch_bytes = 0U; /* the arena came through the binder, not the request */
  out->scratch_align = 0U;
  out->dim_max       = (uint32_t)k_stbdec_dim_max;
  out->streams       = false;
  return k_ra8_ok;
}

/**
 * @brief Decode one PNG, GIF or BMP into the request's destination surface.
 *
 * @details Verifies the container, pre-flights the geometry and the
 * destination, then binds the arena recorded at ::ra8_img_imgdec_bind time and
 * calls `stbi_load_from_memory()` asking for exactly the layout `req->want`
 * names. stb's buffer is copied into `req->dst` and the arena is drained on
 * every return path, success or failure, so a handle is never left holding the
 * previous decode's store.
 *
 * @param[in]  ctx Bound ::ra8_img_arena_t.
 * @param[in]  req Validated request.
 * @param[out] out Result record to fill completely on success.
 * @return Result code.
 * @retval k_ra8_ok                The destination holds the decoded surface.
 * @retval k_ra8_err_invalid_state No arena was bound to this handle.
 * @retval k_ra8_err_invalid_size  Byte count past INT_MAX, dimension past
 *                                 `dim_max`, or a destination too small.
 * @retval k_ra8_err_not_supported The bytes are not the declared container, or
 *                                 stb refused them.
 * @retval k_ra8_err_validation_failed stb's geometry disagrees with the header.
 * @retval k_ra8_err_no_mem        The arena cannot hold this decode.
 * @pre @p req has passed the fabric's guarantees.
 * @post On any return the bound arena is empty and unbound.
 * @note Not thread-safe: the stb hooks reach one file-static arena slot.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decode(void* ctx, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out)
{
  ra8_img_arena_t* const arena = (ra8_img_arena_t*)ctx;
  if (arena == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (req->byte_count > (uint32_t)INT32_MAX) {
    return k_ra8_err_invalid_size;
  }

  uint32_t        src_w = 0U;
  uint32_t        src_h = 0U;
  const ra8_err_t geom  = internal_probe(req, &src_w, &src_h);
  if (geom != k_ra8_ok) {
    ra8_log_error(s_tag_stbdec, "decode: header rejected");
    return geom;
  }

  const uint32_t bpp    = ra8_imgdec_pixel_bytes(req->want);
  const uint32_t packed = src_w * bpp;
  uint32_t       need   = 0U;
  const ra8_err_t fits  = internal_dst_ok(req, src_h, packed, &need);
  if (fits != k_ra8_ok) {
    ra8_log_error(s_tag_stbdec, "decode: destination cannot hold the surface");
    return fits;
  }

  int sx   = 0;
  int sy   = 0;
  int comp = 0;
  ra8_img_arena_bind(arena); /* resets the arena to empty */
  /* The stb call stays on one line so the no-alloc audit
     (scripts/checks/check_no_dynamic_alloc.py) finds its opt-out on the flagged
     call line; clang-format would otherwise wrap it across many lines. */
  // clang-format off: the allocation opt-out comment must stay on the flagged call line.
  uint8_t* const pixels = stbi_load_from_memory(req->bytes, (int)req->byte_count, &sx, &sy, &comp, (int)bpp); /* alloc-allow: stb is backed by the fixed ra8_img_arena (zero-heap), not malloc */
  // clang-format on
  if (pixels == nullptr) {
    const ra8_err_t why = internal_decode_fail();
    internal_arena_release(arena);
    ra8_log_error(s_tag_stbdec, "decode: stb refused the image");
    return why;
  }

  /* The probe read the header; stb read it again on its way through the body.
   * They must agree, and a disagreement means the copy below would run off the
   * end of one buffer or the other, so it is refused rather than clamped. */
  if ((sx != (int)src_w) || (sy != (int)src_h)) {
    internal_arena_release(arena);
    ra8_log_error(s_tag_stbdec, "decode: decoded geometry disagrees with the header");
    return k_ra8_err_validation_failed;
  }

  const uint32_t stride = (req->dst_stride != 0U) ? req->dst_stride : packed;
  internal_copy_rows(pixels, req->dst, stride, src_h, packed);

  // clang-format off: the allocation opt-out comment must stay on the flagged call line.
  stbi_image_free(pixels); /* alloc-allow: ra8_img_arena-backed (zero-heap), not malloc */
  // clang-format on
  internal_arena_release(arena);

  out->width_px   = src_w;
  out->height_px  = src_h;
  out->stride     = stride;
  out->used_bytes = need;
  out->format     = req->format;
  out->pixel      = req->want;
  out->had_alpha =
    (comp == (int)k_stbdec_alpha_g) || (comp == (int)k_stbdec_alpha_c);
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

/** @brief Implementation of `ra8_img_imgdec_bind()`. */
ra8_err_t ra8_img_imgdec_bind(ra8_imgdec_t* out, ra8_img_arena_t* scratch)
{
  RA8_CHECK_NULL_PTR(out, s_tag_stbdec, "bind: null handle");
  RA8_CHECK_NULL_PTR(scratch, s_tag_stbdec, "bind: null scratch");
  out->iface = &s_iface;
  out->ctx   = scratch;
  return k_ra8_ok;
}
