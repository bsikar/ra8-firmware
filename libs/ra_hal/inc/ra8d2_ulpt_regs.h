/**
 * @file ra8d2_ulpt_regs.h
 * @brief Ultra-Low-Power Timer (ULPT) register layout for the Renesas RA8D2
 *
 * @details
 * The Ultra-Low-Power Timer is a 32-bit down-counter that can run on
 * the sub-clock or LOCO in deep-sleep modes. RA8D2 exposes two
 * channels (ULPT0 and ULPT1) at base `0x40041000` with a `0x100`
 * stride per instance.
 *
 * Per-channel registers (partial):
 *
 * | Offset | Name    | Width | Purpose                            |
 * |-------:|---------|------:|------------------------------------|
 * |  0x00  | ULPTMR1 |     8 | Mode 1                             |
 * |  0x01  | ULPTMR2 |     8 | Mode 2                             |
 * |  0x02  | ULPTMR3 |     8 | Mode 3                             |
 * |  0x04  | ULPTIOC |     8 | I/O control                        |
 * |  0x08  | ULPT    |    32 | Counter / reload                   |
 * |  0x0C  | ULPTCR  |     8 | Control (start / stop / flags)     |
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

/**
 * @enum ra_ulpt_addr_t
 * @brief Memory-mapped base addresses for ULPT.
 */
typedef enum : uintptr_t {
  k_ra_ulpt0_base_addr = 0x40041000UL, /**< ULPT channel 0 base. */
} ra_ulpt_addr_t;

/**
 * @enum ra_ulpt_limits_t
 * @brief Channel count and stride.
 */
typedef enum : uint16_t {
  k_ra_ulpt_channel_count  = 2U,     /**< ULPT0 and ULPT1.          */
  k_ra_ulpt_channel_stride = 0x100U, /**< Bytes between channels.   */
} ra_ulpt_limits_t;

/**
 * @enum ra_ulptcr_bit_t
 * @brief ULPTCR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_ulpt_bit_tstart = 0U, /**< 1 = start count, 0 = stop.            */
  k_ra_ulpt_bit_tcstf  = 1U, /**< Count-status flag (read-only).        */
  k_ra_ulpt_bit_tstop  = 2U, /**< Forced stop bit.                      */
  k_ra_ulpt_bit_tunf   = 4U, /**< Underflow flag.                       */
  k_ra_ulpt_bit_tedga  = 5U, /**< Edge detect flag A.                   */
  k_ra_ulpt_bit_tedgb  = 6U, /**< Edge detect flag B.                   */
} ra_ulptcr_bit_t;

/**
 * @enum ra_ulpt_mask_t
 * @brief ULPTCR bit masks.
 */
typedef enum : uint8_t {
  k_ra_ulpt_mask_tstart = 0x01U, /**< TSTART bit. */
  k_ra_ulpt_mask_tstop  = 0x04U, /**< TSTOP bit.  */
} ra_ulpt_mask_t;

/**
 * @struct r_ulpt_regs_t
 * @brief Per-channel ULPT register window.
 */
typedef struct {
  volatile uint8_t  ULPTMR1; /**< +0x00 Mode register 1.                 */
  volatile uint8_t  ULPTMR2; /**< +0x01 Mode register 2.                 */
  volatile uint8_t  ULPTMR3; /**< +0x02 Mode register 3.                 */
  volatile uint8_t  _r0;     /**< Padding.                               */
  volatile uint8_t  ULPTIOC; /**< +0x04 I/O control.                     */
  volatile uint8_t  _r1[3];  /**< Padding.                               */
  volatile uint32_t ULPT;    /**< +0x08 Counter / reload (32-bit).       */
  volatile uint8_t  ULPTCR;  /**< +0x0C Control register.                */
  volatile uint8_t  _r2[3];  /**< Padding.                               */
} r_ulpt_regs_t;

/**
 * @brief Get pointer to ULPT channel `channel`.
 * @param[in] channel Channel index (0..1).
 * @return Volatile pointer, or `nullptr` if `channel` is out of range.
 */
static inline volatile r_ulpt_regs_t* ra_ulpt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return nullptr;
  }
  return (volatile r_ulpt_regs_t*)(k_ra_ulpt0_base_addr +
                                   ((uintptr_t)channel * (uintptr_t)k_ra_ulpt_channel_stride));
}

#ifdef __cplusplus
}
#endif
