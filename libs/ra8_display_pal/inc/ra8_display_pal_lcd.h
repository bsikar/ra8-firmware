/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_display_pal_lcd.h
 * @brief LCD backend (ra8_glcdc) for the display PAL
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Exports the const ``display_backend_iface_t`` instance that wraps
 * ``libs/ra8_hal/src/ra8_glcdc.c`` plus the EK-RA8D2 panel power-on
 * sequence. Apps select this backend by setting
 * ``display_cfg_t.iface = &k_display_backend_lcd_ra8_glcdc;`` -- the
 * rest of the PAL API is identical regardless of which backend is
 * bound.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_display_pal.h"

/**
 * @var k_display_backend_lcd_ra8_glcdc
 * @brief LCD backend vtable -- pass its address through
 *        ``display_cfg_t.iface`` to drive the EK-RA8D2 panel.
 *
 * @details
 * The vtable's ``init`` callback runs the full GLCDC bring-up:
 *
 *   1. ``ra8_board_lcd_panel_power_on`` -- toggle RESET_L low/high
 *      and assert BLEN.
 *   2. ``ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb888)`` -- pin
 *      and clock setup for the parallel-RGB bus.
 *   3. 200 ms settle delay (panel POR + pin direction).
 *   4. ``ra8_glcdc_init`` with the caller's framebuffer / dimensions.
 *   5. ``ra8_glcdc_set_background_color(0x000000)`` -- BG plane black.
 *   6. ``ra8_glcdc_start(true)`` -- enable scan-out.
 *   7. ``ra8_glcdc_layer1_show`` -- expose Graphics 1 over the FB.
 *
 * The matching ``deinit`` callback turns the panel back off.
 *
 * @note Only one display can be initialised at a time; calling
 *       ``display_init`` while a previous handle is still live
 *       returns ``k_ra8_err_busy``.
 *
 * @since 0.1.0
 */
extern const display_backend_iface_t k_display_backend_lcd_ra8_glcdc;

#ifdef __cplusplus
}
#endif
