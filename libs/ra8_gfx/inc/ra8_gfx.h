/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gfx.h
 * @brief Software 2D graphics primitives layered on top of a caller-owned
 * @ingroup grp_ereader
 *        framebuffer (DRW / D/AVE 2D / GLCDC ready).
 *
 * @details
 * This module is a small, dependency-free 2D graphics library aimed at the
 * RA8D2's GLCDC + DRW (D/AVE 2D) accelerator. The current implementation is
 * a portable C software pixel pusher; if `RA8_GFX_USE_DRW` is defined a
 * future revision can route ra8_gfx_rect / ra8_gfx_blit through ra8_drw's
 * hardware blitter without changing call sites.
 *
 * The framebuffer memory itself is owned by the caller -- ra8_gfx_init only
 * remembers a pointer and metadata, so the same library can be used over
 * the GLCDC display plane, an off-screen scratch buffer, or a host-side
 * test buffer.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_gfx_font.h"

/**
 * @enum ra8_gfx_format_t
 * @brief Pixel format of the framebuffer bound by ra8_gfx_init().
 *
 * @details
 * Values double as the bytes-per-pixel - the low byte is the byte stride
 * for one pixel; the high byte is just a unique tag.
 */
typedef enum : uint8_t {
  k_ra8_gfx_format_rgb565   = 2, /**< 16-bit RGB565, little-endian in memory. */
  k_ra8_gfx_format_rgb888   = 3, /**< 24-bit packed R,G,B bytes.              */
  k_ra8_gfx_format_argb8888 = 4, /**< 32-bit ARGB, A in MSB.                  */
} ra8_gfx_format_t;

/**
 * @enum ra8_gfx_dim_limits_t
 * @brief Bounds on framebuffer dimensions accepted by ra8_gfx_init().
 */
typedef enum : uint16_t {
  k_ra8_gfx_min_dim = 1,    /**< Minimum width or height in pixels. */
  k_ra8_gfx_max_dim = 4096, /**< Maximum supported edge length.     */
} ra8_gfx_dim_limits_t;

