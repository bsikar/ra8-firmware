/**
 * @file ra8_ulpt_regs.h
 * @brief Ultra-Low-Power Timer (ULPT) register layout for the Renesas RA8D2
 * @ingroup grp_hal_timers
 *
 * @details
 * The Ultra-Low-Power Timer is a 32-bit down-counter that can run on
 * the sub-clock or LOCO in deep-sleep modes. RA8D2 exposes two
 * channels (ULPT0 and ULPT1) at 0x40220000 / 0x40220100 (stride
 * 0x100) per FSP R_ULPT0_BASE / R_ULPT1_BASE.
 *
 * Per-channel register map (verified against FSP R_ULPT0_Type in
 * R7KA8D2KF_core0.h lines 30865-31006):
 *
 *   | Offset | Name    | Width | Purpose                         |
 *   |-------:|---------|------:|---------------------------------|
 *   |  0x00  | ULPTCNT |  32   | 32-bit counter / reload         |
 *   |  0x04  | ULPTCMA |  32   | Compare match A                 |
 *   |  0x08  | ULPTCMB |  32   | Compare match B                 |
 *   |  0x0C  | ULPTCR  |   8   | Control (start / stop / flags)  |
 *   |  0x0D  | ULPTMR1 |   8   | Mode register 1                 |
 *   |  0x0E  | ULPTMR2 |   8   | Mode register 2                 |
 *   |  0x0F  | ULPTMR3 |   8   | Mode register 3                 |
 *   |  0x10  | ULPTIOC |   8   | I/O control                     |
 *
 * Register offsets tracked against HUM Ch 25 "Ultra-Low-Power
 * Timer (ULPT)" p 1187.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_ulpt_addr_t
 * @brief Memory-mapped base addresses for ULPT.
 */
typedef enum : uintptr_t {
  k_ra8_ulpt0_base_addr = 0x40220000UL, /**< ULPT0 base (FSP R_ULPT0_BASE). */
  k_ra8_ulpt1_base_addr = 0x40220100UL, /**< ULPT1 base (FSP R_ULPT1_BASE). */
} ra8_ulpt_addr_t;

/**
 * @enum ra8_ulpt_limits_t
 * @brief Channel count and stride.
 */
typedef enum : uint16_t {
  k_ra8_ulpt_channel_count  = 2U,     /**< ULPT0 and ULPT1.        */
  k_ra8_ulpt_channel_stride = 0x100U, /**< Bytes between channels. */
} ra8_ulpt_limits_t;

/**
 * @enum ra8_ulptcr_bit_t
 * @brief ULPTCR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra8_ulpt_bit_tstart = 0U, /**< 1 = start count, 0 = stop.     */
  k_ra8_ulpt_bit_tcstf  = 1U, /**< Count-status flag (read-only). */
  k_ra8_ulpt_bit_tstop  = 2U, /**< Forced stop bit.               */
  k_ra8_ulpt_bit_tunf   = 4U, /**< Underflow flag.                */
  k_ra8_ulpt_bit_tedga  = 5U, /**< Edge detect flag A.            */
  k_ra8_ulpt_bit_tedgb  = 6U, /**< Edge detect flag B.            */
} ra8_ulptcr_bit_t;

/**
 * @enum ra8_ulpt_mask_t
 * @brief ULPTCR bit masks.
 */
typedef enum : uint8_t {
  k_ra8_ulpt_mask_tstart = 0x01U, /**< TSTART bit.                   */
  k_ra8_ulpt_mask_tcstf  = 0x02U, /**< TCSTF count-status flag (RO). */
  k_ra8_ulpt_mask_tstop  = 0x04U, /**< TSTOP bit.                    */
} ra8_ulpt_mask_t;

/**
 * @struct r_ulpt_regs_t
 * @brief Per-channel ULPT register window.
 */
typedef struct {
  volatile uint32_t ULPTCNT; /**< +0x00 Counter / reload (32-bit). */
  volatile uint32_t ULPTCMA; /**< +0x04 Compare match A (32-bit).  */
  volatile uint32_t ULPTCMB; /**< +0x08 Compare match B (32-bit).  */
  volatile uint8_t  ULPTCR;  /**< +0x0C Control register.          */
  volatile uint8_t  ULPTMR1; /**< +0x0D Mode register 1.           */
  volatile uint8_t  ULPTMR2; /**< +0x0E Mode register 2.           */
  volatile uint8_t  ULPTMR3; /**< +0x0F Mode register 3.           */
  volatile uint8_t  ULPTIOC; /**< +0x10 I/O control.               */
} r_ulpt_regs_t;

/**
 * @brief Get pointer to ULPT channel `channel`.
 * @param[in] channel Channel index (0..1).
 * @return Volatile pointer, or `nullptr` if `channel` is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_ulpt_regs_t* ra8_ulpt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ulpt_channel_count) {
    return nullptr;
  }
  return (volatile r_ulpt_regs_t*)(k_ra8_ulpt0_base_addr +
                                   ((uintptr_t)channel * (uintptr_t)k_ra8_ulpt_channel_stride));
}

#ifdef __cplusplus
}
#endif
