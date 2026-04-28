/**
 * @file ra8d2_iwdt_regs.h
 * @brief Independent Watchdog Timer (IWDT) register layout for the RA8D2
 *
 * @details
 * The IWDT is a free-running 14-bit down-counter clocked from the
 * IWDT-dedicated oscillator (roughly 15 kHz), completely independent
 * of the CPU clock tree. Once started it cannot be stopped by software
 * -- the only way to prevent a reset is to refresh it (write the
 * sequence `0x00, 0xFF` to IWDTRR) before it reaches zero.
 *
 * Most of the IWDT configuration (period, window, reset vs. interrupt
 * output) lives in the `OFS0` option-setting register, which is read
 * out of flash at boot. This header only exposes the runtime refresh
 * and status registers.
 *
 * | Offset | Reg     | Width | RW | Purpose                        |
 * |-------:|---------|------:|----|--------------------------------|
 * | 0x00   | IWDTRR  | 8     | RW | Refresh Register               |
 * | 0x04   | IWDTSR  | 16    | R  | Status Register                |
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
  k_ra_iwdt_base_addr = 0x40202200UL,
} ra_iwdt_addr_t;

typedef enum : uint8_t {
  k_ra_iwdt_refresh_a = 0x00U, /**< First byte of the refresh sequence. */
  k_ra_iwdt_refresh_b = 0xFFU, /**< Second byte of the refresh sequence. */
  k_ra_iwdt_pad_bytes = 3U,    /**< Reserved padding after IWDTRR.      */
} ra_iwdt_refresh_t;

typedef struct {
  volatile uint8_t  IWDTRR; /**< +0x00 Refresh register.    */
  volatile uint8_t  _r0[k_ra_iwdt_pad_bytes];
  volatile uint16_t IWDTSR; /**< +0x04 Status register.     */
} r_iwdt_regs_t;

/** @brief Get pointer to the IWDT block. */
static inline volatile r_iwdt_regs_t* ra_iwdt(void)
{
  return (volatile r_iwdt_regs_t*)k_ra_iwdt_base_addr;
}

/**
 * @brief Refresh the IWDT.
 *
 * @details
 * Writes `0x00` followed by `0xFF` to IWDTRR, the mandatory two-byte
 * unlock sequence. Must happen strictly faster than the IWDT period
 * programmed in OFS0 or the next counter underflow resets the chip.
 *
 * @note Not thread-safe unless called from a single writer. For
 *       multi-task systems a real RTOS port should serialise via a
 *       per-task heartbeat aggregator.
 */
static inline void ra_iwdt_refresh(void)
{
  volatile r_iwdt_regs_t* w = ra_iwdt();
  w->IWDTRR                 = k_ra_iwdt_refresh_a;
  w->IWDTRR                 = k_ra_iwdt_refresh_b;
}

#ifdef __cplusplus
}
#endif
