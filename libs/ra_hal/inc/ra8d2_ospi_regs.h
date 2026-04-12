/**
 * @file ra8d2_ospi_regs.h
 * @brief xSPI / Octo-SPI controller register bases for the Renesas RA8D2
 *
 * @details
 * RA8D2 uses the "xSPI" controller (generalised Octa/Hex SPI) at
 * `0x40268000` and `0x40268400`. The EK-RA8D2 wires an xSPI flash
 * (64 MiB) to the first instance. The "DOTF" (Decryption-on-the-Fly)
 * block at `0x40268800` sits alongside for encrypted XIP execution.
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
  k_ra_xspi0_base_addr = 0x40268000UL,
  k_ra_xspi1_base_addr = 0x40268400UL,
  k_ra_dotf_base_addr  = 0x40268800UL,
} ra_xspi_addr_t;

typedef enum : uint8_t {
  k_ra_xspi_instance_count = 2U,
} ra_xspi_limits_t;

#ifdef __cplusplus
}
#endif
