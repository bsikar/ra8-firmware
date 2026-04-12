/**
 * @file ra8d2_usb_regs.h
 * @brief USB FS / HS controller register bases for the Renesas RA8D2
 *
 * @details
 * - `R_USB_FS0_BASE` = `0x40250000` -- USB Full-Speed device/host.
 * - `R_USB_HS0_BASE` = `0x40351000` -- USB High-Speed device/host.
 *
 * This header only exposes the base addresses for now. USB register
 * detail will grow a lot once the first USB driver lands.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_usb_fs0_base_addr = 0x40250000UL,
  k_ra_usb_hs0_base_addr = 0x40351000UL,
} ra_usb_addr_t;

#ifdef __cplusplus
}
#endif
