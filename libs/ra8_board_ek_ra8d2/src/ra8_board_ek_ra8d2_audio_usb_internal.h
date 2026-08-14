/**
 * @file ra8_board_ek_ra8d2_audio_usb_internal.h
 * @brief Private diagnostic surface for the EK-RA8D2 audio and USB BSP.
 *
 * @details
 * Declares the USB-HS bring-up trail shared by the production translation
 * unit, the host coverage test, and a J-Link bench session. Keeping the
 * declaration in this module-private header gives the externally linked
 * diagnostic object one authoritative type without adding it to the public
 * board API.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @var s_usbhs_probe
 * @brief USB-HS bring-up step visible to host tests and J-Link.
 *
 * @details
 * The board implementation advances this value around clock, module-stop,
 * and device-initialization operations. It is volatile because a debugger
 * reads it asynchronously and because every intermediate step must remain
 * observable.
 *
 * @note The board worker is the only writer; tests and J-Link only read it.
 * @since 0.1.0
 */
extern volatile uint32_t s_usbhs_probe;

#ifdef __cplusplus
}
#endif
