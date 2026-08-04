/**
 * @file ra8_gfx_text_glyph.c
 * @brief Glyph-cell blitting and text rendering for the ra8_gfx software rasteriser.
 *
 * @details
 * Holds the text sub-responsibility of the software pixel-pusher: the 1-bpp
 * glyph-cell blitters and the public text entry points (ra8_gfx_text_out,
 * ra8_gfx_text_size). These share the single framebuffer binding
 * (s_gfx_text_state) and the low-level packers/plotters with the core
 * rasteriser TU (ra8_gfx_text.c) via ra8_gfx_internal.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_internal.h"

/**
 * @enum ra8_gfx_glyph_bits_t
 * @brief Constants for monochrome glyph bit extraction.
 */
typedef enum : uint8_t {
  k_glyph_bits_per_byte = 8, /**< Glyph bits per byte. */
  k_glyph_msb_index     = 7, /**< Glyph msb index.     */
} ra8_gfx_glyph_bits_t;

/**
 * @enum ra8_gfx_color_masks_t
 * @brief Per-channel masks for colour conversion.
 */
typedef enum : uint32_t {
  k_mask_byte = 0xFFU, /**< Mask byte. */
} ra8_gfx_color_masks_t;

/**
 * @enum ra8_gfx_byte_idx_t
 * @brief Per-byte indices for RGB888 packing.
 */
typedef enum : uint8_t {
  k_idx_r = 0, /**< Index r. */
  k_idx_g = 1, /**< Index g. */
} ra8_gfx_byte_idx_t;

/**
 * @brief Blit one 1-bpp glyph cell into an RGB565 framebuffer, clipped on screen.
 *
 * @details
 * The foreground and background colours are packed to RGB565 once, then each
 * visible cell pixel stores the two pre-packed bytes directly -- so the per-glyph
 * colour packing (s_gfx_text_pack_565 + per-channel extraction) and the per-pixel
 * clip happen once each instead of once per pixel. The cell is clipped to the
 * framebuffer up front (the same in-bounds set s_gfx_text_plot would have written),
 * and every on/off bit maps to the same colour, so the result is byte-identical to
 * the per-pixel s_gfx_text_plot path. The full cell is painted (foreground for set
 * bits, background for clear bits), matching the per-pixel path's opaque cell.
 *
 * @param[in]  x         Glyph top-left x in framebuffer pixels.
 * @param[in]  y         Glyph top-left y in framebuffer pixels.
 * @param[in]  gw        Glyph cell width in pixels.
 * @param[in]  gh        Glyph cell height in pixels.
 * @param[in]  row_bytes Bytes per glyph row in gd.
 * @param[in]  gd        Packed 1-bpp glyph bitmap (MSB-first within each byte).
 * @param[in]  fg        Foreground 32-bit colour (applied to set bits).
 * @param[in]  bg        Background 32-bit colour (applied to clear bits).
 * @pre The bound format is k_ra8_gfx_format_rgb565.
 * @pre @p gd addresses at least gh*row_bytes bytes.
 * @post Every on-screen cell pixel holds fg (set bit) or bg (clear bit).
 * @post No off-screen pixel is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_blit_glyph_565(int32_t        x,
                                    int32_t        y,
                                    int32_t        gw,
                                    int32_t        gh,
                                    uint32_t       row_bytes,
                                    const uint8_t* gd,
                                    uint32_t       fg,
                                    uint32_t       bg)
{
  /* Clip the glyph cell against the active clip rectangle (within the
   * framebuffer), so it honours both the panel bounds and any dirty-region clip.
   * Default full clip => byte-identical to the unclipped blit. */
  const int32_t cl  = s_gfx_text_state.clip_x0;
  const int32_t ct  = s_gfx_text_state.clip_y0;
  const int32_t cr  = s_gfx_text_state.clip_x1;
  const int32_t cb  = s_gfx_text_state.clip_y1;
  const int32_t cx0 = (x >= cl) ? x : cl;
  const int32_t cy0 = (y >= ct) ? y : ct;
  const int64_t xr  = (int64_t)x + (int64_t)gw; /* 64-bit: no int32 overflow. */
  const int64_t yr  = (int64_t)y + (int64_t)gh;
  const int32_t cx1 = (xr < (int64_t)cr) ? (int32_t)xr : cr;
  const int32_t cy1 = (yr < (int64_t)cb) ? (int32_t)yr : cb;
  if ((cx1 <= cx0) || (cy1 <= cy0)) {
    return; /* glyph fully outside the clip. */
  }
  const uint16_t vfg    = s_gfx_text_pack_565(fg);
  const uint16_t vbg    = s_gfx_text_pack_565(bg);
  const uint8_t  flo    = (uint8_t)(vfg & (uint16_t)k_mask_byte);
  const uint8_t  fhi    = (uint8_t)((vfg >> k_glyph_bits_per_byte) & (uint16_t)k_mask_byte);
  const uint8_t  blo    = (uint8_t)(vbg & (uint16_t)k_mask_byte);
  const uint8_t  bhi    = (uint8_t)((vbg >> k_glyph_bits_per_byte) & (uint16_t)k_mask_byte);
  const size_t   bpp    = (size_t)s_gfx_text_state.bpp;
  const size_t   stride = (size_t)s_gfx_text_state.width * bpp;
  for (int32_t sy = cy0; sy < cy1; sy++) {
    const uint32_t grow = (uint32_t)(sy - y);
    uint8_t*       p    = s_gfx_text_state.fb + ((size_t)sy * stride) + ((size_t)cx0 * bpp);
    for (int32_t sx = cx0; sx < cx1; sx++) {
      const uint32_t gcol     = (uint32_t)(sx - x);
      const uint32_t byte_idx = (grow * row_bytes) + (gcol / (uint32_t)k_glyph_bits_per_byte);
      const uint8_t  bit = (uint8_t)(k_glyph_msb_index - (gcol % (uint32_t)k_glyph_bits_per_byte));
      const bool     on  = ((gd[byte_idx] >> bit) & 0x01U) != 0U;
      p[k_idx_r]         = on ? flo : blo;
      p[k_idx_g]         = on ? fhi : bhi;
      p += bpp;
    }
  }
}

