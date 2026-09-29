/**
 * @file ra8_jpeg_imgdec.c
 * @brief `ra8_imgdec` backend over the first-party software JPEG codec (#768).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Compiled only where the `ra8_imgdec` seam is on the include path. A
 * JPEG-only consumer (the camera path, the bench) links `libs/ra8_jpeg`
 * without the seam and gets this translation unit empty, the same guard
 * `comic_tiles.c` uses for its optional `jof` dependency. That is what keeps
 * the binder free of build wiring: no target gains a dependency it did not
 * already have.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#if __has_include("ra8_imgdec_backend.h")

#include "ra8_jpeg_imgdec.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "ra8_jpeg_sw.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_jpeg_imgdec";

/**
 * @enum ra8_jpeg_imgdec_const_t
 * @brief The two numbers this backend publishes about itself.
 */
typedef enum : uint32_t {
  k_ra8_jpeg_imgdec_bpp = 3U, /**< Bytes per pixel ra8_jpeg_sw_decode writes. */
  k_ra8_jpeg_imgdec_dim_max =
    (uint32_t)k_ra8_imgdec_dim_max, /**< Widest/tallest frame accepted. */
} ra8_jpeg_imgdec_const_t;

/* The destination size is computed in 32 bits, so the largest frame this
 * backend admits must fit there with its three bytes per pixel. */
