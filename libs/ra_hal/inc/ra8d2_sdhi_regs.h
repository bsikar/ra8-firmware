/**
 * @file ra8d2_sdhi_regs.h
 * @brief SDHI (SD host interface) base addresses for the Renesas RA8D2
 *
 * @details
 * Two SDHI instances at `0x40252000` (SDHI0) and `0x40252400`
 * (SDHI1). This header only exposes the base addresses today; full
 * register struct lands with the first SD card driver.
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
  k_ra_sdhi0_base_addr = 0x40252000UL,
  k_ra_sdhi1_base_addr = 0x40252400UL,
} ra_sdhi_addr_t;

#ifdef __cplusplus
}
#endif
