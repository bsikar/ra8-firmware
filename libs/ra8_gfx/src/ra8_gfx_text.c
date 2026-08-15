/**
 * @file ra8_gfx_text.c
 * @brief Software pixel-pusher implementation of ra8_gfx primitives.
 *
 * @details
 * Portable C software rasteriser. The framebuffer pointer + format are
 * stored in module-static state by ra8_gfx_init(); each draw entry point
 * dispatches to a per-format pixel writer.
 *
 * The main shapes (line, rect, circle) clip per pixel, which is fine for
 * the sizes we render (a 7-inch panel is 1024 x 600). When DRW becomes
 * available the rect/blit fast paths will be replaced; the public API
 * does not change.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_internal.h"

/** @brief RGB565 channel masks. */
typedef enum : uint32_t {
  k_565_r_mask = 0x1FU, /**< 5-bit red.   */
  k_565_g_mask = 0x3FU, /**< 6-bit green. */
  k_565_b_mask = 0x1FU, /**< 5-bit blue.  */
} gfx_565_mask_t;

/**
 * @enum ra8_gfx_color_shifts_t
 * @brief Bit positions for unpacking 0xAARRGGBB colour values.
 */
typedef enum : uint8_t {
  k_shift_blue  = 0,  /**< Shift blue.  */
  k_shift_green = 8,  /**< Shift green. */
  k_shift_red   = 16, /**< Shift red.   */
  k_shift_alpha = 24, /**< Shift alpha. */
} ra8_gfx_color_shifts_t;

/**
 * @enum ra8_gfx_color_masks_t
 * @brief Per-channel masks for colour conversion.
 */
typedef enum : uint32_t {
  k_mask_byte = 0xFFU, /**< Mask byte. */
} ra8_gfx_color_masks_t;

/**
 * @enum ra8_gfx_565_t
 * @brief RGB565 packing constants.
 */
typedef enum : uint8_t {
  k_565_r_shift_in  = 3,  /**< drop 3 LSBs of R (8 -> 5 bits). */
  k_565_g_shift_in  = 2,  /**< drop 2 LSBs of G (8 -> 6 bits). */
  k_565_b_shift_in  = 3,  /**< drop 3 LSBs of B (8 -> 5 bits). */
  k_565_r_shift_out = 11, /**< 565 r shift out.                */
  k_565_g_shift_out = 5,  /**< 565 g shift out.                */
} ra8_gfx_565_t;

/**
 * @enum ra8_gfx_byte_idx_t
 * @brief Per-byte indices for RGB888 packing.
 */
typedef enum : uint8_t {
  k_idx_r      = 0, /**< Index r.      */
  k_idx_g      = 1, /**< Index g.      */
  k_idx_b      = 2, /**< Index b.      */
  k_idx_argb_b = 0, /**< Index argb b. */
  k_idx_argb_g = 1, /**< Index argb g. */
  k_idx_argb_r = 2, /**< Index argb r. */
  k_idx_argb_a = 3, /**< Index argb a. */
} ra8_gfx_byte_idx_t;

/**
 * @enum ra8_gfx_glyph_bits_t
 * @brief Constants for monochrome glyph bit extraction.
 */
typedef enum : uint8_t {
  k_glyph_bits_per_byte = 8, /**< Glyph bits per byte. */
  k_glyph_msb_index     = 7, /**< Glyph msb index.     */
} ra8_gfx_glyph_bits_t;

/** @brief RGB565 bytes per pixel (used to size span byte counts). */
typedef enum : uint8_t {
  k_rgb565_bpp = 2, /**< Rgb565 bpp. */
} ra8_gfx_rgb565_bpp_t;

/**
 * @var s_gfx_text_state
 * @brief Module-wide framebuffer binding (single shared object; see ra8_gfx_internal.h).
 *
 * @details
 * The one definition of the shared rasteriser state. Other ra8_gfx TUs read it
 * through the extern declaration in ra8_gfx_internal.h.
 *
 * @note Not thread-safe; mutated only by ra8_gfx_init()/ra8_gfx_set_clip().
 * @warning Do not redefine; this is the single shared object for all ra8_gfx TUs.
 * @since 0.1.0
 */
// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) -- format unused until ra8_gfx_init().
ra8_gfx_state_t s_gfx_text_state = {};

/* ------------------------------------------------------------------ */
/* Helpers */
/* ------------------------------------------------------------------ */

/**
 * @brief Extract red byte from a 0xAARRGGBB colour.
 *
 * @details See implementation.
 * @param[in] color See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_color_r(uint32_t color)
{
  return (uint8_t)((color >> k_shift_red) & k_mask_byte);
}
/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] color See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_color_g(uint32_t color)
{
  return (uint8_t)((color >> k_shift_green) & k_mask_byte);
}
/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] color See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_color_b(uint32_t color)
{
  return (uint8_t)((color >> k_shift_blue) & k_mask_byte);
}
/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] color See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_color_a(uint32_t color)
{
  return (uint8_t)((color >> k_shift_alpha) & k_mask_byte);
}

/** @brief Implementation of `priv_gfx_text_pack_565()` -- promoted for cross-TU use. */
uint16_t priv_gfx_text_pack_565(uint32_t color)
{
  const uint16_t r = (uint16_t)(internal_color_r(color) >> k_565_r_shift_in);
  const uint16_t g = (uint16_t)(internal_color_g(color) >> k_565_g_shift_in);
  const uint16_t b = (uint16_t)(internal_color_b(color) >> k_565_b_shift_in);
  return (uint16_t)((r << k_565_r_shift_out) | (g << k_565_g_shift_out) | b);
}

/**
 * @brief Bytes per pixel for a given format.
 *
 * @details See implementation.
 * @param[in] format See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_bpp(ra8_gfx_format_t format)
{
  return (uint8_t)format;
}

/**
 * @brief Validate a format enum.
 *
 * @details See implementation.
 * @param[in] f See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline bool internal_format_ok(ra8_gfx_format_t f)
{
  return (f == k_ra8_gfx_format_rgb565) || (f == k_ra8_gfx_format_rgb888) ||
         (f == k_ra8_gfx_format_argb8888);
}

/**
 * @brief Write one pixel into a generic destination buffer.
 *
 * @param[out] dst     Destination buffer base.
 * @param[in]  stride  Bytes per row in destination.
 * @param[in]  format  Destination pixel format.
 * @param[in]  x,y     Pixel coordinates within the destination.
 * @param[in]  color   32-bit colour.
 *
 * @details See implementation.
 * @param[in] y See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_put_pixel(uint8_t*         dst,
                               size_t           stride,
                               ra8_gfx_format_t format,
                               size_t           x,
                               size_t           y,
                               uint32_t         color)
{
  uint8_t* p = dst + (y * stride) + (x * (size_t)internal_bpp(format));
  switch (format) {
    case k_ra8_gfx_format_rgb565: {
      const uint16_t v = priv_gfx_text_pack_565(color);
      p[k_idx_r]       = (uint8_t)(v & k_mask_byte);
      p[k_idx_g]       = (uint8_t)((v >> k_glyph_bits_per_byte) & k_mask_byte);
      break;
    }
    case k_ra8_gfx_format_rgb888: {
      p[k_idx_r] = internal_color_r(color);
      p[k_idx_g] = internal_color_g(color);
      p[k_idx_b] = internal_color_b(color);
      break;
    }
    case k_ra8_gfx_format_argb8888: {
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
 *
 * @details See implementation.
 * @param[in] src See implementation.
 * @param[in] stride See implementation.
 * @param[in] format See implementation.
 * @param[in] x See implementation.
 * @param[in] y See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_get_pixel(const uint8_t* src, size_t stride, ra8_gfx_format_t format, size_t x, size_t y)
{
  const uint8_t* p = src + (y * stride) + (x * (size_t)internal_bpp(format));
  switch (format) {
    case k_ra8_gfx_format_rgb565: {
      const uint16_t v = (uint16_t)(p[k_idx_r] | ((uint16_t)p[k_idx_g] << k_glyph_bits_per_byte));
      const uint32_t r = ((uint32_t)(v >> k_565_r_shift_out) & k_565_r_mask) << k_565_r_shift_in;
      const uint32_t g = ((uint32_t)(v >> k_565_g_shift_out) & k_565_g_mask) << k_565_g_shift_in;
      const uint32_t b = ((uint32_t)v & k_565_b_mask) << k_565_b_shift_in;
      return (r << k_shift_red) | (g << k_shift_green) | (b << k_shift_blue);
    }
    case k_ra8_gfx_format_rgb888: {
      const uint32_t r = p[k_idx_r];
      const uint32_t g = p[k_idx_g];
      const uint32_t b = p[k_idx_b];
      return (r << k_shift_red) | (g << k_shift_green) | (b << k_shift_blue);
    }
    case k_ra8_gfx_format_argb8888: {
      const uint32_t bb = p[k_idx_argb_b];
      const uint32_t gg = p[k_idx_argb_g];
      const uint32_t rr = p[k_idx_argb_r];
      const uint32_t aa = p[k_idx_argb_a];
      return (aa << k_shift_alpha) | (rr << k_shift_red) | (gg << k_shift_green) | bb;
    }
  }
  return 0U;
}

/** @brief Implementation of `priv_gfx_text_plot()` -- promoted for cross-TU use. */
void priv_gfx_text_plot(int32_t x, int32_t y, uint32_t color)
{
  /* The clip rectangle is always within the framebuffer, so this single test
   * enforces both the clip and the framebuffer bounds. With the default clip
   * (0,0,width,height) it is identical to a plain bounds check. */
  if ((x < s_gfx_text_state.clip_x0) || (y < s_gfx_text_state.clip_y0)) {
    return;
  }
  if ((x >= s_gfx_text_state.clip_x1) || (y >= s_gfx_text_state.clip_y1)) {
    return;
  }
  internal_put_pixel(s_gfx_text_state.fb,
                     (size_t)s_gfx_text_state.width * (size_t)s_gfx_text_state.bpp,
                     s_gfx_text_state.format,
                     (size_t)x,
                     (size_t)y,
                     color);
}

