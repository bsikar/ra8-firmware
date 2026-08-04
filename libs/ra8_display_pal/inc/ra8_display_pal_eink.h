/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_display_pal_eink.h
 * @brief E-ink backend (IT8951) for the display PAL
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Exports the const ``display_backend_iface_t`` instance that drives an
 * IT8951-compatible e-ink panel through ``libs/ra8_hal/src/ra8_epaper.c``.
 * Apps select this backend by setting
 * ``display_cfg_t.iface = &k_display_backend_eink_it8951;`` and pass the
 * BSP's IT8951 descriptor (a ``const ra8_epaper_cfg_t*``) through
 * ``display_cfg_t.panel_timing``. The backend accepts the same canonical
 * RGB565 framebuffer the LCD backend uses, so swapping panels is a config
 * change -- the app's drawing code is unchanged.
 *
 * On ``display_flush`` the backend converts the requested rectangle from
 * RGB565 to the IT8951's native 8 bpp greyscale (Rec.601 luma) and pushes
 * it to the controller, then refreshes with the waveform mapped from the
 * caller's ``display_refresh_hint_t`` (fast -> A2, quality -> GC16,
 * init -> INIT). On-panel HIL validation needs an IT8951 panel on the
 * bench; the vtable + conversion are host-verified against the
 * fake-backed ``ra8_epaper`` driver.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_display_pal.h"

/**
 * @var k_display_backend_eink_it8951
 * @brief E-ink (IT8951) backend vtable.
 *
 * @details
 * ``init`` brings up the IT8951 over SPI via ``ra8_epaper_init`` (using
 * the ``ra8_epaper_cfg_t`` supplied in ``display_cfg_t.panel_timing``);
 * ``get_framebuffer`` hands back the app's RGB565 buffer; ``flush``
 * converts the dirty rectangle to 8 bpp luma and refreshes the panel;
 * ``clear`` fills the framebuffer; ``deinit`` sleeps the panel.
 *
 * @since 0.1.0
 */
extern const display_backend_iface_t k_display_backend_eink_it8951;

/**
 * @brief Convert one RGB565 pixel to an 8 bpp greyscale (Rec.601 luma).
 *
 * @details
 * Internal-but-exposed so host unit tests can pin the conversion the
 * e-ink ``flush`` path relies on. Expands the 5/6/5 channels to 8 bits by
 * bit-replication, then applies the integer Rec.601 weights
 * (77/150/29, /256). Pure white maps to 255 and pure black to 0.
 *
 * @param[in] px Source pixel in RGB565 (little-endian field layout).
 *
 * @return The 8 bpp greyscale value.
 * @retval 0   For ``px == 0x0000`` (black).
 * @retval 255 For ``px == 0xFFFF`` (white).
 *
 * @pre  None.
 * @pre  None.
 * @post No state mutated.
 * @post Return depends solely on ``px``.
 *
 * @note Pure function; thread-safe.
 *
 * @since 0.1.0
 */
uint8_t ra8_display_pal_eink_luma_from_rgb565(uint16_t px);

#ifdef __cplusplus
}
#endif
