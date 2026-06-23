/**
 * @file ra_reflow_image.c
 * @brief Zero-heap raster image decode + nearest-neighbour scale + blit (#106).
 *
 * @details
 * Implements ra_reflow_image.h. The decode runs through the vendored stb_image
 * (built once in libs/third_party/stb/stb_image_impl.c) with its allocator
 * redirected to a caller-bound bump arena, so no `malloc` is reached. The blit
 * is an integer nearest-neighbour scale-to-fit into a layout box, emitting one
 * `ra_gfx_pixel()` per destination pixel (which clips to the framebuffer).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_reflow_image.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_img_arena.h"
#include "ra_log.h"
#include "ra_reflow_svg.h"
#include "stb_image.h"

/** @brief Log tag for the image decode/blit module. */
static const char* const s_tag_img = "ra_img";

/**
 * @enum ra_img_pack_t
 * @brief Pixel-packing and channel constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra_img_req_rgb  = 3, /**< Desired channel count requested from stb_image. */
  k_ra_img_ch_r     = 0, /**< Red byte offset within an RGB triple.          */
  k_ra_img_ch_g     = 1, /**< Green byte offset within an RGB triple.        */
  k_ra_img_ch_b     = 2, /**< Blue byte offset within an RGB triple.         */
  k_ra_img_min_edge = 1, /**< Minimum scaled / box edge length, pixels.      */
} ra_img_pack_t;

/**
 * @enum ra_img_shift_t
 * @brief Channel shifts to assemble a 0x00RRGGBB colour for ra_gfx (no magics).
 */
typedef enum : uint8_t {
  k_ra_img_shift_r = 16, /**< Red channel shift into 0x00RRGGBB.   */
  k_ra_img_shift_g = 8,  /**< Green channel shift into 0x00RRGGBB. */
} ra_img_shift_t;

ra_err_t ra_img_probe_size(const uint8_t* bytes, size_t len, int32_t* out_w, int32_t* out_h)
{
  RA_CHECK_NULL_PTR(bytes, s_tag_img, "probe: null bytes");
  RA_CHECK_NULL_PTR(out_w, s_tag_img, "probe: null out_w");
  RA_CHECK_NULL_PTR(out_h, s_tag_img, "probe: null out_h");

  /* SVG is not a raster: take its intrinsic size from the document (#112). */
  if (ra_svg_is_svg(bytes, len)) {
    return ra_svg_size(bytes, len, out_w, out_h);
  }

  int x    = 0;
  int y    = 0;
  int comp = 0;
  if (stbi_info_from_memory(bytes, (int)len, &x, &y, &comp) == 0) {
    ra_log_error(s_tag_img, "probe: stbi_info rejected the header");
    return k_ra_err_not_supported;
  }
  *out_w = (int32_t)x;
  *out_h = (int32_t)y;
  return k_ra_ok;
}

/**
 * @brief Compute the aspect-preserving fit rectangle for an image in a box.
 *
 * @details Picks the larger integer scale `s` with `src_w*s <= box_w` and
 * `src_h*s <= box_h`, then returns the scaled extents (each clamped to >= 1).
 * Internal helper for ra_img_decode_blit(); int64 products avoid overflow.
 *
 * @param[in]  src_w Source width, pixels (>= 1).
 * @param[in]  src_h Source height, pixels (>= 1).
 * @param[in]  box_w Box width, pixels (>= 1).
 * @param[in]  box_h Box height, pixels (>= 1).
 * @param[out] fit_w Receives the scaled width, pixels.
 * @param[out] fit_h Receives the scaled height, pixels.
 */
static void internal_fit_box(int32_t  src_w,
                             int32_t  src_h,
                             int32_t  box_w,
                             int32_t  box_h,
                             int32_t* fit_w,
                             int32_t* fit_h)
{
  int32_t sw = 0;
  int32_t sh = 0;
  if (((int64_t)box_w * (int64_t)src_h) <= ((int64_t)box_h * (int64_t)src_w)) {
    /* Width is the tighter constraint: fill the box width. */
    sw = box_w;
    sh = (int32_t)(((int64_t)src_h * (int64_t)box_w) / (int64_t)src_w);
  } else {
    /* Height is the tighter constraint: fill the box height. */
    sh = box_h;
    sw = (int32_t)(((int64_t)src_w * (int64_t)box_h) / (int64_t)src_h);
  }
  *fit_w = (sw < k_ra_img_min_edge) ? (int32_t)k_ra_img_min_edge : sw;
  *fit_h = (sh < k_ra_img_min_edge) ? (int32_t)k_ra_img_min_edge : sh;
}

/**
 * @brief Map a decode failure to the closest ra_err_t via stbi_failure_reason.
 *
 * @details stb_image reports out-of-memory with the tag "outofmem"; treat that
 * as ::k_ra_err_no_mem (the arena was too small) and any other failure as
 * ::k_ra_err_not_supported (unrecognised / corrupt bytes).
 *
 * @return ::k_ra_err_no_mem or ::k_ra_err_not_supported.
 */
