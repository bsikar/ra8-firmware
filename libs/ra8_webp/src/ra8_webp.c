/**
 * @file ra8_webp.c
 * @brief Zero-heap WebP decode facade over the vendored libwebp (impl).
 *
 * @details
 * See ra8_webp.h for the contract. This TU is the only first-party code that
 * includes libwebp's public `src/webp/decode.h`; every consumer reaches WebP
 * through the `ra8_err_t` facade here, never through libwebp directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / WebP] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_webp.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_webp_arena.h"
#include "src/webp/decode.h" /* WebPGetInfo, WebPDecodeRGBAInto (vendored libwebp) */

/** Component tag used in validation log lines. */
static const char k_ra8_webp_tag[] = "WEBP";

ra8_err_t ra8_webp_get_info(const uint8_t* data, size_t size, uint32_t* out_w, uint32_t* out_h)
{
  RA8_CHECK_NULL_PTR(data, k_ra8_webp_tag, "data");
  RA8_CHECK_NULL_PTR(out_w, k_ra8_webp_tag, "out_w");
  RA8_CHECK_NULL_PTR(out_h, k_ra8_webp_tag, "out_h");
  if (size == 0U) {
    ra8_log_error(k_ra8_webp_tag, "empty WebP buffer");
    return k_ra8_err_invalid_arg;
  }

  int w = 0;
  int h = 0;
  /* WebPGetInfo returns 0 for any invalid container or a zero/oversized
   * dimension, so on success both w and h are already in [1, WEBP_MAX_DIMENSION]
   * -- no separate non-positive check is needed (it would be dead code). */
  if (WebPGetInfo(data, size, &w, &h) == 0) {
    return k_ra8_err_validation_failed;
  }
  if (((uint32_t)w > k_ra8_webp_max_dim) || ((uint32_t)h > k_ra8_webp_max_dim)) {
    ra8_log_error(k_ra8_webp_tag, "WebP dimension exceeds cap");
    return k_ra8_err_not_supported;
  }

  *out_w = (uint32_t)w;
  *out_h = (uint32_t)h;
  return k_ra8_ok;
}

