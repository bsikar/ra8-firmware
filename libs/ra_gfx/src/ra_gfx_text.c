/**
 * @file ra_gfx_text.c
 * @brief Software pixel-pusher implementation of ra_gfx primitives.
 *
 * @details
 * Portable C software rasteriser. The framebuffer pointer + format are
 * stored in module-static state by ra_gfx_init(); each draw entry point
 * dispatches to a per-format pixel writer.
 *
 * The main shapes (line, rect, circle) clip per pixel, which is fine for
 * the sizes we render (a 7-inch panel is 1024 x 600). When DRW becomes
 * available the rect/blit fast paths will be replaced; the public API
 * does not change.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_gfx.h"

/**
 * @enum ra_gfx_color_shifts_t
 * @brief Bit positions for unpacking 0xAARRGGBB colour values.
 */
typedef enum : uint8_t {
  k_shift_blue  = 0,
  k_shift_green = 8,
  k_shift_red   = 16,
  k_shift_alpha = 24,
} ra_gfx_color_shifts_t;

/**
 * @enum ra_gfx_color_masks_t
 * @brief Per-channel masks for colour conversion.
 */
typedef enum : uint32_t {
  k_mask_byte = 0xFFU,
} ra_gfx_color_masks_t;

/**
 * @enum ra_gfx_565_t
 * @brief RGB565 packing constants.
 */
typedef enum : uint8_t {
  k_565_r_shift_in  = 3, /**< drop 3 LSBs of R (8 -> 5 bits). */
  k_565_g_shift_in  = 2, /**< drop 2 LSBs of G (8 -> 6 bits). */
  k_565_b_shift_in  = 3, /**< drop 3 LSBs of B (8 -> 5 bits). */
  k_565_r_shift_out = 11,
  k_565_g_shift_out = 5,
} ra_gfx_565_t;

/**
 * @enum ra_gfx_byte_idx_t
 * @brief Per-byte indices for RGB888 packing.
 */
typedef enum : uint8_t {
  k_idx_r      = 0,
  k_idx_g      = 1,
  k_idx_b      = 2,
  k_idx_argb_b = 0,
  k_idx_argb_g = 1,
  k_idx_argb_r = 2,
  k_idx_argb_a = 3,
} ra_gfx_byte_idx_t;

/**
 * @enum ra_gfx_glyph_bits_t
 * @brief Constants for monochrome glyph bit extraction.
 */
typedef enum : uint8_t {
  k_glyph_bits_per_byte = 8,
  k_glyph_msb_index     = 7,
} ra_gfx_glyph_bits_t;

/**
 * @struct ra_gfx_state_t
 * @brief Internal module state populated by ra_gfx_init().
 */
typedef struct {
  uint8_t*        fb;          /**< Framebuffer base.             */
  uint16_t        width;       /**< Width in pixels.              */
  uint16_t        height;      /**< Height in pixels.             */
  ra_gfx_format_t format;      /**< Pixel format.                 */
  uint8_t         bpp;         /**< Bytes per pixel.              */
  bool            initialised; /**< Set after a successful init.  */
} ra_gfx_state_t;

/** @brief Module-private framebuffer binding. */
// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) -- format unused until ra_gfx_init().
static ra_gfx_state_t s_state = {};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Extract red byte from a 0xAARRGGBB colour.
 */
static inline uint8_t internal_color_r(uint32_t color)
{
  return (uint8_t)((color >> k_shift_red) & k_mask_byte);
}
static inline uint8_t internal_color_g(uint32_t color)
{
  return (uint8_t)((color >> k_shift_green) & k_mask_byte);
}
static inline uint8_t internal_color_b(uint32_t color)
{
  return (uint8_t)((color >> k_shift_blue) & k_mask_byte);
}
static inline uint8_t internal_color_a(uint32_t color)
{
  return (uint8_t)((color >> k_shift_alpha) & k_mask_byte);
}

/**
 * @brief Pack a 32-bit colour into a single RGB565 word.
 */
static inline uint16_t internal_pack_565(uint32_t color)
{
  const uint16_t r = (uint16_t)(internal_color_r(color) >> k_565_r_shift_in);
  const uint16_t g = (uint16_t)(internal_color_g(color) >> k_565_g_shift_in);
  const uint16_t b = (uint16_t)(internal_color_b(color) >> k_565_b_shift_in);
  return (uint16_t)((r << k_565_r_shift_out) | (g << k_565_g_shift_out) | b);
}

