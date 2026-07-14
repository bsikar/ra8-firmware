/**
 * @file ra8_panel_timing.h
 * @brief EK-RA8D2 panel timing as the HAL's ra8_glcdc_timing_t (ER-TFT070-6)
 * @ingroup grp_board
 *
 * @details
 * Assembles the named panel constants from ra8_panel.h into the HAL's
 * ra8_glcdc_timing_t so callers can hand the EK-RA8D2 panel's timing to the
 * (panel-agnostic) GLCDC driver. Pass &k_ra8_panel_ek_ra8d2_timing into
 * ra8_glcdc_config_t.timing directly, or into display_cfg_t.panel_timing for
 * the LCD display-PAL backend.
 *
 * Unlike the geometry-only ra8_panel.h, this header pulls in the HAL type
 * (ra8_glcdc.h), so it is target-side -- not for host previews.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include "ra8_glcdc.h"
#include "ra8_panel.h"

/**
 * @brief EK-RA8D2 ER-TFT070-6 RGB timing for ra8_glcdc_init / the LCD backend.
 */
static const ra8_glcdc_timing_t k_ra8_panel_ek_ra8d2_timing = {
  .h_active = (uint16_t)k_panel_width_px,
  .h_front  = (uint16_t)k_panel_h_front,
  .h_back   = (uint16_t)k_panel_h_back,
  .h_sync   = (uint16_t)k_panel_h_sync,
  .v_active = (uint16_t)k_panel_height_px,
  .v_front  = (uint16_t)k_panel_v_front,
  .v_back   = (uint16_t)k_panel_v_back,
  .v_sync   = (uint16_t)k_panel_v_sync,
};
