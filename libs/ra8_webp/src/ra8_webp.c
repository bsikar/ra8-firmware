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

  uint32_t w = 0U;
  uint32_t h = 0U;
  RA8_RETURN_ON_ERROR(ra8_webp_get_info(data, size, &w, &h), k_ra8_webp_tag, "info");

  /* The output buffer must hold `height` rows of at least `width * 4` bytes,
   * and the stride must fit libwebp's `int` parameter. */
  const size_t min_stride = (size_t)w * (size_t)k_ra8_webp_bytes_per_px;
  if ((out_stride < min_stride) || (((size_t)h * out_stride) > out_capacity)) {
    ra8_log_error(k_ra8_webp_tag, "output buffer too small");
    return k_ra8_err_range_check_failed;
  }
  if (out_stride > (size_t)INT_MAX) {
    ra8_log_error(k_ra8_webp_tag, "output stride exceeds INT_MAX");
    return k_ra8_err_range_check_failed;
  }

  ra8_webp_arena_bind(arena);
  uint8_t* const result = WebPDecodeRGBAInto(data, size, out_rgba, out_capacity, (int)out_stride);
  ra8_webp_arena_unbind();

  if (result == nullptr) {
    return k_ra8_err_validation_failed;
  }

  /*
   * TODO(#289): render-path integration. This facade decodes a whole WebP into
   * a caller RGBA8888 buffer. Once the #289 band-tile format lands, the
   * normalise-on-import path will slice the decoded canvas into band tiles and
   * route render-time decodes through the single band-tile codec, and the
   * ra8_reflow / ra8_img raster dispatch (libs/ra8_reflow/src/ra8_reflow_image.c)
   * will gain a WebP arm that calls this function. Deliberately NOT wired here
   * (issue #290 is vendor + build + standalone decode only).
   */

  if (out_w != nullptr) {
    *out_w = w;
  }
  if (out_h != nullptr) {
    *out_h = h;
  }
  return k_ra8_ok;
}
