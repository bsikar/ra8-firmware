/**
 * @file ra8d2_gpt_regs.h
 * @brief General PWM Timer (GPT) register layout for the Renesas RA8D2
 *
 * @details
 * RA8D2 exposes 14 GPT channels (GPT0..GPT13) at `0x40322000` with a
 * `0x100` stride per channel. Each channel is a 32-bit up/down
 * counter with independent GTIOA/GTIOB outputs, PWM / compare-match
 * modes, dead-time control, and ADC trigger hooks. A small set of
 * shared registers (GTCLK, GTODC, GTOPS) lives at higher offsets.
 *
 * The per-channel struct below models the most frequently touched
 * registers; extend as drivers need.
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
  k_ra_gpt0_base_addr    = 0x40322000UL,
  k_ra_gpt_gtclk_addr    = 0x40323F10UL, /**< Shared GTCLK regs. */
  k_ra_gpt_odc_base_addr = 0x40324000UL, /**< Output Disable Control. */
  k_ra_gpt_ops_base_addr = 0x40323F00UL, /**< Output Phase Switching. */
} ra_gpt_addr_t;

typedef enum : uint16_t {
  k_ra_gpt_channel_count  = 14U,
  k_ra_gpt_channel_stride = 0x100U,
} ra_gpt_limits_t;

/**
 * @struct r_gpt_channel_regs_t
 * @brief Per-channel GPT register window (subset).
 */
typedef struct {
  volatile uint32_t GTWP;     /**< +0x00 Write-Protection.               */
  volatile uint32_t GTSTR;    /**< +0x04 Start / Stop.                    */
  volatile uint32_t GTSTP;    /**< +0x08 Stop (write 1 stop).             */
  volatile uint32_t GTCLR;    /**< +0x0C Clear counter.                   */
  volatile uint32_t GTSSR;    /**< +0x10 Start source select.             */
  volatile uint32_t GTPSR;    /**< +0x14 Stop  source select.             */
  volatile uint32_t GTCSR;    /**< +0x18 Clear source select.             */
  volatile uint32_t GTUPSR;   /**< +0x1C Up-count source select.          */
  volatile uint32_t GTDNSR;   /**< +0x20 Down-count source select.        */
  volatile uint32_t GTICASR;  /**< +0x24 Input capture A source select.   */
  volatile uint32_t GTICBSR;  /**< +0x28 Input capture B source select.   */
  volatile uint32_t GTCR;     /**< +0x2C Timer Control.                   */
  volatile uint32_t GTUDDTYC; /**< +0x30 Count direction / duty.          */
  volatile uint32_t GTIOR;    /**< +0x34 I/O Control.                     */
  volatile uint32_t GTINTAD;  /**< +0x38 Interrupt / ADC trigger.         */
  volatile uint32_t GTST;     /**< +0x3C Status.                          */
  volatile uint32_t GTBER;    /**< +0x40 Buffer Enable.                   */
  volatile uint32_t _r0;
  volatile uint32_t GTCNT;    /**< +0x48 Counter.                         */
  volatile uint32_t GTCCR[6]; /**< +0x4C..+0x60 Compare/Capture A..F.     */
  volatile uint32_t GTPR;     /**< +0x64 Period.                          */
  volatile uint32_t GTPBR;    /**< +0x68 Period buffer.                   */
} r_gpt_channel_regs_t;

/** @brief Get pointer to GPT channel N. */
static inline volatile r_gpt_channel_regs_t* ra_gpt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_gpt_channel_count) {
    return nullptr;
  }
  return (
    volatile r_gpt_channel_regs_t*)(k_ra_gpt0_base_addr +
                                    ((uintptr_t)channel * (uintptr_t)k_ra_gpt_channel_stride));
}

#ifdef __cplusplus
}
#endif