static_assert(((uint64_t)k_ra8_jpeg_imgdec_dim_max * (uint64_t)k_ra8_jpeg_imgdec_dim_max *
               (uint64_t)k_ra8_jpeg_imgdec_bpp) <= (uint64_t)UINT32_MAX,
              "a dim_max frame must not overflow a 32-bit byte count");

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Read the frame's declared geometry and hold it to `dim_max`.
 *
 * @details Split out because the pre-flight is not redundant with the decode
 * that follows it. The fabric hands a backend three duties it cannot discharge
 * without knowing the geometry first: refuse a frame past the advertised
 * `dim_max`, refuse a destination too small for the surface, and refuse a row
 * stride the decoder cannot honour. All three need the size before any pixel
 * is written, and ::ra8_jpeg_sw_get_dimensions is the cheap way to get it --
 * it walks markers and does no entropy decoding.
 *
 * @param[in]  req   Validated request.
 * @param[out] out_w Receives the declared width in pixels.
 * @param[out] out_h Receives the declared height in pixels.
 * @return Result code.
 * @retval k_ra8_ok               Geometry read and within `dim_max`.
 * @retval k_ra8_err_invalid_size A dimension is past `dim_max`.
 * @retval other                  Propagated from the marker walk.
 * @pre @p req has passed the fabric's guarantees.
 * @post On success both outputs are non-zero and within `dim_max`.
 * @note Thread-safe: the marker walk touches no module state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_probe(const ra8_imgdec_req_t* req, uint16_t* out_w, uint16_t* out_h) {
  const ra8_err_t err = ra8_jpeg_sw_get_dimensions(req->bytes, req->byte_count, out_w, out_h);
  if (err != k_ra8_ok) {
    return err;
  }
  if (((uint32_t)*out_w > (uint32_t)k_ra8_jpeg_imgdec_dim_max) ||
      ((uint32_t)*out_h > (uint32_t)k_ra8_jpeg_imgdec_dim_max)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Decide whether the request's destination can take the surface.
 *
 * @details Two refusals, and they are different failures. A stride narrower
 * than one packed row is a destination too small to describe the surface at
 * all. A stride *wider* than one packed row is a padded destination, and
 * ::ra8_jpeg_sw_decode has no stride parameter: it writes rows back to back.
 * Honouring that request is impossible, so it is refused
 * ::k_ra8_err_not_supported rather than silently ignored, which would leave a
 * caller's padded surface holding a sheared image.
 *
 * @param[in] req    Validated request.
 * @param[in] stride Bytes in one packed row of the declared frame.
 * @param[in] need   Bytes the whole packed surface occupies.
 * @return Result code.
 * @retval k_ra8_ok                The destination is packed and large enough.
 * @retval k_ra8_err_invalid_size  Too small, or the stride is under one row.
 * @retval k_ra8_err_not_supported The stride asks for row padding.
 * @pre @p stride and @p need describe the frame ::internal_probe read.
 * @post No state mutated.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_dst_ok(const ra8_imgdec_req_t* req, uint32_t stride, uint32_t need) {
  if ((req->dst_stride != 0U) && (req->dst_stride < stride)) {
    return k_ra8_err_invalid_size;
  }
  if ((req->dst_stride != 0U) && (req->dst_stride > stride)) {
    return k_ra8_err_not_supported; /* the codec writes packed rows only */
  }
  if (req->dst_bytes < need) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Report this backend's static capabilities.
 *
 * @details Baseline JPEG into packed RGB888, no scratch, `dim_max` at the
 * fabric limit. The codec's own frame ceiling is the 16-bit SOF field, wider
 * than the seam admits, so the seam limit is the binding one and is published
 * as such rather than a number this file invents.
 *
 * @param[in]  ctx Unused; the codec keeps its state in module statics.
 * @param[out] out Capability record, filled whole.
 * @return Result code.
 * @retval k_ra8_ok           `*out` was filled.
 * @retval k_ra8_err_null_ptr @p out was NULL.
 * @pre @p out is writable.
 * @post On success every field of `*out` is set.
 * @note Thread-safe (writes only @p out).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_caps(void* ctx, ra8_imgdec_caps_t* out) {
  (void)ctx;
  RA8_CHECK_NULL_PTR(out, s_tag, "caps: null out");
  out->formats       = (uint32_t)k_ra8_imgdec_format_jpeg;
  out->pixels        = (uint32_t)k_ra8_imgdec_pixel_rgb888;
  out->scratch_bytes = 0U;
  out->scratch_align = 0U;
  out->dim_max       = (uint32_t)k_ra8_jpeg_imgdec_dim_max;
  out->streams       = false; /* the whole frame must be resident */
  return k_ra8_ok;
}

/**
 * @brief Decode one baseline JPEG into the request's packed RGB888 surface.
 *
 * @details The fabric has already proved the pointers, the non-zero counts,
 * that `want` is the one layout this backend advertised and that `format` is
 * ::k_ra8_imgdec_format_jpeg. What is left is this module's own: read the
 * geometry, hold it to `dim_max`, prove the destination, decode, and describe
 * what was written.
 *
 * The geometry reported in `*out` is the decoder's own, not the pre-flight's.
 * Both read the same SOF field so they cannot disagree, and the decoder's pair
 * is the one that describes the bytes now sitting in `dst`.
 *
 * @param[in]  ctx Unused; the codec keeps its state in module statics.
 * @param[in]  req Validated request (see `ra8_imgdec_backend.h`).
 * @param[out] out Result record, filled whole on success.
 * @return Result code.
 * @retval k_ra8_ok                `req->dst` holds the decoded surface.
 * @retval k_ra8_err_invalid_size  Frame past `dim_max`, or `dst` too small.
 * @retval k_ra8_err_not_supported Padded stride, or a non-baseline frame.
 * @retval other                   Propagated from the codec.
 * @pre The fabric's documented guarantees hold for @p req.
 * @post On success every field of `*out` describes the written surface.
 * @post On failure @p out is left to the fabric, which clears it.
 * @note Not thread-safe: the codec's decode state is module-static.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decode(void* ctx, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out) {
  (void)ctx;
  uint16_t        w     = 0U;
  uint16_t        h     = 0U;
  const ra8_err_t probe = internal_probe(req, &w, &h);
  if (probe != k_ra8_ok) {
    return probe;
  }

  const uint32_t  stride = (uint32_t)w * (uint32_t)k_ra8_jpeg_imgdec_bpp;
  const uint32_t  need   = stride * (uint32_t)h;
  const ra8_err_t fits   = internal_dst_ok(req, stride, need);
  if (fits != k_ra8_ok) {
    return fits;
  }

  uint16_t        dw  = 0U;
  uint16_t        dh  = 0U;
  const ra8_err_t err = ra8_jpeg_sw_decode(req->bytes, req->byte_count, req->dst,
                                           req->dst_bytes, &dw, &dh);
  if (err != k_ra8_ok) {
    return err;
  }

  out->width_px   = (uint32_t)dw;
  out->height_px  = (uint32_t)dh;
  out->stride     = (uint32_t)dw * (uint32_t)k_ra8_jpeg_imgdec_bpp;
  out->used_bytes = out->stride * (uint32_t)dh;
  out->format     = k_ra8_imgdec_format_jpeg;
  out->pixel      = k_ra8_imgdec_pixel_rgb888;
  out->had_alpha  = false; /* JPEG carries no alpha channel */
  return k_ra8_ok;
}

/** @brief The one vtable instance; the handle carries no state of its own. */
static const ra8_imgdec_iface_t s_iface = {
  .get_caps = internal_caps,
  .decode   = internal_decode,
};

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_jpeg_imgdec_bind(ra8_imgdec_t* out) {
  RA8_CHECK_NULL_PTR(out, s_tag, "bind: null out");
  out->iface = &s_iface;
  out->ctx   = nullptr;
  return k_ra8_ok;
}

#endif /* __has_include("ra8_imgdec_backend.h") */
