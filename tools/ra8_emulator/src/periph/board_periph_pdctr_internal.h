/**
 * @file board_periph_pdctr_internal.h
 * @brief Module-private graphics power-domain state for the gated blocks
 *
 * @details
 * The RA8D2 graphics power domain (MIPI DSI, MIPI CSI, VIN, DRW, GLCDC --
 * HUM Ch 11.5.1 Table 11.7 p 480) is gated OFF at reset by @c PDCTRGD
 * (HUM Ch 11.2.14 p 452). While it is gated, the peripherals inside it are
 * unpowered: their registers read 0 and their writes are lost.
 *
 * @c board_periph_pdctr.c owns @c PDCTRGD; the blocks that model peripherals
 * inside the domain ask ::board_pdctr_graphics_powered before reporting a
 * live register value, so the emulator shows a dark domain exactly as the
 * bench does. This header is the seam between them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report whether the graphics power domain is currently powered.
 *
 * @details
 * Tracks @c PDCTRGD.PDPGSF: false while the domain is gated off (the reset
 * state), true once firmware has cleared @c PDDE with PRCR.PRC1 unlocked.
 *
 * @return @c true when the graphics domain is powered on.
 * @retval true  @c PDPGSF == 0: DRW / GLCDC / MIPI / VIN are alive.
 * @retval false Domain still gated; those peripherals read 0.
 *
 * @pre The PDCTRGD block is registered (host constructor, before main).
 * @post No state is mutated.
 *
 * @note Not thread-safe; ra8_emulator drives all blocks from one thread.
 * @since 0.1.0
 */
RA8_PRIV bool board_pdctr_graphics_powered(void);

#ifdef __cplusplus
}
#endif
