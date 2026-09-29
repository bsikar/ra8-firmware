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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_display_pal.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"

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

/**
 * @brief Bind the GLCDC backend to a framebuffer and a panel timing,
 *        with both halves typed.
 *
 * @details
 * The typed counterpart to filling a ``display_cfg_t`` by hand. The
 * caller supplies only what it owns -- its framebuffer and the panel
 * timing its board BSP publishes (e.g.
 * ``&s_ra8_panel_ek_ra8d2_timing``) -- and this helper supplies the
 * backend vtable, so a GLCDC bind cannot be handed an e-ink panel
 * descriptor through an untyped ``const void*``. The bring-up work
 * is unchanged: this composes a ``display_cfg_t`` and calls
 * ``display_init``, which dispatches into
 * ``k_display_backend_lcd_ra8_glcdc``'s 7-step init.
 *
 * @param[out] out    Filled with the PAL handle on success.
 * @param[in]  fb     Framebuffer storage and geometry.
 * @param[in]  timing Panel RGB timing for the parallel-RGB bus.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Panel is up; ``*out`` populated.
 * @retval k_ra8_err_null_ptr      ``out``, ``fb``, ``fb->pixels`` or
 *                                ``timing`` was NULL.
 * @retval k_ra8_err_invalid_arg   Geometry rejected, or ``fb->bytes``
 *                                too small for the geometry.
 * @retval k_ra8_err_busy          A display handle is already live.
 * @retval k_ra8_err_not_supported ``fb->pixfmt`` is not RGB565.
 *
 * @pre Clocks, MSTP and the system tick are up (the GLCDC bring-up
 *      delays and drives board GPIO).
 * @pre ``fb->pixels`` is alive and 64-byte aligned for AXI bursts.
 * @post On success the handle is valid until ``display_deinit``.
 * @post On any non-ok return ``*out`` is untouched.
 *
 * @note Not thread-safe; single-shot startup helper.
 * @note ``timing`` is read during the call only; the backend
 *       snapshots what it needs, so it need not outlive the call.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_pal_bind_glcdc(display_handle_t**        out,
                                               const display_fb_cfg_t*   fb,
                                               const ra8_glcdc_timing_t* timing);

#ifdef __cplusplus
}
#endif
