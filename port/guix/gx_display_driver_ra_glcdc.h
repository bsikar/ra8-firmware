/**
 * @file gx_display_driver_ra_glcdc.h
 * @brief Eclipse GUIX display driver shim against the RA8D2 GLCDC + ra_gfx
 *
 * @deprecated GUIX retirement -- tracked by issue #81. This GUIX driver shim
 * is replaced by the ra_reflow chrome renderer (issue #80) and is scheduled
 * for wholesale removal. Do not add new callers.
 *
 * @par Tag
 * [Ring 4 / DRV] {World: S}
 *
 * @details
 * Bridges Eclipse GUIX's display-driver function-pointer table onto
 * the project's `ra_glcdc` framebuffer plumbing and the `ra_gfx`
 * software pixel pusher. The shim's job is two-fold:
 *
 *   1. Stand up a `GX_DISPLAY` whose pixel format is RGB565 (matching
 *      what GLCDC produces on the EK-RA8D2 panel) and whose write
 *      paths land in the same caller-owned framebuffer that GLCDC's
 *      Graphics 1 layer is currently scanning out.
 *   2. Forward GUIX's per-pixel and rectangular `block_move` calls
 *      through `ra_gfx_pixel` and `ra_gfx_blit` so the same software
 *      drawing code can be exercised in host unit tests against an
 *      off-screen buffer.
 *
 * For v1 the shim runs as a single-buffer driver: the `buffer_toggle`
 * entry point flushes the canvas dirty rectangle straight to the
 * GLCDC scan-out buffer without flipping a back-buffer pointer. Future
 * revisions can wire up a true double-buffer flip via
 * `ra_glcdc_set_layer2` -- the toggle hook already receives the
 * `GX_CANVAS *` so it has everything it needs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/* GUIX core types are pulled in transitively by the vendor headers
 * once they are on the include path; the host unit-test build does
 * not link the GUIX vendor tree, so guard the include the same way
 * `examples/threadx_*` apps guard `tx_api.h`.
 */
#ifndef RA_SIMULATOR_MODE
#include "gx_api.h"
#include "gx_display.h"
#endif

/**
 * @enum ra_guix_panel_dim_t
 * @brief Panel pixel resolution baked into the shim.
 *
 * @details
 * The EK-RA8D2 ships with a 1024 x 600 parallel-RGB panel. Apps that
 * use a different framebuffer size pass their dimensions through
 * `gx_display_create()` directly; the constants below double as a
 * compile-time sanity check that callers stay within
 * `GX_MAX_DISPLAY_HEIGHT` (800).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_guix_panel_width  = 1024U, /**< Panel visible width (pixels). */
  k_ra_guix_panel_height = 600U,  /**< Panel visible height (pixels). */
} ra_guix_panel_dim_t;

/**
 * @struct ra_guix_aux_t
 * @brief Auxiliary data attached to `display->gx_display_driver_data`
 *        so the per-driver hooks can recover the framebuffer base.
 *
 * @details
 * GUIX itself only knows about the framebuffer through the canvas's
 * `gx_canvas_memory` pointer set by `gx_canvas_create()`. The shim
 * stashes that base here too so `block_move` and `buffer_toggle`
 * can hand a flat byte pointer to `ra_gfx_blit` without re-deriving
 * it from the canvas every call.
 *
 * @invariant `framebuffer` is non-NULL once
 *            `ra_guix_display_driver_setup` has been called as the
 *            `display_driver_setup` argument to `gx_display_create`.
 *
 * @since 0.1.0
 */
typedef struct {
  void*    framebuffer; /**< RGB565 scan-out base (caller-owned).        */
  uint16_t width_px;    /**< Framebuffer width in pixels.                */
  uint16_t height_px;   /**< Framebuffer height in pixels.               */
  uint8_t  swap_count;  /**< Diagnostic counter incremented per toggle.  */
  uint8_t  reserved[3]; /**< Padding to keep struct size 32-bit clean.   */
} ra_guix_aux_t;

