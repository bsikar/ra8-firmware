/**
 * @file ra8_board_ek_ra8d2_backdrop.h
 * @brief Solid-colour panel backdrop (GLCDC BG plane) for the EK-RA8D2 v1
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Drives the J1 parallel-RGB panel with a flat colour and no framebuffer at
 * all. The display controller composes its output as background x layer2 x
 * layer1; with both graphics layers held invisible the background plane fills
 * the whole active area on its own, so nothing has to allocate or scan out
 * 1.2 MiB of SDRAM to put a colour on the glass.
 *
 * That is worth a board seam rather than an application recipe because every
 * value it needs is a board fact already stated in this layer: the panel
 * geometry in ``ra8_panel.h``, its RGB timing in ``ra8_panel_timing.h``, the
 * J1 pin routing in ``ra8_board_glcdc_init``. An application reaching for the
 * controller directly has to restate all three, and the two that are easy to
 * get subtly wrong (porches and the no-framebuffer convention) produce a dark
 * panel rather than an error.
 *
 * Intended users are bring-up, test and panic paths: prove the panel lights
 * before any graphics stack exists, flash a colour on an unrecoverable fault,
 * blank to black without tearing the display stack down. Anything drawing
 * pixels wants the display PAL instead.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Start the panel showing a single flat colour.
 *
 * @details
 * Configures the display controller for this board's panel with no
 * framebuffer, sets @p rgb888 as the background and starts scanout. Pin
 * routing and panel power are NOT done here: call
 * ::ra8_board_lcd_panel_power_on and ::ra8_board_glcdc_init first, in that
 * order, exactly as a framebuffer-backed bring-up would.
 *
 * @param[in] rgb888 Initial colour, bits[23:16] red, [15:8] green, [7:0] blue.
 *                   Bits[31:24] are reserved and ignored.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Panel is scanning out @p rgb888.
 * @retval k_ra8_err_invalid_arg  Controller rejected the panel timing.
 * @retval k_ra8_err_invalid_state Controller was already running.
 *
 * @pre ::ra8_board_lcd_panel_power_on has run (panel out of reset, backlight on).
 * @pre ::ra8_board_glcdc_init has routed the J1 pins.
 * @post The panel displays @p rgb888 across its whole active area.
 * @post No framebuffer memory is read; both graphics layers stay invisible.
 *
 * @note Not thread-safe; single-shot bring-up helper.
 * @see ra8_board_panel_backdrop_set  Change the colour afterwards.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_panel_backdrop_begin(uint32_t rgb888);

/**
 * @brief Change the flat colour the panel is showing.
 *
 * @details
 * The background colour register is shadowed: the write lands at the next
 * vertical sync, so the panel never shows a partially updated colour and the
 * call does not block waiting for one.
 *
 * @param[in] rgb888 New colour, bits[23:16] red, [15:8] green, [7:0] blue.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Colour accepted; it appears at the next sync.
 * @retval k_ra8_err_invalid_state ::ra8_board_panel_backdrop_begin has not run.
 *
 * @pre ::ra8_board_panel_backdrop_begin has run successfully.
 * @post The panel shows @p rgb888 from the next vertical sync onward.
 * @post No other panel or controller state changes.
 *
 * @note Not thread-safe; call from one context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_panel_backdrop_set(uint32_t rgb888);
