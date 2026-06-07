/**
 * @file sim_panel.h
 * @brief Display descriptor loaded from a per-panel TOML config
 *
 * @details
 * The simulator is display-agnostic: it becomes whatever panel a small TOML
 * config describes, so a new display is a new file under tools/simulator/panels/
 * -- no rebuild. Flat key = value pairs only (a TOML subset), parsed by a
 * dependency-free bounded reader.
 *
 * Example (tools/simulator/panels/ek_ra8d2.toml):
 * @code
 *   name   = "ER-TFT070-6"
 *   width  = 1024
 *   height = 600
 *   format = "rgb565"
 *   ppi    = 169
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum sim_pixfmt_t
 * @brief Pixel format a simulated panel renders in.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_sim_pixfmt_unknown = 0U, /**< Unset / unrecognised.            */
  k_sim_pixfmt_rgb565  = 1U, /**< 16bpp colour (LCD).              */
  k_sim_pixfmt_gray4   = 2U, /**< 4bpp greyscale (e-paper; v2).    */
} sim_pixfmt_t;

/**
 * @enum sim_panel_limits_t
 * @brief Field bounds for a loaded panel (keeps storage static, no alloc).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_sim_panel_name_max = 48U,   /**< Max panel-name bytes incl NUL. */
  k_sim_panel_max_w    = 2048U, /**< Max simulated width  (px).     */
  k_sim_panel_max_h    = 1536U, /**< Max simulated height (px).     */
} sim_panel_limits_t;

/**
 * @struct sim_panel_t
 * @brief A simulated display's geometry + format + physical size.
 *
 * @since 0.1.0
 */
typedef struct {
  char         name[k_sim_panel_name_max]; /**< Human-readable part number.    */
  uint16_t     width_px;                   /**< Native width  (pixels).        */
  uint16_t     height_px;                  /**< Native height (pixels).        */
  uint16_t     ppi;                        /**< Pixels per inch (0 = unknown). */
  sim_pixfmt_t format;                     /**< Pixel format.                  */
} sim_panel_t;

/**
 * @brief Load a panel descriptor from a TOML config file.
 *
 * @param[in]  path TOML file path.
 * @param[out] out  Filled descriptor on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            Loaded; width/height/format valid.
 * @retval k_ra_err_null_ptr  @p path or @p out was NULL.
 * @retval k_ra_err_invalid_arg File missing, unreadable, or required fields
 *                              absent / out of range.
 *
 * @since 0.1.0
 */
ra_err_t sim_panel_load(const char* path, sim_panel_t* out);

/**
 * @brief Human-readable name for a pixel format.
 *
 * @param[in] fmt Pixel format.
 * @return Static string ("rgb565", "gray4", or "unknown").
 *
 * @since 0.1.0
 */
const char* sim_pixfmt_str(sim_pixfmt_t fmt);