/**
 * @brief Bytes per pixel for a given format.
 */
static inline uint8_t internal_bpp(ra_gfx_format_t format)
{
  return (uint8_t)format;
}

/**
 * @brief Validate a format enum.
 */
static inline bool internal_format_ok(ra_gfx_format_t f)
{
  return (f == k_ra_gfx_format_rgb565) || (f == k_ra_gfx_format_rgb888) ||
         (f == k_ra_gfx_format_argb8888);
}

/**
 * @brief Write one pixel into a generic destination buffer.
 *
 * @param[out] dst     Destination buffer base.
 * @param[in]  stride  Bytes per row in destination.
 * @param[in]  format  Destination pixel format.
 * @param[in]  x,y     Pixel coordinates within the destination.
 * @param[in]  color   32-bit colour.
 */
static void internal_put_pixel(uint8_t*        dst,
                               size_t          stride,
                               ra_gfx_format_t format,
                               size_t          x,
                               size_t          y,
                               uint32_t        color)
{
  uint8_t* p = dst + (y * stride) + (x * (size_t)internal_bpp(format));
  switch (format) {
    case k_ra_gfx_format_rgb565: {
      const uint16_t v = internal_pack_565(color);
      p[k_idx_r]       = (uint8_t)(v & k_mask_byte);
      p[k_idx_g]       = (uint8_t)((v >> k_glyph_bits_per_byte) & k_mask_byte);
      break;
    }
    case k_ra_gfx_format_rgb888: {
      p[k_idx_r] = internal_color_r(color);
      p[k_idx_g] = internal_color_g(color);
      p[k_idx_b] = internal_color_b(color);
      break;
    }
    case k_ra_gfx_format_argb8888: {
      p[k_idx_argb_b] = internal_color_b(color);
      p[k_idx_argb_g] = internal_color_g(color);
      p[k_idx_argb_r] = internal_color_r(color);
      p[k_idx_argb_a] = internal_color_a(color);
      break;
    }
  }
}

/**
 * @brief Read one pixel from a source buffer and return it as 0xAARRGGBB.
 */
static uint32_t
internal_get_pixel(const uint8_t* src, size_t stride, ra_gfx_format_t format, size_t x, size_t y)
{
  const uint8_t* p = src + (y * stride) + (x * (size_t)internal_bpp(format));
  switch (format) {
    case k_ra_gfx_format_rgb565: {
      const uint16_t v = (uint16_t)(p[k_idx_r] | ((uint16_t)p[k_idx_g] << k_glyph_bits_per_byte));
      const uint32_t r = ((uint32_t)(v >> k_565_r_shift_out) & 0x1FU) << k_565_r_shift_in;
      const uint32_t g = ((uint32_t)(v >> k_565_g_shift_out) & 0x3FU) << k_565_g_shift_in;
      const uint32_t b = ((uint32_t)v & 0x1FU) << k_565_b_shift_in;
      return (r << k_shift_red) | (g << k_shift_green) | (b << k_shift_blue);
    }
    case k_ra_gfx_format_rgb888: {
      const uint32_t r = p[k_idx_r];
      const uint32_t g = p[k_idx_g];
      const uint32_t b = p[k_idx_b];
      return (r << k_shift_red) | (g << k_shift_green) | (b << k_shift_blue);
    }
    case k_ra_gfx_format_argb8888: {
      const uint32_t bb = p[k_idx_argb_b];
      const uint32_t gg = p[k_idx_argb_g];
      const uint32_t rr = p[k_idx_argb_r];
      const uint32_t aa = p[k_idx_argb_a];
      return (aa << k_shift_alpha) | (rr << k_shift_red) | (gg << k_shift_green) | bb;
    }
  }
  return 0U;
}

/**
 * @brief Plot a single pixel with bounds checking.
 *
 * Used internally so that line/rect/circle can clip per pixel.
 */
