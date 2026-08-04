/**
 * @file ra8_gfx_blit_gray4.c
 * @brief Sub-rectangle nearest-neighbour integer-zoom blit for packed gray4 images.
 *
 * @details
 * Implements ra8_gfx_blit_gray4_zoom(): the one primitive the reader loupe (#211)
 * needs that the scale-to-fit blitters deliberately lack -- a source-rect ->
 * dest-rect copy that magnifies (or reproduces 1:1) without decimating source
 * pixels. It shares the single ra8_gfx framebuffer binding and the clipped
 * per-pixel plotter (s_gfx_text_plot) with the rest of the rasteriser, so a lens
 * window is clipped to the active clip rectangle exactly like every other draw.
 *
 * The source pixel format is the reader's native 4-bit grayscale: two pixels per
 * byte at flat nibble index `y * src_w + x`, even index in the high nibble, odd
 * index in the low nibble -- the same packing ra8_gfx_blit_gray8() consumers unpack
 * a row at a time out of the demand-paged image pool.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_gfx.h"
#include "ra8_gfx_internal.h"

/**
 * @enum ra8_gfx_g4_const_t
 * @brief Nibble-unpack + gray-to-colour expansion constants for the gray4 blit.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_g4_nib_lo   = 0x0FU, /**< Low-nibble mask.                           */
  k_ra8_g4_nib_sh   = 4U,    /**< Nibble shift (and the 4->8-bit replicate). */
  k_ra8_g4_rgb_g_sh = 8U,    /**< Green shift of the 0x00RRGGBB gray expand. */
  k_ra8_g4_rgb_r_sh = 16U,   /**< Red shift of the 0x00RRGGBB gray expand.   */
} ra8_gfx_g4_const_t;

/**
 * @brief Expand the packed gray4 pixel at (@p x, @p y) to a 0x00RRGGBB gray colour.
 *
 * @details Reads the byte holding flat nibble index `y * src_w + x`, selects the
 * high nibble for an even index and the low nibble for an odd one, replicates the
 * 4-bit level into 8 bits, then broadcasts it across R, G and B -- the identical
 * gray level ra8_gfx_blit_gray8() would produce for the same nibble.
 *
 * @param[in] src   Packed gray4 base pointer (two pixels per byte).
 * @param[in] src_w Source width in pixels (nibble stride); caller-guaranteed > 0.
 * @param[in] x     Source column, caller-clamped to [0, src_w).
 * @param[in] y     Source row, caller-clamped to [0, src_h).
 * @return 0x00RRGGBB gray colour with R == G == B.
 * @retval 0x00000000 The sampled nibble is 0 (black).
 * @retval 0x00FFFFFF The sampled nibble is 0xF (white).
 * @pre @p src addresses at least `(y * src_w + x) / 2 + 1` readable bytes.
 * @pre @p x and @p y are already clamped to the image bounds by the caller.
 * @post No memory is modified (read-only traversal).
 * @post The returned colour has equal R, G and B channels.
 * @note Not thread-safe only in that it reads the caller's buffer; no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_gray4_color(const uint8_t* src, int32_t src_w, int32_t x, int32_t y)
{
  const size_t  flat = ((size_t)y * (size_t)src_w) + (size_t)x;
  const uint8_t byte = src[flat >> 1U];
  const uint8_t nib =
    ((flat & 1U) != 0U) ? (uint8_t)(byte & k_ra8_g4_nib_lo) : (uint8_t)(byte >> k_ra8_g4_nib_sh);
  const uint8_t g8 = (uint8_t)((nib << k_ra8_g4_nib_sh) | nib);
  return ((uint32_t)g8 << k_ra8_g4_rgb_r_sh) | ((uint32_t)g8 << k_ra8_g4_rgb_g_sh) | (uint32_t)g8;
}

/**
 * @brief Fill the @p zoom x @p zoom framebuffer block at (@p bx, @p by) with @p color.
 *
 * @details Writes one magnified source pixel: a solid @p zoom-square block whose
 * top-left is (@p bx, @p by). Each destination pixel is plotted through
 * s_gfx_text_plot(), which clips against the active clip rectangle, so a block
 * straddling a clip edge writes only its visible pixels.
 *
 * @param[in] bx    Block left edge in framebuffer pixels.
 * @param[in] by    Block top edge in framebuffer pixels.
 * @param[in] zoom  Block edge length in pixels; caller-guaranteed >= 1.
 * @param[in] color 0x00RRGGBB colour to write to every pixel of the block.
 * @return Nothing.
 * @pre s_gfx_text_state is initialised (caller checked).
 * @pre @p zoom >= 1 so the block is non-empty.
 * @post Every in-clip pixel of the block equals the down-converted @p color.
 * @post Pixels outside the clip rectangle are left unchanged.
 * @note Not thread-safe; shares s_gfx_text_state with all rasteriser functions.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_gray4_block(int32_t bx, int32_t by, int32_t zoom, uint32_t color)
{
  for (int32_t dy = 0; dy < zoom; ++dy) {
    for (int32_t dx = 0; dx < zoom; ++dx) {
      s_gfx_text_plot(bx + dx, by + dy, color);
    }
  }
}

ra8_err_t ra8_gfx_blit_gray4_zoom(const uint8_t* src,
                                  int32_t        src_w,
                                  int32_t        src_h,
                                  int32_t        sx,
                                  int32_t        sy,
                                  int32_t        sw,
                                  int32_t        sh,
                                  int32_t        zoom,
                                  int32_t        dst_x,
                                  int32_t        dst_y)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if (src == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (zoom <= 0) {
    return k_ra8_err_invalid_arg;
  }
  if ((src_w <= 0) || (src_h <= 0)) {
    return k_ra8_err_invalid_arg;
  }
  /* Clamp the sampled window to the source image bounds; an off-image edge draws
   * only the in-image portion at its natural (dst + offset*zoom) position. A
   * non-positive sw/sh collapses the range, so nothing is drawn. */
  const int32_t x_lo = (sx > 0) ? sx : 0;
  const int32_t y_lo = (sy > 0) ? sy : 0;
  const int32_t x_hi = ((sx + sw) < src_w) ? (sx + sw) : src_w;
  const int32_t y_hi = ((sy + sh) < src_h) ? (sy + sh) : src_h;
  for (int32_t py = y_lo; py < y_hi; ++py) {
    const int32_t by = dst_y + ((py - sy) * zoom);
    for (int32_t px = x_lo; px < x_hi; ++px) {
      internal_gray4_block(dst_x + ((px - sx) * zoom),
                           by,
                           zoom,
                           internal_gray4_color(src, src_w, px, py));
    }
  }
  return k_ra8_ok;
}