#ifndef RA_SIMULATOR_MODE

/**
 * @brief GUIX display-driver setup callback for the RA8D2 GLCDC.
 *
 * @details
 * Pass this function as the `display_driver_setup` argument to
 * `gx_display_create()`. It calls the vendor-supplied
 * `_gx_display_driver_565rgb_setup()` to populate the standard
 * RGB565 function-pointer table, then overrides `pixel_write`,
 * `block_move`, and `buffer_toggle` with project-specific versions
 * that route writes through the bound framebuffer.
 *
 * @param[in,out] display GUIX display control block created by
 *                        `gx_display_create()`. Must be non-NULL.
 *
 * @return GUIX status code.
 * @retval GX_SUCCESS         Driver function-pointer table populated.
 * @retval GX_INVALID_VALUE   `display` was NULL or the configured
 *                            framebuffer pointer has not been bound.
 *
 * @pre  `ra_guix_display_driver_bind()` has been called with a valid
 *       framebuffer base address, width and height.
 * @pre  GLCDC is running and scanning out the bound framebuffer.
 * @post `display->gx_display_color_format == GX_COLOR_FORMAT_565RGB`.
 * @post `display->gx_display_driver_pixel_write`,
 *       `..._block_move`, and `..._buffer_toggle` point at the shim's
 *       hand-written entries.
 *
 * @note Not thread-safe; call once at GUIX bring-up.
 *
 * @par Example:
 * @code
 * ra_guix_display_driver_bind(s_fb, 1024, 600);
 * gx_system_initialize();
 * gx_display_create(&display, "ra8d2-glcdc",
 *                   ra_guix_display_driver_setup, 1024, 600);
 * @endcode
 *
 * @see ra_guix_display_driver_bind()
 *
 * @since 0.1.0
 */
[[deprecated("GUIX retirement -- see issue #81")]] UINT
ra_guix_display_driver_setup(GX_DISPLAY* display);

#endif /* !RA_SIMULATOR_MODE */

/**
 * @brief Bind the shim to the framebuffer GLCDC is scanning out.
 *
 * @details
 * Stores `fb`, `width`, and `height` in a file-scope `ra_guix_aux_t`
 * so the driver setup callback can attach them to
 * `display->gx_display_driver_data`. Must be called before
 * `gx_display_create()`.
 *
 * @param[in] fb     Framebuffer base address (RGB565 pixels). Must be
 *                   non-NULL.
 * @param[in] width  Framebuffer width in pixels (1..4096).
 * @param[in] height Framebuffer height in pixels (1..4096).
 *
 * @return Error code.
 * @retval k_ra_ok               Bound successfully.
 * @retval k_ra_err_null_ptr     `fb` was NULL.
 * @retval k_ra_err_invalid_arg  `width` or `height` was 0.
 *
 * @pre  `ra_glcdc_init` has succeeded and `ra_glcdc_start(true)` has
 *       handed the panel to the GLCDC.
 * @post Subsequent calls to `ra_guix_display_driver_setup` see the
 *       bound base address through `display->gx_display_driver_data`.
 *
 * @note Not thread-safe; call once at boot before launching GUIX.
 *
 * @since 0.1.0
 */
[[nodiscard]] [[deprecated("GUIX retirement -- see issue #81")]] ra_err_t
ra_guix_display_driver_bind(void* fb, uint16_t width, uint16_t height);

/**
 * @brief Read the current shim auxiliary data (test/debug hook).
 *
 * @details
 * Exposes the shim's internal `ra_guix_aux_t` so host unit tests can
 * verify the framebuffer pointer ends up where the setup callback
 * expects it. Returns NULL before `ra_guix_display_driver_bind()`.
 *
 * @return Pointer to the internal aux record, or NULL if unbound.
 *
 * @note The pointer is valid for the lifetime of the program; do not
 *       free it.
 *
 * @since 0.1.0
 */
const ra_guix_aux_t* ra_guix_get_aux(void);

#ifdef __cplusplus
}
#endif
