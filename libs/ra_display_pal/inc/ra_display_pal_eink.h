/**
 * @file ra_display_pal_eink.h
 * @brief E-ink backend (IT8951) for the display PAL -- stub
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Exports the const ``display_backend_iface_t`` instance that will
 * drive an IT8951-compatible e-ink panel once the hardware is on
 * the bench. Apps select this backend by setting
 * ``display_cfg_t.iface = &k_display_backend_eink_it8951;`` -- the
 * scaffolding accepts the same canonical RGB565 framebuffer
 * dimensions the LCD backend accepts so a future swap is a single
 * line change.
 *
 * ## Current status
 *
 * Stub. ``init`` validates the cfg and parks state. ``get_caps``
 * reports the framebuffer's stride. Every other vtable entry
 * returns ``k_ra_err_not_supported`` until the SPI / waveform
 * driver lands. The end-to-end API surface is exercised by host
 * unit tests so the contract is locked in advance.
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

#include "ra_display_pal.h"

/**
 * @var k_display_backend_eink_it8951
 * @brief E-ink backend vtable -- stub today.
 *
 * @details
 * Once wired to silicon, this backend will:
 *
 *   1. Bring up the SPI bus the IT8951 is attached to.
 *   2. Read the controller's panel-info block.
 *   3. Reserve a host-side intermediate buffer for the RGB565 -> 4bpp
 *      luma conversion done on every ``display_flush``.
 *   4. Map ``display_refresh_hint_t`` onto INIT / GC16 / GL16 / A2
 *      waveform modes (see HUM-equivalent IT8951 application note).
 *
 * For now ``display_flush`` and ``display_get_framebuffer`` return
 * ``k_ra_err_not_supported``; ``display_init`` and ``display_get_caps``
 * succeed so apps can verify the swap works at compile + link time.
 *
 * @since 0.1.0
 */
extern const display_backend_iface_t k_display_backend_eink_it8951;

#ifdef __cplusplus
}
#endif
