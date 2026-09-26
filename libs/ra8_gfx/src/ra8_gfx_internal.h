/**
 * @file ra8_gfx_internal.h
 * @brief Module-private declarations shared across the ra8_gfx software rasteriser TUs.
 * @ingroup grp_ereader
 *
 * @details
 * The software pixel-pusher is split across several translation units that all
 * operate on one framebuffer binding. This header exposes the single mutable
 * module state object and the few low-level helpers that more than one TU needs,
 * so the implementation can be partitioned without duplicating mutable state.
 *
 * Read-only colour/format enum constants are intentionally NOT declared here:
 * each TU keeps its own private copy of those compile-time literals.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_gfx.h"

/**
 * @struct ra8_gfx_state_t
 * @brief Internal module state populated by ra8_gfx_init().
 */
typedef struct {
  uint8_t*         fb;          /**< Framebuffer base.                        */
  uint16_t         width;       /**< Width in pixels.                         */
  uint16_t         height;      /**< Height in pixels.                        */
  uint32_t         pitch;       /**< Row pitch in bytes; >= width * bpp.      */
  ra8_gfx_format_t format;      /**< Pixel format.                            */
  uint8_t          bpp;         /**< Bytes per pixel.                         */
  bool             initialized; /**< Set after a successful init.             */
  int32_t          clip_x0;     /**< Clip left (inclusive), within [0,width]. */
  int32_t          clip_y0;     /**< Clip top (inclusive), within [0,height]. */
  int32_t          clip_x1;     /**< Clip right (exclusive), within [0,w].    */
  int32_t          clip_y1;     /**< Clip bottom (exclusive), within [0,h].   */
} ra8_gfx_state_t;

/**
 * @var g_gfx_text_state
 * @brief Single module-wide framebuffer binding shared by every ra8_gfx TU.
 *
 * @details
 * Defined once in ra8_gfx_text.c (populated by ra8_gfx_init()); the text/glyph TU
 * reads it through this extern declaration. There is exactly one object so all
 * draw entry points observe the same framebuffer, format, and clip rectangle.
 *
 * @note Not thread-safe; mutated only by ra8_gfx_init()/ra8_gfx_set_clip().
 * @warning Do not redefine; this is a single shared object, not per-TU state.
 * @since 0.1.0
 */
extern ra8_gfx_state_t g_gfx_text_state;

/**
 * @brief Pack a 32-bit colour into a single RGB565 word.
 *
 * @details
 * Shared low-level colour packer. Promoted to module-external linkage so both
 * the core rasteriser TU and the text/glyph TU pack colours identically.
 *
 * @param[in] color 32-bit 0xAARRGGBB colour to pack.
 * @return uint16_t Packed RGB565 word.
 * @retval 0x0000 When color is black.
 * @retval 0xFFFF When color is white.
 * @pre Module state is consistent.
 * @pre The colour packing constants are valid compile-time literals.
 * @post The returned word is the RGB565 encoding of color.
 * @post No module state is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV uint16_t priv_gfx_text_pack_565(uint32_t color);

/**
 * @brief Bytes per pixel for a pixel format.
 *
 * @details
 * The format enum's low byte is the per-pixel byte stride by construction, so
 * this is the one place that fact is spelled out. Promoted to module-external
 * linkage so the bind TU and the rasteriser TUs size pixels identically.
 *
 * @param[in] format Pixel format to size.
 * @return uint8_t Bytes occupied by one pixel of `format`.
 * @retval 2 For k_ra8_gfx_format_rgb565.
 * @retval 4 For k_ra8_gfx_format_argb8888.
 * @pre `format` is a valid ra8_gfx_format_t.
 * @post No module state is modified.
 * @note Thread-safe; pure function of its argument.
 * @since 0.1.0
 */
RA8_PRIV uint8_t priv_gfx_bpp(ra8_gfx_format_t format);

/**
 * @brief Test whether a format enum is one this library renders.
 *
 * @details
 * Shared by both bind entry points and by the blit source-format check, so a
 * format accepted in one place cannot be rejected in another.
 *
 * @param[in] f Format value to validate.
 * @return bool Whether `f` names a supported format.
 * @retval true  `f` is RGB565, RGB888 or ARGB8888.
 * @retval false Any other value.
 * @pre None.
 * @post No module state is modified.
 * @note Thread-safe; pure function of its argument.
 * @since 0.1.0
 */
RA8_PRIV bool priv_gfx_format_ok(ra8_gfx_format_t f);

/**
 * @brief Plot a single pixel with bounds checking against the active clip.
 *
 * @details
 * Shared per-pixel plotter. Promoted to module-external linkage so the
 * text/glyph TU can fall back to the exact per-pixel path used by the core
 * rasteriser for non-RGB565 formats.
 *
 * @param[in] x     Framebuffer x coordinate.
 * @param[in] y     Framebuffer y coordinate.
 * @param[in] color 32-bit 0xAARRGGBB colour.
 * @pre Module state is consistent.
 * @pre g_gfx_text_state is initialised.
 * @post The pixel is written only if it lies within the active clip rectangle.
 * @post No pixel outside the clip rectangle is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void priv_gfx_text_plot(int32_t x, int32_t y, uint32_t color);
