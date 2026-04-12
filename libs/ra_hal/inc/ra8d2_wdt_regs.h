/**
 * @file ra8d2_wdt_regs.h
 * @brief Watchdog Timer (WDT) register layout for the RA8D2
 *
 * @details
 * The WDT is the *software*-clocked companion to the IWDT. Where the
 * IWDT runs from its own oscillator and cannot be stopped, the WDT
 * shares PCLKB with the rest of the peripherals and can be halted by
 * debug. Use the IWDT for safety resets and the WDT for application
 * liveness checks that should pause when the debugger halts.
 *
 * | Offset | Reg   | Width | RW | Purpose              |
 * |-------:|-------|------:|----|----------------------|
 * | 0x00   | WDTRR | 8     | RW | Refresh register      |
 * | 0x02   | WDTCR | 16    | RW | Control register      |
 * | 0x04   | WDTSR | 16    | R  | Status register       |
 * | 0x06   | WDTRCR| 8     | RW | Reset control         |
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
  k_ra_wdt_base_addr = 0x40202600UL,
} ra_wdt_addr_t;

typedef enum : uint8_t {
  k_ra_wdt_refresh_a = 0x00U,
  k_ra_wdt_refresh_b = 0xFFU,
} ra_wdt_refresh_t;

typedef struct {
  volatile uint8_t  WDTRR; /**< +0x00 Refresh register.  */
  volatile uint8_t  _r0;
  volatile uint16_t WDTCR;  /**< +0x02 Control register.  */
  volatile uint16_t WDTSR;  /**< +0x04 Status register.   */
  volatile uint8_t  WDTRCR; /**< +0x06 Reset control.    */
} r_wdt_regs_t;

/** @brief Get pointer to the WDT block. */
static inline volatile r_wdt_regs_t* ra_wdt(void)
{
  return (volatile r_wdt_regs_t*)k_ra_wdt_base_addr;
}

/** @brief Refresh the WDT using the two-byte unlock sequence. */
static inline void ra_wdt_refresh(void)
{
  volatile r_wdt_regs_t* w = ra_wdt();
  w->WDTRR                 = (uint8_t)k_ra_wdt_refresh_a;
  w->WDTRR                 = (uint8_t)k_ra_wdt_refresh_b;
}

#ifdef __cplusplus
}
#endif
