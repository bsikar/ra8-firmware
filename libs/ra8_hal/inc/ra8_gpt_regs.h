/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gpt_regs.h
 * @brief General PWM Timer (GPT) register layout for the Renesas RA8D2
 * @ingroup grp_hal_timers
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
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

typedef enum : uintptr_t {
  k_ra8_gpt0_base_addr    = 0x40322000UL, /**< RA8 gpt0 base address.  */
  k_ra8_gpt_gtclk_addr    = 0x40323F10UL, /**< Shared GTCLK regs.      */
  k_ra8_gpt_odc_base_addr = 0x40324000UL, /**< Output Disable Control. */
  k_ra8_gpt_ops_base_addr = 0x40323F00UL, /**< Output Phase Switching. */
} ra8_gpt_addr_t;

typedef enum : uint16_t {
  k_ra8_gpt_channel_count  = 14U,    /**< RA8 GPT channel count.  */
  k_ra8_gpt_channel_stride = 0x100U, /**< RA8 GPT channel stride. */
} ra8_gpt_limits_t;

/**
 * @struct r_gpt_channel_regs_t
 * @brief Per-channel GPT register window.
 *
 * @details
 * Mirrors the FSP CMSIS R_GPT0_Type layout for the RA8D2 (sized
 * 0xF0 in CMSIS). Only the registers we touch are unioned here;
 * fields that are not currently read or written by the driver
 * are still emitted at the right offset so future drivers can
 * extend the struct without re-deriving the map.
 *
 * HUM Ch 22 "General PWM Timer (GPT)", reg map summary, p 879.
 */
typedef struct {
  volatile uint32_t GTWP;          /**< +0x00 Write-Protection.               */
  volatile uint32_t GTSTR;         /**< +0x04 Software Start.                 */
  volatile uint32_t GTSTP;         /**< +0x08 Software Stop.                  */
  volatile uint32_t GTCLR;         /**< +0x0C Software Clear.                 */
  volatile uint32_t GTSSR;         /**< +0x10 Start source select.            */
  volatile uint32_t GTPSR;         /**< +0x14 Stop source select.             */
  volatile uint32_t GTCSR;         /**< +0x18 Clear source select.            */
  volatile uint32_t GTUPSR;        /**< +0x1C Up-count source select.         */
  volatile uint32_t GTDNSR;        /**< +0x20 Down-count source select.       */
  volatile uint32_t GTICASR;       /**< +0x24 Input capture A source select.  */
  volatile uint32_t GTICBSR;       /**< +0x28 Input capture B source select.  */
  volatile uint32_t GTCR;          /**< +0x2C Timer Control.                  */
  volatile uint32_t GTUDDTYC;      /**< +0x30 Count direction / duty.         */
  volatile uint32_t GTIOR;         /**< +0x34 I/O Control.                    */
  volatile uint32_t GTINTAD;       /**< +0x38 Interrupt / ADC trigger.        */
  volatile uint32_t GTST;          /**< +0x3C Status.                         */
  volatile uint32_t GTBER;         /**< +0x40 Buffer Enable.                  */
  volatile uint32_t GTITC;         /**< +0x44 Interrupt + ADC start skipping. */
  volatile uint32_t GTCNT;         /**< +0x48 Counter.                        */
  volatile uint32_t GTCCR[6];      /**< +0x4C..+0x60 Compare/Capture A..F.    */
  volatile uint32_t GTPR;          /**< +0x64 Period.                         */
  volatile uint32_t GTPBR;         /**< +0x68 Period buffer.                  */
  volatile uint32_t GTPDBR;        /**< +0x6C Period double-buffer.           */
  volatile uint32_t GTADTRA;       /**< +0x70 ADC start request timing A.     */
  volatile uint32_t GTADTBRA;      /**< +0x74 ADC start request timing buf A. */
  volatile uint32_t GTADTDBRA;     /**< +0x78 ADC start request timing dbl A. */
  volatile uint32_t GTADTRB;       /**< +0x7C ADC start request timing B.     */
  volatile uint32_t GTADTBRB;      /**< +0x80 ADC start request timing buf B. */
  volatile uint32_t GTADTDBRB;     /**< +0x84 ADC start request timing dbl B. */
  volatile uint32_t GTDTCR;        /**< +0x88 Dead Time Control.              */
  volatile uint32_t GTDVU;         /**< +0x8C Dead Time Value U.              */
  volatile uint32_t GTDVD;         /**< +0x90 Dead Time Value D.              */
  volatile uint32_t GTDBU;         /**< +0x94 Dead Time Buffer U.             */
  volatile uint32_t GTDBD;         /**< +0x98 Dead Time Buffer D.             */
  volatile uint32_t GTSOS;         /**< +0x9C Output Protection Status (RO).  */
  volatile uint32_t GTSOTR;        /**< +0xA0 Output Protection Trigger.      */
  volatile uint32_t GTADSMR;       /**< +0xA4 ADC start request skip mode.    */
  volatile uint32_t GTEITC;        /**< +0xA8 Extended interrupt skipping.    */
  volatile uint32_t GTEITLI1;      /**< +0xAC Extended interrupt skip link 1. */
  volatile uint32_t GTEITLI2;      /**< +0xB0 Extended interrupt skip link 2. */
  volatile uint32_t GTEITLB;       /**< +0xB4 Extended buffer-xfer skip.      */
  volatile uint32_t GTICLF;        /**< +0xB8 Inter-channel logical op.       */
  volatile uint32_t GTPC;          /**< +0xBC Period count.                   */
  volatile uint32_t GTADCMSC;      /**< +0xC0 ADC start request control.      */
  volatile uint32_t GTADCMSS;      /**< +0xC4 ADC start request status.       */
  volatile uint32_t _reserved0[2]; /**< +0xC8..+0xCC reserved.                */
  volatile uint32_t GTSECSR;       /**< +0xD0 Op-enable simultaneous control. */
  volatile uint32_t GTSECR;        /**< +0xD4 Op-enable simultaneous reg.     */
  volatile uint32_t _reserved1[2]; /**< +0xD8..+0xDC reserved.                */
  volatile uint32_t GTBER2;        /**< +0xE0 Buffer Enable 2.                */
  volatile uint32_t GTOLBR;        /**< +0xE4 Output Level Buffer.            */
  volatile uint32_t _reserved2;    /**< +0xE8 reserved.                       */
  volatile uint32_t GTICCR;        /**< +0xEC Inter-channel cooperation in.   */
} r_gpt_channel_regs_t;

/** @brief Get pointer to GPT channel N. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_gpt_channel_regs_t* ra8_gpt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_gpt_channel_count) {
    return nullptr;
  }
  return (
    volatile r_gpt_channel_regs_t*)(k_ra8_gpt0_base_addr +
                                    ((uintptr_t)channel * (uintptr_t)k_ra8_gpt_channel_stride));
}

#ifdef __cplusplus
}
#endif