/**
 * @brief Fill `count` consecutive RGB565 pixels at `p` with packed bytes lo,hi.
 *
 * @details
 * One tight inner loop that stores the two pre-packed bytes per pixel, so a span
 * fill pays the colour packing (priv_gfx_text_pack_565 + the per-channel extraction)
 * once for the whole run instead of once per pixel. The byte order (low byte
 * first) is exactly what internal_put_pixel writes for k_ra8_gfx_format_rgb565, so
 * the result is byte-identical to plotting each pixel individually.
 *
 * @param[out] p     Destination of the first pixel (2 bytes per pixel).
 * @param[in]  count Number of pixels to write.
 * @param[in]  lo    Low byte of the packed RGB565 word.
 * @param[in]  hi    High byte of the packed RGB565 word.
 * @pre p is non-null and addresses at least 2*count writable bytes.
 * @pre The destination format is k_ra8_gfx_format_rgb565.
 * @post count pixels starting at p hold the packed colour.
 * @post No bytes outside the 2*count-byte run are modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void internal_fill_565(uint8_t* p, size_t count, uint8_t lo, uint8_t hi)
{
  if (lo == hi) {
    /* Symmetric packed colour (e.g. white paper 0xFFFF, black 0x0000): every
     * byte of the run is the same value, so the optimised libc memset replaces
     * the byte loop. Byte-identical -- both write `lo` to every byte. This is the
     * common full-screen clear, so it is the hot path. */
    (void)memset(p, (int)lo, count * (size_t)k_rgb565_bpp);
    return;
  }
  for (size_t i = 0; i < count; i++) {
    *p++ = lo;
    *p++ = hi;
  }
}

