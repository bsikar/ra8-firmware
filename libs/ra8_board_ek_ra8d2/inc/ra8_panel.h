/**
 * @file ra8_panel.h
 * @brief Active display-panel descriptor for the EK-RA8D2 (ER-TFT070-6)
 * @ingroup grp_board
 *
 * @details
 * BSP description of the display wired to the EK-RA8D2 Parallel Graphics
 * Expansion Board 1: the ER-TFT070-6, a 7.0" 1024x600 IPS panel driven RGB565
 * through the GLCDC. Panel-agnostic code (the shared UI, the host preview)
 * reads its geometry from here rather than hardcoding pixels.
 *
 * Each board package exposes its own ``ra8_panel.h``; the build selects the
 * active panel through the include path (no preprocessor selection), so a
 * different board -- e.g. a future e-reader -- swaps in its own descriptor
 * without touching the UI.
 *
 * Holds the full panel description: UI-facing geometry plus the GLCDC RGB
 * timing (porches / sync). The geometry + timing values here are plain
 * constants (host-includable); the GLCDC-typed timing instance the HAL
 * consumes lives in the companion ra8_panel_timing.h (target-only, since it
 * pulls in the HAL type).
 *
 * Self-contained (no MCU / FSP includes) so host-side previews include it
 * unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra8_panel_geometry_t
 * @brief ER-TFT070-6 geometry as compile-time constants.
 *
 * @details
 * The resolution is the hard pixel budget for any UI. The physical fields
 * drive design rules: size touch targets and fonts in millimetres, not just
 * pixels, because the panel PPI is far below a laptop display and the glass
 * does no supersampling. PPI = k_panel_width_px / (k_panel_width_mm / 25.4),
 * which for this panel is about 169. Physical values are approximate -- confirm
 * against the ER-TFT070-6 datasheet.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_panel_width_px  = 1024U, /**< Native width  (pixels).          */
  k_panel_height_px = 600U,  /**< Native height (pixels).          */
  k_panel_width_mm  = 154U,  /**< Active-area width  (mm, approx). */
  k_panel_height_mm = 86U,   /**< Active-area height (mm, approx). */
  k_panel_ppi       = 169U,  /**< Pixels per inch (approx).        */
} ra8_panel_geometry_t;

/**
 * @enum ra8_panel_timing_t
 * @brief ER-TFT070-6 RGB timing (porches + sync); active size = the geometry.
 *
 * @details Per the LVGL EK-RA8D2 reference for this panel (these differ from
 * the generic 1024x600 values: h_back 140->160, h_sync 20->4, v_back 20->23).
 * Assembled into the HAL's ra8_glcdc_timing_t in ra8_panel_timing.h.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_panel_h_front = 160U, /**< Horizontal front porch (px).   */
  k_panel_h_back  = 160U, /**< Horizontal back porch (px).    */
  k_panel_h_sync  = 4U,   /**< HSYNC pulse width (px clocks). */
  k_panel_v_front = 12U,  /**< Vertical front porch (lines).  */
  k_panel_v_back  = 23U,  /**< Vertical back porch (lines).   */
  k_panel_v_sync  = 3U,   /**< VSYNC pulse width (lines).     */
} ra8_panel_timing_t;

/**
 * @enum ra8_panel_framebuffer_t
 * @brief How a framebuffer for this panel must be laid out in memory.
 *
 * @details
 * The GLCDC fetches scanlines over AXI in bursts, and a framebuffer whose base
 * is not 64-byte aligned splits every burst -- which shows up as torn scanout,
 * not as an error. Nothing in the API can catch it, so the number has to be
 * stated where a consumer will find it. Before this existed, thirteen example
 * files each declared their own `= 64U` under a different name
 * (`k_app_fb_align_bytes`, `k_cm_fb_align`, `k_wd_fb_align`, ...), every one
 * carrying the same copied comment -- the tell that the fact had been copied
 * rather than derived.
 *
 * Use ::RA8_BOARD_PANEL_FRAMEBUFFER for a full-panel surface. An application
 * drawing into a smaller scratch surface declares its own array but should
 * still take the alignment from here.
 *
 * @invariant ::k_ra8_board_fb_align_bytes is a power of two and at least the
 *            AXI burst length the GLCDC issues.
 *
 * @code
 * [[gnu::aligned(k_ra8_board_fb_align_bytes)]] static uint16_t s_scratch[64 * 64];
 * @endcode
 *
 * @see RA8_BOARD_PANEL_FRAMEBUFFER  Declares a correctly shaped full-panel one.
 * @see ra8_panel_geometry_t         The panel's pixel budget.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_board_fb_align_bytes = 64U, /**< GLCDC AXI burst alignment, in bytes. */
} ra8_panel_framebuffer_t;

/**
 * @def RA8_BOARD_PANEL_FRAMEBUFFER
 * @brief Declare a correctly aligned, sized and placed RGB565 framebuffer for
 *        the on-board panel.
 *
 * @details
 * Expands to a file-scope `static` array of ::k_panel_height_px x
 * ::k_panel_width_px 16-bit pixels, aligned to ::k_ra8_board_fb_align_bytes so
 * the GLCDC's AXI bursts fetch cleanly, and placed in `.sdram_data` -- the
 * external-SDRAM output section every EK-RA8D2 linker script defines. All
 * three are board facts, and none of them is checkable by the API that
 * consumes the pointer: the wrong alignment is torn scanout, and 1.2 MiB of
 * surface does not fit in on-chip SRAM at all.
 *
 * This is the one case the project style rules name as macro-legal --
 * reducing duplicated code -- because the alternative, a library-owned array
 * behind an accessor, would fix the size at library build time and cost every
 * application the memory whether it drives the panel or not.
 *
 * An application drawing into a smaller scratch surface declares its own array
 * instead, but should still take the alignment from
 * ::k_ra8_board_fb_align_bytes rather than writing 64 again.
 *
 * @param[in] name Identifier for the declared array (a token, not a value).
 *
 * @pre `ra8_panel.h` is included and the app targets the on-board panel.
 * @pre Used at file scope; the expansion declares a `static` object.
 * @pre The app's linker script defines a `.sdram_data` output section (the
 *      shared board script does).
 * @post `name` is a framebuffer the GLCDC can scan out without tearing.
 * @post Roughly 1.2 MiB of external SDRAM is reserved, outside the image.
 *
 * @note Not thread-safe in any sense -- it is a declaration.
 * @warning Omitting the alignment is not a compile error and not a runtime
 *          error; it is torn scanout. Prefer this macro over hand-writing the
 *          attributes.
 *
 * @par Example:
 * @code
 * RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);
 * // ... later, at init:
 * (void)ra8_gfx_init(s_framebuffer, k_panel_width_px, k_panel_height_px);
 * @endcode
 *
 * @see k_ra8_board_fb_align_bytes  The alignment this applies.
 *
 * @since 0.1.0
 */
#define RA8_BOARD_PANEL_FRAMEBUFFER(name)                                                          \
  [[gnu::section(".sdram_data"), gnu::aligned(k_ra8_board_fb_align_bytes)]] static uint16_t        \
    name[(size_t)k_panel_height_px * (size_t)k_panel_width_px]