static void internal_plot(int32_t x, int32_t y, uint32_t color)
{
  if ((x < 0) || (y < 0)) {
    return;
  }
  if ((x >= (int32_t)s_state.width) || (y >= (int32_t)s_state.height)) {
    return;
  }
  internal_put_pixel(s_state.fb,
                     (size_t)s_state.width * (size_t)s_state.bpp,
                     s_state.format,
                     (size_t)x,
                     (size_t)y,
                     color);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

ra_err_t ra_gfx_init(void* fb, uint16_t width, uint16_t height, ra_gfx_format_t format)
{
  if (fb == NULL) {
    return k_ra_err_null_ptr;
  }
  if ((width < k_ra_gfx_min_dim) || (width > k_ra_gfx_max_dim)) {
    return k_ra_err_invalid_arg;
  }
  if ((height < k_ra_gfx_min_dim) || (height > k_ra_gfx_max_dim)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_format_ok(format)) {
    return k_ra_err_invalid_arg;
  }
  s_state.fb          = (uint8_t*)fb;
  s_state.width       = width;
  s_state.height      = height;
  s_state.format      = format;
  s_state.bpp         = internal_bpp(format);
  s_state.initialised = true;
  return k_ra_ok;
}

ra_err_t ra_gfx_clear(uint32_t color)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  for (uint32_t y = 0; y < s_state.height; y++) {
    for (uint32_t x = 0; x < s_state.width; x++) {
      internal_put_pixel(s_state.fb,
                         (size_t)s_state.width * (size_t)s_state.bpp,
                         s_state.format,
                         x,
                         y,
                         color);
    }
  }
  return k_ra_ok;
}

ra_err_t ra_gfx_pixel(int32_t x, int32_t y, uint32_t color)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if ((x < 0) || (y < 0) || (x >= (int32_t)s_state.width) || (y >= (int32_t)s_state.height)) {
    return k_ra_err_range_check_failed;
  }
  internal_plot(x, y, color);
  return k_ra_ok;
}

ra_err_t ra_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  /* Bresenham. */
  int32_t       dx  = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
  int32_t       dy  = (y1 >= y0) ? -(y1 - y0) : -(y0 - y1);
  const int32_t sx  = (x0 < x1) ? 1 : -1;
  const int32_t sy  = (y0 < y1) ? 1 : -1;
  int32_t       err = dx + dy;
  int32_t       x   = x0;
  int32_t       y   = y0;
  /* Bound the iteration count statically (NASA Rule 2). */
  const int32_t max_iters = (int32_t)(k_ra_gfx_max_dim * 2);
  for (int32_t i = 0; i < max_iters; i++) {
    internal_plot(x, y, color);
    if ((x == x1) && (y == y1)) {
      break;
    }
    const int32_t e2 = err * 2;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, bool filled)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra_ok;
  }
  if (filled) {
    for (int32_t row = 0; row < h; row++) {
      for (int32_t col = 0; col < w; col++) {
        internal_plot(x + col, y + row, color);
      }
    }
  } else {
    for (int32_t col = 0; col < w; col++) {
      internal_plot(x + col, y, color);
      internal_plot(x + col, y + h - 1, color);
    }
    for (int32_t row = 0; row < h; row++) {
      internal_plot(x, y + row, color);
      internal_plot(x + w - 1, y + row, color);
    }
  }
  return k_ra_ok;
}

/**
 * @brief Plot the eight symmetric points of a midpoint-circle step.
 */
static void
internal_circle_outline_step(int32_t cx, int32_t cy, int32_t x, int32_t y, uint32_t color)
{
  internal_plot(cx + x, cy + y, color);
  internal_plot(cx - x, cy + y, color);
  internal_plot(cx + x, cy - y, color);
  internal_plot(cx - x, cy - y, color);
  internal_plot(cx + y, cy + x, color);
  internal_plot(cx - y, cy + x, color);
  internal_plot(cx + y, cy - x, color);
  internal_plot(cx - y, cy - x, color);
}

/**
 * @brief Plot the two horizontal scan-lines for a midpoint-circle step.
 */
static void
internal_circle_filled_step(int32_t cx, int32_t cy, int32_t x, int32_t y, uint32_t color)
{
  for (int32_t col = -x; col <= x; col++) {
    internal_plot(cx + col, cy + y, color);
    internal_plot(cx + col, cy - y, color);
  }
  for (int32_t col = -y; col <= y; col++) {
    internal_plot(cx + col, cy + x, color);
    internal_plot(cx + col, cy - x, color);
  }
}