/**
 * @brief Span-fill an axis-aligned RGB565 rectangle, clipped to the framebuffer.
 *
 * @details
 * Clips [x,x+w) x [y,y+h) to the framebuffer once, then fills each visible row
 * with internal_fill_565. This writes exactly the pixels priv_gfx_text_plot would
 * (the same in-bounds set) with the same packed bytes, so it is byte-identical to
 * the per-pixel fill while replacing roughly six function calls per pixel
 * (plot -> put_pixel -> pack_565 -> color_r/g/b -> bpp) with one packed store.
 *
 * @param[in] x     Top-left corner x (may be partly or fully off-screen).
 * @param[in] y     Top-left corner y (may be partly or fully off-screen).
 * @param[in] w     Width in pixels (assumed > 0 by the caller).
 * @param[in] h     Height in pixels (assumed > 0 by the caller).
 * @param[in] color 32-bit colour, packed to RGB565 once.
 * @pre The module is initialised and the format is k_ra8_gfx_format_rgb565.
 * @pre w > 0 and h > 0 (guaranteed by ra8_gfx_rect).
 * @post Every on-screen pixel of the rectangle holds the packed colour.
 * @post No off-screen or out-of-rectangle pixel is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fill_rect_565(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  /* Clip against the active clip rectangle (which is itself within the
   * framebuffer), so this is both the on-screen clip and the dirty-region clip.
   * With the default full-framebuffer clip it reduces to min/max against the
   * framebuffer, i.e. byte-identical to the unclipped fill. */
  const int32_t cl = s_gfx_text_state.clip_x0;
  const int32_t ct = s_gfx_text_state.clip_y0;
  const int32_t cr = s_gfx_text_state.clip_x1;
  const int32_t cb = s_gfx_text_state.clip_y1;
  const int32_t x0 = (x >= cl) ? x : cl;
  const int32_t y0 = (y >= ct) ? y : ct;
  /* Right/bottom edges in 64-bit so x+w / y+h cannot overflow int32 for a
   * pathological caller; then clamped to the clip. */
  const int64_t xr = (int64_t)x + (int64_t)w;
  const int64_t yr = (int64_t)y + (int64_t)h;
  const int32_t x1 = (xr < (int64_t)cr) ? (int32_t)xr : cr;
  const int32_t y1 = (yr < (int64_t)cb) ? (int32_t)yr : cb;
  if ((x1 <= x0) || (y1 <= y0)) {
    return; /* fully clipped -- nothing to fill. */
  }
  const uint16_t v      = priv_gfx_text_pack_565(color);
  const uint8_t  lo     = (uint8_t)(v & (uint16_t)k_mask_byte);
  const uint8_t  hi     = (uint8_t)((v >> k_glyph_bits_per_byte) & (uint16_t)k_mask_byte);
  const size_t   bpp    = (size_t)s_gfx_text_state.bpp;
  const size_t   stride = (size_t)s_gfx_text_state.width * bpp;
  const size_t   count  = (size_t)(x1 - x0);
  for (int32_t row = y0; row < y1; row++) {
    internal_fill_565(s_gfx_text_state.fb + ((size_t)row * stride) + ((size_t)x0 * bpp),
                      count,
                      lo,
                      hi);
  }
}

/**
 * @brief Fill a solid rectangle (RGB565 span fast path, else per-pixel).
 *
 * @details
 * RGB565 -- the panel format -- takes the clipped span fill; other formats fall
 * back to the per-pixel priv_gfx_text_plot loop. Both write the same in-bounds pixels.
 *
 * @param[in] x     Top-left corner x.
 * @param[in] y     Top-left corner y.
 * @param[in] w     Width in pixels (> 0, checked by ra8_gfx_rect).
 * @param[in] h     Height in pixels (> 0, checked by ra8_gfx_rect).
 * @param[in] color 32-bit fill colour.
 * @pre The module is initialised (checked by ra8_gfx_rect).
 * @pre w > 0 and h > 0 (checked by ra8_gfx_rect).
 * @post Every on-screen pixel of the rectangle holds the colour.
 * @post No off-screen pixel is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  if (s_gfx_text_state.format == k_ra8_gfx_format_rgb565) {
    internal_fill_rect_565(x, y, w, h, color);
    return;
  }
  for (int32_t row = 0; row < h; row++) {
    for (int32_t col = 0; col < w; col++) {
      priv_gfx_text_plot(x + col, y + row, color);
    }
  }
}

/**
 * @brief Draw the four edges of a 1-pixel-wide rectangle outline.
 *
 * @details
 * Plots the top row (y), bottom row (y+h-1), left column (x), and right column
 * (x+w-1) by iterating over all column and row positions in two separate loops.
 * interior pixels are never touched. Each pixel is routed through priv_gfx_text_plot,
 * which applies the active clip rectangle before writing to the framebuffer, so
 * partial off-screen outlines are rendered correctly without additional guards here.
 *
 * @param[in] x     Top-left corner x.
 * @param[in] y     Top-left corner y.
 * @param[in] w     Width in pixels (> 0, checked by ra8_gfx_rect).
 * @param[in] h     Height in pixels (> 0, checked by ra8_gfx_rect).
 * @param[in] color 32-bit edge colour.
 * @pre The module is initialised (checked by ra8_gfx_rect).
 * @pre w > 0 and h > 0 (checked by ra8_gfx_rect).
 * @post The four edge runs hold the colour; the interior is untouched.
 * @post priv_gfx_text_plot clips every pixel to the framebuffer.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_rect_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  for (int32_t col = 0; col < w; col++) {
    priv_gfx_text_plot(x + col, y, color);
    priv_gfx_text_plot(x + col, y + h - 1, color);
  }
  for (int32_t row = 0; row < h; row++) {
    priv_gfx_text_plot(x, y + row, color);
    priv_gfx_text_plot(x + w - 1, y + row, color);
  }
}

/* ------------------------------------------------------------------ */
/* Public API */
/* ------------------------------------------------------------------ */

