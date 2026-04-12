/**
 * @file ra8d2_mpu_regs.h
 * @brief Memory Protection Unit register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 has TWO layers of memory protection:
 *
 *  1. The Cortex-M85 core MPU (architectural, programmable through
 *     `MPU_TYPE / MPU_CTRL / MPU_RBAR / MPU_RLAR` at address
 *     `0xE000ED90`+). Lives inside the System Control Space.
 *  2. The Renesas "MMPU" bus-level protection block at
 *     `0x40000000`, which enforces access rights independently of
 *     the core MPU. Useful for isolating DMA accesses.
 *
 * Plus a Stack Pointer Monitor (`R_MPU_SPMON`) at `0x40000D00` that
 * traps if the current MSP crosses a programmed boundary.
 *
 * This header only exposes the base addresses; full field layouts
 * come with the first consumer.
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
  k_ra_mpu_core_base_addr  = 0xE000ED90UL, /**< Cortex-M85 core MPU.  */
  k_ra_mpu_mmpu_base_addr  = 0x40000000UL, /**< Renesas MMPU.         */
  k_ra_mpu_spmon_base_addr = 0x40000D00UL, /**< Stack Pointer Monitor. */
} ra_mpu_addr_t;

#ifdef __cplusplus
}
#endif