/**
 * @brief Bind ra8_gfx to a caller-owned framebuffer.
 *
 * @param[in] fb     Pointer to framebuffer memory.
 * @param[in] width  Framebuffer width in pixels.
 * @param[in] height Framebuffer height in pixels.
 * @param[in] format Pixel format (see ra8_gfx_format_t).
 *
 * @return Error code.
 * @retval k_ra8_ok               Bound successfully.
 * @retval k_ra8_err_null_ptr     `fb` was NULL.
 * @retval k_ra8_err_invalid_arg  Dimensions out of range or unsupported format.
 *
 * @pre  fb points to at least `width * height * bytes_per_pixel(format)` bytes.
 * @pre  width, height in [1, 4096].
 * @post Subsequent draw calls operate on the bound buffer.
 * @post On error, no global state is changed.
 *
 * @note Not thread-safe; bind once during init.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_init(void* fb, uint16_t width, uint16_t height, ra8_gfx_format_t format);

/**
 * @brief Fill the bound framebuffer's clip region with a single colour.
 *
 * @param[in] color 32-bit colour in 0x00RRGGBB or 0xAARRGGBB form.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Cleared.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @post Every pixel of the active clip region equals `color` (down-converted as
 *       needed to the active pixel format). With the default (full-framebuffer)
 *       clip this is the whole framebuffer.
 *
 * @note Honours the clip rectangle set by ra8_gfx_set_clip(); the default clip is
 *       the whole framebuffer, so unclipped callers see no change.
 * @see ra8_gfx_set_clip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_clear(uint32_t color);

/**
 * @brief Restrict all subsequent drawing to a rectangle (dirty-region updates).
 *
 * @details
 * Every draw call (clear, rect, line, pixel, text, blit) writes only pixels
 * inside the intersection of this rectangle and the framebuffer; pixels outside
 * are left untouched. This is the primitive for incremental / dirty-region
 * repaints -- set the clip to the damaged area, redraw, then ra8_gfx_reset_clip()
 * -- so a small change (e.g. an overlay banner) does not repaint the whole panel.
 * The clip persists until changed or reset; ra8_gfx_init() and ra8_gfx_reset_clip()
 * set it to the full framebuffer.
 *
 * @param[in] x Clip left in pixels (clamped to the framebuffer).
 * @param[in] y Clip top in pixels (clamped to the framebuffer).
 * @param[in] w Clip width in pixels (a non-positive width yields an empty clip).
 * @param[in] h Clip height in pixels (a non-positive height yields an empty clip).
 *
 * @return Error code.
 * @retval k_ra8_ok                  Clip set (possibly empty).
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @post Subsequent draws are confined to the clamped rectangle.
 * @post An off-screen or zero-area request leaves an empty clip (draws are no-ops).
 *
 * @note Not thread-safe; the clip is module-global state.
 * @see ra8_gfx_reset_clip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_set_clip(int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * @brief Reset the clip rectangle to the whole framebuffer.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Clip reset to the full framebuffer.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @post Subsequent draws may touch any framebuffer pixel.
 *
 * @note Not thread-safe; the clip is module-global state.
 * @see ra8_gfx_set_clip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_reset_clip(void);

/**
 * @brief Set a single pixel.
 *
 * @param[in] x     Column, 0 = left.
 * @param[in] y     Row, 0 = top.
 * @param[in] color 32-bit colour.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Pixel written.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 * @retval k_ra8_err_range_check_failed (x,y) outside framebuffer.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @post On success the addressed pixel equals the down-converted colour.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_pixel(int32_t x, int32_t y, uint32_t color);

/**
 * @brief Blit an 8-bit grayscale image to a framebuffer rectangle.
 *
 * @details
 * Writes the @p w x @p h block of 8-bit gray samples at @p src into the bound
 * framebuffer with its top-left at (@p dst_x, @p dst_y), expanding each sample
 * @c g to the colour `(g<<16)|(g<<8)|g` and down-converting to the panel format.
 * The clip rectangle and framebuffer bounds are resolved ONCE for the whole
 * block (not per pixel), so a full image is a tight row loop -- the same pixels
 * a per-pixel ra8_gfx_pixel() loop would write, far fewer instructions. Rows are
 * sampled left-to-right, top-to-bottom; @p src is row-major with stride @p w.
 *
 * @param[in] src   Row-major 8-bit gray buffer of at least @p w * @p h bytes.
 * @param[in] w     Source width in pixels (> 0).
 * @param[in] h     Source height in pixels (> 0).
 * @param[in] dst_x Destination column of the block's left edge.
 * @param[in] dst_y Destination row of the block's top edge.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Visible pixels written (or fully clipped out).
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 * @retval k_ra8_err_invalid_arg     @p src is NULL, or @p w / @p h <= 0.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @pre  @p src holds at least @p w * @p h bytes.
 * @post Each in-clip destination pixel equals its down-converted gray sample.
 * @post Pixels outside the clip rectangle are left unchanged.
 *
 * @note Not thread-safe; shares the single ra8_gfx bind state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_blit_gray8(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y);

/**
 * @brief Nearest-neighbour integer-zoom blit of a sub-rectangle of a packed
 *        4-bit grayscale image into the framebuffer (no scale-to-fit).
 *
 * @details
 * Samples the source sub-rectangle [@p sx, @p sx + @p sw) x [@p sy, @p sy + @p sh)
 * of a packed gray4 image and writes every sampled source pixel as a @p zoom x
 * @p zoom nearest-neighbour block, so the destination spans @p sw * @p zoom by
 * @p sh * @p zoom pixels (before clipping). Unlike ra8_gfx_blit() or the reflow
 * engine's scale-to-fit path, no source pixels are decimated -- this is the 1:1
 * (@p zoom == 1) or magnified (@p zoom >= 2) view a reader loupe uses to inspect
 * a full-resolution page window without resolution loss.
 *
 * The source is packed two pixels per byte at flat nibble index `y * src_w + x`:
 * an even flat index occupies the high nibble, an odd flat index the low nibble.
 * Each 4-bit sample @c n is expanded to the 8-bit gray `(n << 4) | n`, then to
 * `(g << 16) | (g << 8) | g` before down-conversion to the bound pixel format --
 * byte-identical to ra8_gfx_blit_gray8()'s expansion of the same gray level.
 *
 * The sampled window is clamped to the source image bounds [0, @p src_w) x
 * [0, @p src_h): a sub-rectangle that runs off an image edge draws only the
 * in-image portion at its natural destination offset (the off-image remainder is
 * left untouched). Destination pixels are additionally confined to the active
 * clip rectangle, so a lens window near a panel edge is clipped, never wrapped.
 *
 * @param[in] src   Packed gray4 source image, `>= (src_w * src_h + 1) / 2` bytes.
 * @param[in] src_w Source image width in pixels (> 0); also the nibble stride.
 * @param[in] src_h Source image height in pixels (> 0).
 * @param[in] sx    Sub-rectangle left in source pixels (may be negative).
 * @param[in] sy    Sub-rectangle top in source pixels (may be negative).
 * @param[in] sw    Sub-rectangle width in source pixels (<= 0 draws nothing).
 * @param[in] sh    Sub-rectangle height in source pixels (<= 0 draws nothing).
 * @param[in] zoom  Integer magnification factor (>= 1; <= 0 rejected).
 * @param[in] dst_x Destination column of the sub-rectangle's top-left.
 * @param[in] dst_y Destination row of the sub-rectangle's top-left.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Visible pixels written (or fully clipped out).
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 * @retval k_ra8_err_invalid_arg     @p src is NULL, @p src_w / @p src_h <= 0, or @p zoom <= 0.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @pre  @p src holds at least `(src_w * src_h + 1) / 2` readable bytes.
 * @post Each in-clip, in-image destination pixel equals its zoomed gray sample.
 * @post Pixels outside the clip rectangle or off the source image are unchanged.
 *
 * @note Not thread-safe; shares the single ra8_gfx bind state.
 * @see ra8_gfx_blit_gray8  1:1 gray8 blit with no zoom or sub-rect.
 * @see ra8_gfx_set_clip    Confine the lens blit to its window.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_blit_gray4_zoom(const uint8_t* src,
                                                int32_t        src_w,
                                                int32_t        src_h,
                                                int32_t        sx,
                                                int32_t        sy,
                                                int32_t        sw,
                                                int32_t        sh,
                                                int32_t        zoom,
                                                int32_t        dst_x,
                                                int32_t        dst_y);

/**
 * @brief Draw a line from (x0,y0) to (x1,y1) with Bresenham's algorithm.
 *
 * @param[in] x0,y0,x1,y1 Endpoints (clipped to framebuffer).
 * @param[in] color       32-bit colour.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Line drawn (after clipping).
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre ra8_gfx_init() returned k_ra8_ok.
 * @post Pixels on the rasterised line within bounds equal `color`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);

/**
 * @brief Draw an axis-aligned rectangle.
 *
 * @param[in] x,y    Top-left corner.
 * @param[in] w,h    Width and height in pixels.
 * @param[in] color  32-bit colour.
 * @param[in] filled true for solid fill, false for 1-pixel outline.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Drawn.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre ra8_gfx_init() returned k_ra8_ok.
 * @post Rectangle area within bounds is updated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, bool filled);

/**
 * @brief Draw a circle using midpoint algorithm.
 *
 * @param[in] cx,cy  Centre.
 * @param[in] r      Radius in pixels (>= 0).
 * @param[in] color  32-bit colour.
 * @param[in] filled true for solid disc, false for 1-pixel outline.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Drawn.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 * @retval k_ra8_err_invalid_arg     r < 0.
 *
 * @pre ra8_gfx_init() returned k_ra8_ok.
 * @post Pixels on / inside the circle within bounds are updated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color, bool filled);

/**
 * @brief Render a NUL-terminated ASCII string.
 *
 * @param[in] x,y      Top-left of the first glyph.
 * @param[in] str      NUL-terminated ASCII string (NULL not allowed).
 * @param[in] font     Font descriptor (NULL not allowed).
 * @param[in] fg_color Foreground colour.
 * @param[in] bg_color Background colour.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Rendered.
 * @retval k_ra8_err_null_ptr        str or font was NULL.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @pre  font->glyph_data covers at least last-first+1 glyphs.
 * @post Glyph cells within bounds are filled with fg/bg colour pairs.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_text_out(int32_t               x,
                                         int32_t               y,
                                         const char*           str,
                                         const ra8_gfx_font_t* font,
                                         uint32_t              fg_color,
                                         uint32_t              bg_color);

/**
 * @brief Compute rendered pixel dimensions of a string.
 *
 * @param[in]  str   ASCII string.
 * @param[in]  font  Font descriptor.
 * @param[out] out_w Receives total pixel width.
 * @param[out] out_h Receives total pixel height (font glyph height).
 *
 * @return Error code.
 * @retval k_ra8_ok               Measured.
 * @retval k_ra8_err_null_ptr     Any argument was NULL.
 *
 * @pre  All pointer arguments are non-NULL.
 * @post *out_w and *out_h are written.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_gfx_text_size(const char* str, const ra8_gfx_font_t* font, uint32_t* out_w, uint32_t* out_h);

/**
 * @brief Copy a sub-image from `src_buf` into the framebuffer at (dst_x,dst_y).
 *
 * @param[in] src_buf    Source image bytes.
 * @param[in] src_w,src_h Source size.
 * @param[in] src_format Pixel format of the source image.
 * @param[in] dst_x,dst_y Destination top-left in the framebuffer.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Blitted (clipped if needed).
 * @retval k_ra8_err_null_ptr        src_buf was NULL.
 * @retval k_ra8_err_not_initialized ra8_gfx_init() was not called.
 * @retval k_ra8_err_invalid_arg     src_w or src_h was zero or src_format invalid.
 *
 * @pre  ra8_gfx_init() returned k_ra8_ok.
 * @post Destination rectangle (clipped to FB) holds the converted pixels.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_blit(const void*      src_buf,
                                     uint16_t         src_w,
                                     uint16_t         src_h,
                                     ra8_gfx_format_t src_format,
                                     int32_t          dst_x,
                                     int32_t          dst_y);