/**
 * @brief Render a single glyph at (x,y).
 *
 * @details See implementation.
 * @param[in] x See implementation.
 * @param[in] y See implementation.
 * @param[in] font See implementation.
 * @param[in] cp See implementation.
 * @param[in] fg See implementation.
 * @param[in] bg See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_render_glyph(int32_t               x,
                                  int32_t               y,
                                  const ra8_gfx_font_t* font,
                                  uint8_t               cp,
                                  uint32_t              fg,
                                  uint32_t              bg)
{
  uint8_t idx;
  if ((cp < font->first_codepoint) || (cp > font->last_codepoint)) {
    idx = 0; /* render as space */
  } else {
    idx = (uint8_t)(cp - font->first_codepoint);
  }
  const uint32_t row_bytes =
    ((uint32_t)font->glyph_width + (k_glyph_bits_per_byte - 1U)) / k_glyph_bits_per_byte;
  const uint8_t* gd = font->glyph_data + ((size_t)idx * (size_t)font->bytes_per_glyph);
  if (s_gfx_text_state.format == k_ra8_gfx_format_rgb565) {
    internal_blit_glyph_565(x,
                            y,
                            (int32_t)font->glyph_width,
                            (int32_t)font->glyph_height,
                            row_bytes,
                            gd,
                            fg,
                            bg);
    return;
  }
  for (uint32_t row = 0; row < font->glyph_height; row++) {
    for (uint32_t col = 0; col < font->glyph_width; col++) {
      const uint32_t byte_idx = (row * row_bytes) + (col / k_glyph_bits_per_byte);
      const uint8_t  bit      = (uint8_t)(k_glyph_msb_index - (col % k_glyph_bits_per_byte));
      const bool     on       = ((gd[byte_idx] >> bit) & 0x01U) != 0U;
      s_gfx_text_plot(x + (int32_t)col, y + (int32_t)row, on ? fg : bg);
    }
  }
}

ra8_err_t ra8_gfx_text_out(int32_t               x,
                           int32_t               y,
                           const char*           str,
                           const ra8_gfx_font_t* font,
                           uint32_t              fg_color,
                           uint32_t              bg_color)
{
  if ((str == nullptr) || (font == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  int32_t       cur_x  = x;
  const int32_t step_x = (int32_t)font->glyph_width;
  /* Static bound: at most one glyph per pixel column we could ever cover. */
  const uint32_t max_chars = k_ra8_gfx_max_dim;
  for (uint32_t i = 0; i < max_chars; i++) {
    const char c = str[i];
    if (c == '\0') {
      break;
    }
    internal_render_glyph(cur_x, y, font, (uint8_t)c, fg_color, bg_color);
    cur_x += step_x;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_gfx_text_size(const char* str, const ra8_gfx_font_t* font, uint32_t* out_w, uint32_t* out_h)
{
  if ((str == nullptr) || (font == nullptr) || (out_w == nullptr) || (out_h == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  uint32_t       n         = 0;
  const uint32_t max_chars = k_ra8_gfx_max_dim;
  for (uint32_t i = 0; i < max_chars; i++) {
    if (str[i] == '\0') {
      break;
    }
    n++;
  }
  *out_w = n * (uint32_t)font->glyph_width;
  *out_h = (uint32_t)font->glyph_height;
  return k_ra8_ok;
}
