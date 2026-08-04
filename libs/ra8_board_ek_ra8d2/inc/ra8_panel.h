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
