/**
 * @file ra8d2_flash_regs.h
 * @brief Flash / MRAM controller register layout for the Renesas RA8D2
 *
 * @details
 * On RA8D2 the primary non-volatile code memory is **MRAM**, not
 * traditional NOR flash, but the controller interface follows the RA
 * flash IP closely. Writes go through a small "FACI" command register
 * sequence; reads are transparent.
 *
 * @note The FSP documentation lists the controller base as part of
 *       `R_FACI_HP` / `R_FACI_LP`. The exact set of registers
 *       differs from non-MRAM RA parts; values here come from the
 *       RA8D2 Hardware User's Manual chapter 41 ("MRAM Memory").
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
  k_ra_flash_code_start = 0x02000000UL, /**< Start of MRAM code region. */
  k_ra_flash_code_size  = 0x00100000UL, /**< 1 MiB of MRAM.             */
} ra_flash_map_t;

typedef enum : uint32_t {
  k_ra_mram_write_size_bytes = 32UL, /**< Minimum programmable unit. */
} ra_mram_write_t;

#ifdef __cplusplus
}
#endif