static ra_err_t internal_decode_fail(void)
{
  const char* const reason = stbi_failure_reason();
  if ((reason != nullptr) && (strstr(reason, "outofmem") != nullptr)) {
    return k_ra_err_no_mem;
  }
  return k_ra_err_not_supported;
}

/**
 * @brief Nearest-neighbour blit a decoded RGB image into the bound framebuffer.
 *
 * @details For each destination pixel in the `fit_w x fit_h` rectangle, samples
 * the source pixel at the proportional position and emits it via ra_gfx_pixel()
 * (which clips to the framebuffer). Internal helper for ra_img_decode_blit().
 *
 * @param[in] pixels Decoded source, RGB triples, row-major `src_w x src_h`.
 * @param[in] src_w  Source width, pixels (>= 1).
 * @param[in] src_h  Source height, pixels (>= 1).
 * @param[in] fit_w  Destination width, pixels (>= 1).
 * @param[in] fit_h  Destination height, pixels (>= 1).
 * @param[in] dst_x  Destination left edge.
 * @param[in] dst_y  Destination top edge.
 */
static void internal_blit_scaled(const uint8_t* pixels,
                                 int32_t        src_w,
                                 int32_t        src_h,
                                 int32_t        fit_w,
                                 int32_t        fit_h,
                                 int32_t        dst_x,
                                 int32_t        dst_y)
{
  for (int32_t dy = 0; dy < fit_h; dy++) {
    const int32_t map_y = (int32_t)(((int64_t)dy * (int64_t)src_h) / (int64_t)fit_h);
    for (int32_t dx = 0; dx < fit_w; dx++) {
      const int32_t map_x = (int32_t)(((int64_t)dx * (int64_t)src_w) / (int64_t)fit_w);
      const size_t  idx =
        (((size_t)map_y * (size_t)src_w) + (size_t)map_x) * (size_t)k_ra_img_req_rgb;
      const uint32_t color = ((uint32_t)pixels[idx + (size_t)k_ra_img_ch_r] << k_ra_img_shift_r) |
                             ((uint32_t)pixels[idx + (size_t)k_ra_img_ch_g] << k_ra_img_shift_g) |
                             (uint32_t)pixels[idx + (size_t)k_ra_img_ch_b];
      (void)ra_gfx_pixel(dst_x + dx, dst_y + dy, color);
    }
  }
}

/** @brief Unbind the decode arena and force it back to fully drained. */
static void internal_arena_release(ra_img_arena_t* arena)
{
  ra_img_arena_unbind();
  arena->offset = 0U;
  arena->live   = 0U;
}

/** @brief Implementation of `ra_img_decode_blit()` -- nearest-neighbour scale. */
ra_err_t ra_img_decode_blit(ra_img_arena_t* arena,
                            const uint8_t*  bytes,
                            size_t          len,
                            int32_t         dst_x,
                            int32_t         dst_y,
                            int32_t         box_w,
                            int32_t         box_h,
                            int32_t*        out_w,
                            int32_t*        out_h)
{
  RA_CHECK_NULL_PTR(arena, s_tag_img, "blit: null arena");
  RA_CHECK_NULL_PTR(bytes, s_tag_img, "blit: null bytes");
  if ((len == 0U) || (box_w < (int32_t)k_ra_img_min_edge) || (box_h < (int32_t)k_ra_img_min_edge)) {
    ra_log_error(s_tag_img, "blit: empty input or box");
    return k_ra_err_invalid_arg;
  }

  ra_img_arena_bind(arena); /* resets the arena to empty */
  int sx   = 0;
  int sy   = 0;
  int comp = 0;
  /* The stb call stays on one line so the no-alloc audit
     (scripts/utils/check_no_dynamic_alloc.py) finds its opt-out on the flagged
     call line; clang-format would otherwise wrap it across many lines. */
  /* clang-format off */
  uint8_t* const pixels = stbi_load_from_memory(bytes, (int)len, &sx, &sy, &comp, (int)k_ra_img_req_rgb); /* alloc-allow: stb is backed by the fixed ra_img_arena (zero-heap), not malloc */
  /* clang-format on */
  if ((pixels == nullptr) || (sx <= 0) || (sy <= 0)) {
    const ra_err_t err = internal_decode_fail();
    internal_arena_release(arena);
    ra_log_error(s_tag_img, "blit: decode failed");
    return err;
  }

  int32_t fit_w = 0;
  int32_t fit_h = 0;
  internal_fit_box((int32_t)sx, (int32_t)sy, box_w, box_h, &fit_w, &fit_h);
  internal_blit_scaled(pixels, (int32_t)sx, (int32_t)sy, fit_w, fit_h, dst_x, dst_y);
  /* clang-format off */
  stbi_image_free(pixels); /* drains the arena (live -> 0 -> offset reset) -- alloc-allow: stb is backed by the fixed ra_img_arena (zero-heap), not malloc */
  /* clang-format on */
  internal_arena_release(arena);

  if (out_w != nullptr) {
    *out_w = fit_w;
  }
  if (out_h != nullptr) {
    *out_h = fit_h;
  }
  return k_ra_ok;
}
