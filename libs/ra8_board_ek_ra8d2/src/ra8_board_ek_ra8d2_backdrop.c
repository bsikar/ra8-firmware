/**
 * @file ra8_board_ek_ra8d2_backdrop.c
 * @brief Solid-colour panel backdrop for the EK-RA8D2 v1 board.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Implements ``ra8_board_ek_ra8d2_backdrop.h``. Pure translation: the board's
 * own panel geometry and timing into one display-controller configuration, no
 * register pokes of its own, the same contract every other file in this layer
 * keeps.
 *
 * The configured pixel format describes the graphics layers, which this path
 * never enables, so it has no effect on what the panel shows. It is stated
 * anyway because the controller validates the field.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_backdrop.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"

/** @brief Address handed to the controller when no framebuffer is in play. */
static const uintptr_t k_backdrop_no_framebuffer = 0UL;

ra8_err_t ra8_board_panel_backdrop_begin(uint32_t rgb888)
{
  const ra8_glcdc_config_t cfg = {
    .framebuffer_addr = k_backdrop_no_framebuffer,
    .width_px         = (uint16_t)k_panel_width_px,
    .height_px        = (uint16_t)k_panel_height_px,
    .format           = k_ra8_glcdc_fmt_rgb565,
    .timing           = s_ra8_panel_ek_ra8d2_timing,
  };
  ra8_err_t err = ra8_glcdc_init(&cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_glcdc_set_background_color(rgb888);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_glcdc_start(true);
}

ra8_err_t ra8_board_panel_backdrop_set(uint32_t rgb888)
{
  return ra8_glcdc_set_background_color(rgb888);
}