ra8_err_t ra8_gfx_init(void* fb, uint16_t width, uint16_t height, ra8_gfx_format_t format)
{
  if (fb == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((width < k_ra8_gfx_min_dim) || (width > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if ((height < k_ra8_gfx_min_dim) || (height > k_ra8_gfx_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_format_ok(format)) {
    return k_ra8_err_invalid_arg;
  }
  s_gfx_text_state.fb          = (uint8_t*)fb;
  s_gfx_text_state.width       = width;
  s_gfx_text_state.height      = height;
  s_gfx_text_state.format      = format;
  s_gfx_text_state.bpp         = internal_bpp(format);
  s_gfx_text_state.clip_x0     = 0;
  s_gfx_text_state.clip_y0     = 0;
  s_gfx_text_state.clip_x1     = (int32_t)width; /* default clip = whole framebuffer. */
  s_gfx_text_state.clip_y1     = (int32_t)height;
  s_gfx_text_state.initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_clear(uint32_t color)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  /* "Clear" fills the active clip region (the whole framebuffer by default).
   * For RGB565 this is the span-fill path -- pack once, memset each row when the
   * bytes are symmetric -- restricted to the clip, so an incremental repaint
   * only touches the damaged area. With the default clip the rect is the whole
   * framebuffer and the bytes written are identical to the old contiguous fill. */
  if (s_gfx_text_state.format == k_ra8_gfx_format_rgb565) {
    internal_fill_rect_565(0,
                           0,
                           (int32_t)s_gfx_text_state.width,
                           (int32_t)s_gfx_text_state.height,
                           color);
    return k_ra8_ok;
  }
  for (int32_t y = s_gfx_text_state.clip_y0; y < s_gfx_text_state.clip_y1; y++) {
    for (int32_t x = s_gfx_text_state.clip_x0; x < s_gfx_text_state.clip_x1; x++) {
      internal_put_pixel(s_gfx_text_state.fb,
                         (size_t)s_gfx_text_state.width * (size_t)s_gfx_text_state.bpp,
                         s_gfx_text_state.format,
                         (size_t)x,
                         (size_t)y,
                         color);
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_set_clip(int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  const int32_t fbw = (int32_t)s_gfx_text_state.width;
  const int32_t fbh = (int32_t)s_gfx_text_state.height;
  /* Clamp the top-left into [0, fb]; the bottom-right into [top-left, fb]. The
   * edges are computed in 64-bit so x+w / y+h cannot overflow int32, and a
   * non-positive size or fully off-screen rect collapses to an empty clip
   * (x1==x0 or y1==y0), which makes every subsequent draw a no-op. */
  int32_t x0 = (x > 0) ? x : 0;
  int32_t y0 = (y > 0) ? y : 0;
  if (x0 > fbw) {
    x0 = fbw;
  }
  if (y0 > fbh) {
    y0 = fbh;
  }
  const int64_t xr = (int64_t)x + (int64_t)w;
  const int64_t yr = (int64_t)y + (int64_t)h;
  int32_t       x1 = (xr < (int64_t)fbw) ? (int32_t)xr : fbw;
  int32_t       y1 = (yr < (int64_t)fbh) ? (int32_t)yr : fbh;
  if (x1 < x0) {
    x1 = x0;
  }
  if (y1 < y0) {
    y1 = y0;
  }
  s_gfx_text_state.clip_x0 = x0;
  s_gfx_text_state.clip_y0 = y0;
  s_gfx_text_state.clip_x1 = x1;
  s_gfx_text_state.clip_y1 = y1;
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_reset_clip(void)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  s_gfx_text_state.clip_x0 = 0;
  s_gfx_text_state.clip_y0 = 0;
  s_gfx_text_state.clip_x1 = (int32_t)s_gfx_text_state.width;
  s_gfx_text_state.clip_y1 = (int32_t)s_gfx_text_state.height;
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_pixel(int32_t x, int32_t y, uint32_t color)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((x < 0) || (y < 0) || (x >= (int32_t)s_gfx_text_state.width) ||
      (y >= (int32_t)s_gfx_text_state.height)) {
    return k_ra8_err_range_check_failed;
  }
  priv_gfx_text_plot(x, y, color);
  return k_ra8_ok;
}

/**
 * @brief Expand an 8-bit gray sample to the 0x00RRGGBB colour word priv_gfx_text_pack_565 expects.
 *
 * @details
 * Replicates the single gray value g into the R, G, and B channels of a packed
 * 0x00RRGGBB word by shifting g to the positions defined by k_shift_red,
 * k_shift_green, and k_shift_blue, then ORing the three contributions together.
 * The alpha channel is left as zero (fully opaque is implied by convention
 * throughout the software rasteriser). The result can be fed directly to
 * priv_gfx_text_pack_565 to produce the equivalent RGB565 pixel.
 *
 * @param[in] g 8-bit gray intensity in the range [0, 255].
 * @return uint32_t Packed 0x00RRGGBB colour with R==G==B==g.
 * @retval 0x00000000 When g is 0 (black).
 * @retval 0x00FFFFFF When g is 255 (white).
 * @pre g must fit in 8 bits (values above 255 saturate the low byte only).
 * @pre The module-level colour shift constants k_shift_red/k_shift_green/k_shift_blue are valid.
 * @post The returned word has identical R, G, and B byte values equal to g.
 * @post The alpha byte of the returned word is zero.
 * @note Not thread-safe; reads only compile-time constants, so concurrent calls are harmless.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_gray_to_color(uint32_t g)
{
  return (g << (uint32_t)k_shift_red) | (g << (uint32_t)k_shift_green) |
         (g << (uint32_t)k_shift_blue);
}

/**
 * @brief RGB565 fast path for ra8_gfx_blit_gray8(): clip pre-resolved, tight rows.
 *
 * @details
 * Writes the clipped block [x0,x1) x [y0,y1) from a packed gray8 source image
 * directly into the RGB565 framebuffer. For each visible pixel the gray sample
 * is located via the src pointer, w (source row stride), dst_x, and dst_y
 * offset arithmetic, then expanded to 0x00RRGGBB by internal_gray_to_color()
 * before being packed to two RGB565 bytes by priv_gfx_text_pack_565(). The two bytes
 * are written in the same low-byte-first order that internal_put_pixel() uses for
 * k_ra8_gfx_format_rgb565, making the result byte-identical to the per-pixel path
 * while avoiding the per-pixel clip overhead.
 *
 * @param[in] src   Base pointer of the source gray8 image (one byte per pixel).
 * @param[in] w     Source image width in pixels (row stride in bytes).
 * @param[in] dst_x Destination x offset of the source image top-left in the framebuffer.
 * @param[in] dst_y Destination y offset of the source image top-left in the framebuffer.
 * @param[in] x0    First visible column (inclusive), already clipped by the caller.
 * @param[in] y0    First visible row (inclusive), already clipped by the caller.
 * @param[in] x1    One past the last visible column (exclusive), already clipped.
 * @param[in] y1    One past the last visible row (exclusive), already clipped.
 * @pre The framebuffer format is k_ra8_gfx_format_rgb565 and s_gfx_text_state is initialised.
 * @pre x0 < x1 and y0 < y1 (non-empty visible region, guaranteed by the caller).
 * @post Every framebuffer pixel in [x0,x1) x [y0,y1) holds the RGB565 encoding of its gray sample.
 * @post No pixel outside that clipped rectangle is modified.
 * @note Not thread-safe; shares s_gfx_text_state with all other rasteriser functions.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_blit_gray8_565(const uint8_t* src,
                                    int32_t        w,
                                    int32_t        dst_x,
                                    int32_t        dst_y,
                                    int32_t        x0,
                                    int32_t        y0,
                                    int32_t        x1,
                                    int32_t        y1)
{
  const size_t stride = (size_t)s_gfx_text_state.width * (size_t)s_gfx_text_state.bpp;
  for (int32_t y = y0; y < y1; ++y) {
    uint8_t* p = s_gfx_text_state.fb + ((size_t)y * stride) + ((size_t)x0 * (size_t)k_rgb565_bpp);
    const uint8_t* s = src + ((size_t)(y - dst_y) * (size_t)w) + (size_t)(x0 - dst_x);
    for (int32_t x = x0; x < x1; ++x) {
      const uint16_t v = priv_gfx_text_pack_565(internal_gray_to_color((uint32_t)*s));
      p[k_idx_r]       = (uint8_t)(v & k_mask_byte);
      p[k_idx_g]       = (uint8_t)((v >> k_glyph_bits_per_byte) & k_mask_byte);
      p += k_rgb565_bpp;
      ++s;
    }
  }
}

/**
 * @brief Per-pixel fallback for non-RGB565 formats, matching priv_gfx_text_plot() exactly.
 *
 * @details
 * Iterates over every pixel in the w x h source gray8 image, expands each
 * sample to 0x00RRGGBB via internal_gray_to_color(), and calls priv_gfx_text_plot()
 * to write the result at framebuffer coordinates (dx+col, dy+row). Because
 * priv_gfx_text_plot() performs per-pixel clipping against the active clip rectangle,
 * no additional bounds checking is needed here. This path is taken for all formats
 * other than k_ra8_gfx_format_rgb565; the RGB565 fast path is internal_blit_gray8_565().
 *
 * @param[in] src Base pointer of the source gray8 image (one byte per pixel, row-major).
 * @param[in] w   Source image width in pixels (also the byte stride per row).
 * @param[in] h   Source image height in pixels.
 * @param[in] dx  Destination x offset of the image top-left in the framebuffer.
 * @param[in] dy  Destination y offset of the image top-left in the framebuffer.
 * @pre src is non-null and addresses at least w*h readable bytes.
 * @pre w > 0 and h > 0 (guaranteed by ra8_gfx_blit_gray8() before dispatch).
 * @post Every on-screen pixel in the destination rectangle holds the colour of its gray sample.
 * @post Off-screen pixels are silently dropped by priv_gfx_text_plot() clip checks.
 * @note Not thread-safe; shares s_gfx_text_state with all other rasteriser functions.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_blit_gray8_slow(const uint8_t* src, int32_t w, int32_t h, int32_t dx, int32_t dy)
{
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      priv_gfx_text_plot(dx + x,
                         dy + y,
                         internal_gray_to_color(src[((size_t)y * (size_t)w) + (size_t)x]));
    }
  }
}

ra8_err_t ra8_gfx_blit_gray8(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((src == nullptr) || (w <= 0) || (h <= 0)) {
    return k_ra8_err_invalid_arg;
  }
  /* Non-panel formats keep the exact per-pixel path; the RGB565 panel resolves
   * the clip once and runs a tight row loop. */
  if (s_gfx_text_state.format != k_ra8_gfx_format_rgb565) {
    internal_blit_gray8_slow(src, w, h, dst_x, dst_y);
    return k_ra8_ok;
  }
  /* Clip the block to the clip rectangle ONCE (always within the framebuffer). */
  const int32_t x0 = (dst_x > s_gfx_text_state.clip_x0) ? dst_x : s_gfx_text_state.clip_x0;
  const int32_t y0 = (dst_y > s_gfx_text_state.clip_y0) ? dst_y : s_gfx_text_state.clip_y0;
  const int32_t x1 =
    ((dst_x + w) < s_gfx_text_state.clip_x1) ? (dst_x + w) : s_gfx_text_state.clip_x1;
  const int32_t y1 =
    ((dst_y + h) < s_gfx_text_state.clip_y1) ? (dst_y + h) : s_gfx_text_state.clip_y1;
  if ((x0 < x1) && (y0 < y1)) {
    internal_blit_gray8_565(src, w, dst_x, dst_y, x0, y0, x1, y1);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
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
  const int32_t max_iters = (int32_t)(k_ra8_gfx_max_dim * 2);
  for (int32_t i = 0; i < max_iters; i++) {
    priv_gfx_text_plot(x, y, color);
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
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, bool filled)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra8_ok;
  }
  if (filled) {
    internal_fill_rect(x, y, w, h, color);
  } else {
    internal_rect_outline(x, y, w, h, color);
  }
  return k_ra8_ok;
}

/**
 * @brief Plot the eight symmetric points of a midpoint-circle step.
 *
 * @details See implementation.
 * @param[in] cx See implementation.
 * @param[in] cy See implementation.
 * @param[in] x See implementation.
 * @param[in] y See implementation.
 * @param[in] color See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_circle_outline_step(int32_t cx, int32_t cy, int32_t x, int32_t y, uint32_t color)
{
  priv_gfx_text_plot(cx + x, cy + y, color);
  priv_gfx_text_plot(cx - x, cy + y, color);
  priv_gfx_text_plot(cx + x, cy - y, color);
  priv_gfx_text_plot(cx - x, cy - y, color);
  priv_gfx_text_plot(cx + y, cy + x, color);
  priv_gfx_text_plot(cx - y, cy + x, color);
  priv_gfx_text_plot(cx + y, cy - x, color);
  priv_gfx_text_plot(cx - y, cy - x, color);
}

/**
 * @brief Plot the two horizontal scan-lines for a midpoint-circle step.
 *
 * @details See implementation.
 * @param[in] cx See implementation.
 * @param[in] cy See implementation.
 * @param[in] x See implementation.
 * @param[in] y See implementation.
 * @param[in] color See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_circle_filled_step(int32_t cx, int32_t cy, int32_t x, int32_t y, uint32_t color)
{
  for (int32_t col = -x; col <= x; col++) {
    priv_gfx_text_plot(cx + col, cy + y, color);
    priv_gfx_text_plot(cx + col, cy - y, color);
  }
  for (int32_t col = -y; col <= y; col++) {
    priv_gfx_text_plot(cx + col, cy + x, color);
    priv_gfx_text_plot(cx + col, cy - x, color);
  }
}

ra8_err_t ra8_gfx_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color, bool filled)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if (r < 0) {
    return k_ra8_err_invalid_arg;
  }
  /* Midpoint circle algorithm. */
  int32_t x   = r;
  int32_t y   = 0;
  int32_t err = 1 - r;
  /* Static loop bound. */
  const int32_t max_iters = (int32_t)k_ra8_gfx_max_dim;
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
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_blit(const void*      src_buf,
                       uint16_t         src_w,
                       uint16_t         src_h,
                       ra8_gfx_format_t src_format,
                       int32_t          dst_x,
                       int32_t          dst_y)
{
  if (src_buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((src_w == 0) || (src_h == 0) || !internal_format_ok(src_format)) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t* src        = (const uint8_t*)src_buf;
  const size_t   src_stride = (size_t)src_w * (size_t)internal_bpp(src_format);
  for (uint32_t row = 0; row < src_h; row++) {
    for (uint32_t col = 0; col < src_w; col++) {
      const uint32_t color = internal_get_pixel(src, src_stride, src_format, col, row);
      priv_gfx_text_plot(dst_x + (int32_t)col, dst_y + (int32_t)row, color);
    }
  }
  return k_ra8_ok;
}