ra_err_t ra_gfx_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color, bool filled)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if (r < 0) {
    return k_ra_err_invalid_arg;
  }
  /* Midpoint circle algorithm. */
  int32_t x   = r;
  int32_t y   = 0;
  int32_t err = 1 - r;
  /* Static loop bound. */
  const int32_t max_iters = (int32_t)k_ra_gfx_max_dim;
  for (int32_t i = 0; i < max_iters; i++) {
    if (filled) {
      internal_circle_filled_step(cx, cy, x, y, color);
    } else {
      internal_circle_outline_step(cx, cy, x, y, color);
    }
    if (x <= y) {
      break;
    }
    y++;
    if (err < 0) {
      err += (2 * y) + 1;
    } else {
      x--;
      err += (2 * (y - x)) + 1;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Render a single glyph at (x,y).
 */
static void internal_render_glyph(int32_t              x,
                                  int32_t              y,
                                  const ra_gfx_font_t* font,
                                  uint8_t              cp,
                                  uint32_t             fg,
                                  uint32_t             bg)
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
  for (uint32_t row = 0; row < font->glyph_height; row++) {
    for (uint32_t col = 0; col < font->glyph_width; col++) {
      const uint32_t byte_idx = (row * row_bytes) + (col / k_glyph_bits_per_byte);
      const uint8_t  bit      = (uint8_t)(k_glyph_msb_index - (col % k_glyph_bits_per_byte));
      const bool     on       = ((gd[byte_idx] >> bit) & 0x01U) != 0U;
      internal_plot(x + (int32_t)col, y + (int32_t)row, on ? fg : bg);
    }
  }
}

ra_err_t ra_gfx_text_out(int32_t              x,
                         int32_t              y,
                         const char*          str,
                         const ra_gfx_font_t* font,
                         uint32_t             fg_color,
                         uint32_t             bg_color)
{
  if ((str == NULL) || (font == NULL)) {
    return k_ra_err_null_ptr;
  }
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  int32_t       cur_x  = x;
  const int32_t step_x = (int32_t)font->glyph_width;
  /* Static bound: at most one glyph per pixel column we could ever cover. */
  const uint32_t max_chars = k_ra_gfx_max_dim;
  for (uint32_t i = 0; i < max_chars; i++) {
    const char c = str[i];
    if (c == '\0') {
      break;
    }
    internal_render_glyph(cur_x, y, font, (uint8_t)c, fg_color, bg_color);
    cur_x += step_x;
  }
  return k_ra_ok;
}

ra_err_t
ra_gfx_text_size(const char* str, const ra_gfx_font_t* font, uint32_t* out_w, uint32_t* out_h)
{
  if ((str == NULL) || (font == NULL) || (out_w == NULL) || (out_h == NULL)) {
    return k_ra_err_null_ptr;
  }
  uint32_t       n         = 0;
  const uint32_t max_chars = k_ra_gfx_max_dim;
  for (uint32_t i = 0; i < max_chars; i++) {
    if (str[i] == '\0') {
      break;
    }
    n++;
  }
  *out_w = n * (uint32_t)font->glyph_width;
  *out_h = (uint32_t)font->glyph_height;
  return k_ra_ok;
}

ra_err_t ra_gfx_blit(const void*     src_buf,
                     uint16_t        src_w,
                     uint16_t        src_h,
                     ra_gfx_format_t src_format,
                     int32_t         dst_x,
                     int32_t         dst_y)
{
  if (src_buf == NULL) {
    return k_ra_err_null_ptr;
  }
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if ((src_w == 0) || (src_h == 0) || !internal_format_ok(src_format)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* src        = (const uint8_t*)src_buf;
  const size_t   src_stride = (size_t)src_w * (size_t)internal_bpp(src_format);
  for (uint32_t row = 0; row < src_h; row++) {
    for (uint32_t col = 0; col < src_w; col++) {
      const uint32_t color = internal_get_pixel(src, src_stride, src_format, col, row);
      internal_plot(dst_x + (int32_t)col, dst_y + (int32_t)row, color);
    }
  }
  return k_ra_ok;
}