/**
 * @brief Read the WebP header and validate the caller's output geometry.
 *
 * @details Fetches the image dimensions via `ra8_webp_get_info()`, then checks
 *          that the output buffer holds `height` rows of at least `width * 4`
 *          bytes and that the stride fits libwebp's `int` parameter. Extracted
 *          from ra8_webp_decode_rgba() so the public entry stays within the
 *          NASA Rule 4 function-size budget.
 *
 * @param[in]  data         WebP byte buffer base.
 * @param[in]  size         Buffer length in bytes.
 * @param[in]  out_stride   Destination row stride in bytes.
 * @param[in]  out_capacity Destination buffer capacity in bytes.
 * @param[out] w            Decoded image width on success.
 * @param[out] h            Decoded image height on success.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Header valid and the output geometry fits.
 * @retval k_ra8_err_validation_failed Malformed WebP container.
 * @retval k_ra8_err_not_supported Dimension exceeds the decode cap.
 * @retval k_ra8_err_range_check_failed Output buffer too small, or stride > INT_MAX.
 *
 * @pre @p data, @p w and @p h are non-NULL (caller-guaranteed).
 * @pre @p size describes the readable extent of @p data.
 * @post On success @p w and @p h hold the image dimensions.
 * @post On failure @p w and @p h are not relied upon.
 *
 * @note Re-entrant; reads only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_webp_check_output(const uint8_t* data,
                                            size_t         size,
                                            size_t         out_stride,
                                            size_t         out_capacity,
                                            uint32_t*      w,
                                            uint32_t*      h)
{
  RA8_RETURN_ON_ERROR(ra8_webp_get_info(data, size, w, h), k_ra8_webp_tag, "info");
  const size_t min_stride = (size_t)*w * (size_t)k_ra8_webp_bytes_per_px;
  if ((out_stride < min_stride) || (((size_t)*h * out_stride) > out_capacity)) {
    ra8_log_error(k_ra8_webp_tag, "output buffer too small");
    return k_ra8_err_range_check_failed;
  }
  if (out_stride > (size_t)INT_MAX) {
    ra8_log_error(k_ra8_webp_tag, "output stride exceeds INT_MAX");
    return k_ra8_err_range_check_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate geometry, decode the WebP into @p out_rgba, and report size.
 *
 * @details Runs the decode pipeline after the public entry has null-checked its
 *          pointers: output-geometry validation, arena-bound
 *          `WebPDecodeRGBAInto()`, then the optional width/height reporting.
 *          Extracted from ra8_webp_decode_rgba() so the public entry stays
 *          within the NASA Rule 4 function-size budget.
 *
 * @param[in]  data         WebP byte buffer base (non-NULL, caller-checked).
 * @param[in]  size         Buffer length in bytes.
 * @param[in]  arena        Bump allocator libwebp draws its scratch from (non-NULL).
 * @param[out] out_rgba     Destination RGBA8888 buffer (non-NULL).
 * @param[in]  out_stride   Destination row stride in bytes.
 * @param[in]  out_capacity Destination buffer capacity in bytes.
 * @param[out] out_w        Decoded width, or NULL if unwanted.
 * @param[out] out_h        Decoded height, or NULL if unwanted.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Decode succeeded; @p out_rgba filled, sizes reported.
 * @retval k_ra8_err_validation_failed Malformed WebP or decode failure.
 * @retval k_ra8_err_not_supported Dimension exceeds the decode cap.
 * @retval k_ra8_err_range_check_failed Output buffer too small, or stride > INT_MAX.
 *
 * @pre @p data, @p arena and @p out_rgba are non-NULL (caller-guaranteed).
 * @pre @p out_capacity describes the writable extent of @p out_rgba.
 * @post On success @p out_rgba holds the decoded image.
 * @post On failure @p out_w and @p out_h are not written.
 *
 * @note Not re-entrant: binds the shared decode arena for the call's duration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_webp_decode_impl(const uint8_t*    data,
                                           size_t            size,
                                           ra8_webp_arena_t* arena,
                                           uint8_t*          out_rgba,
                                           size_t            out_stride,
                                           size_t            out_capacity,
                                           uint32_t*         out_w,
                                           uint32_t*         out_h)
{
  uint32_t w = 0U;
  uint32_t h = 0U;
  RA8_RETURN_ON_ERROR(internal_webp_check_output(data, size, out_stride, out_capacity, &w, &h),
                      k_ra8_webp_tag,
                      "validate");

  ra8_webp_arena_bind(arena);
  uint8_t* const result = WebPDecodeRGBAInto(data, size, out_rgba, out_capacity, (int)out_stride);
  ra8_webp_arena_unbind();
  if (result == nullptr) {
    return k_ra8_err_validation_failed;
  }

  /*
   * This facade decodes a whole WebP into a caller RGBA8888 buffer. The #290
   * normalize-on-import path consumes it in the JOF tile producer
   * (libs/ra8_tileatlas/src/ra8_tileatlas_produce.c: priv_webp_transcode), which
   * bands the decoded canvas into JOF tiles so render time touches one codec
   * regardless of source. TODO(#289): the ra8_reflow / ra8_img inline raster
   * dispatch (libs/ra8_reflow/src/ra8_reflow_image.c) does not yet have a WebP
   * arm for the small-image (non-tiled) path -- that lands with the longstrip
   * render work; large WebP pages already normalize through the producer above.
   */

  if (out_w != nullptr) {
    *out_w = w;
  }
  if (out_h != nullptr) {
    *out_h = h;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_webp_decode_rgba(const uint8_t*    data,
                               size_t            size,
                               ra8_webp_arena_t* arena,
                               uint8_t*          out_rgba,
                               size_t            out_stride,
                               size_t            out_capacity,
                               uint32_t*         out_w,
                               uint32_t*         out_h)
{
  RA8_CHECK_NULL_PTR(data, k_ra8_webp_tag, "data");
  RA8_CHECK_NULL_PTR(arena, k_ra8_webp_tag, "arena");
  RA8_CHECK_NULL_PTR(out_rgba, k_ra8_webp_tag, "out_rgba");
  return internal_webp_decode_impl(data,
                                   size,
                                   arena,
                                   out_rgba,
                                   out_stride,
                                   out_capacity,
                                   out_w,
                                   out_h);
}
