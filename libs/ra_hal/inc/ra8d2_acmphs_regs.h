/**
 * @file ra8d2_acmphs_regs.h
 * @brief High-Speed Analog Comparator (ACMPHS) register layout for the RA8D2
 *
 * @details
 * The RA8D2 exposes up to six high-speed analog comparator channels
 * (ACMPHS0..ACMPHS5). Each channel has a small independent register
 * window at base `0x40324000` with a `0x100` stride.
 *
 * Per-channel registers:
 *
 * | Offset | Name     | Width | Purpose                              |
 * |-------:|----------|------:|--------------------------------------|
 * |  0x00  | CMPCTL   | 8     | Comparator control (HCMON, HCEN, ...)|
 * |  0x04  | CMPSEL0  | 8     | Plus-input selection                 |
 * |  0x08  | CMPSEL1  | 8     | Minus-input selection                |
 * |  0x0C  | CMPMON   | 8     | Output monitor (read-only)           |
 * |  0x10  | CPIOC    | 8     | Output polarity / pin control        |
 * |  0x14  | CMPFIR   | 8     | Digital filter / noise reject        |
 *
 * Register offsets tracked against HUM Ch 56 "High-Speed Analog
 * Comparator (ACMPHS)" p 3508.
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
 * @enum ra_acmphs_addr_t
 * @brief Memory-mapped base addresses for ACMPHS.
 */
typedef enum : uintptr_t {
  k_ra_acmphs0_base_addr = 0x40324000UL, /**< ACMPHS channel 0 base. */
} ra_acmphs_addr_t;

/**
 * @enum ra_acmphs_limits_t
 * @brief Channel count and stride.
 */
typedef enum : uint16_t {
  k_ra_acmphs_channel_count  = 6U,     /**< Channels 0..5 on this package. */
  k_ra_acmphs_channel_stride = 0x100U, /**< Bytes between channels.        */
} ra_acmphs_limits_t;

/**
 * @enum ra_cmpctl_bit_t
 * @brief CMPCTL control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_acmphs_bit_hcmon = 0U, /**< Mirror of the comparator output. */
  k_ra_acmphs_bit_cinv  = 1U, /**< Output polarity inversion.        */
  k_ra_acmphs_bit_coe   = 2U, /**< Output enable (to pin).           */
  k_ra_acmphs_bit_cedg0 = 3U, /**< Edge detect select bit 0.         */
  k_ra_acmphs_bit_cedg1 = 4U, /**< Edge detect select bit 1.         */
  k_ra_acmphs_bit_hcen  = 5U, /**< Comparator enable.                */
  k_ra_acmphs_bit_cdfs0 = 6U, /**< Digital filter select bit 0.      */
  k_ra_acmphs_bit_cdfs1 = 7U, /**< Digital filter select bit 1.      */
} ra_cmpctl_bit_t;

/**
 * @enum ra_acmphs_mask_t
 * @brief CMPCTL bit masks.
 */
typedef enum : uint8_t {
  k_ra_acmphs_mask_hcmon = 0x01U, /**< HCMON bit.   */
  k_ra_acmphs_mask_hcen  = 0x20U, /**< HCEN bit.    */
  k_ra_acmphs_mask_coe   = 0x04U, /**< COE bit.     */
} ra_acmphs_mask_t;

/**
 * @struct r_acmphs_regs_t
 * @brief Per-channel ACMPHS register window.
 */
typedef struct {
  volatile uint8_t CMPCTL;  /**< +0x00 Control (enable, edges, filter).     */
  volatile uint8_t _r0[3];  /**< Padding.                                   */
  volatile uint8_t CMPSEL0; /**< +0x04 Plus input selection.                */
  volatile uint8_t _r1[3];  /**< Padding.                                   */
  volatile uint8_t CMPSEL1; /**< +0x08 Minus input selection.               */
  volatile uint8_t _r2[3];  /**< Padding.                                   */
  volatile uint8_t CMPMON;  /**< +0x0C Output monitor (read-only).          */
  volatile uint8_t _r3[3];  /**< Padding.                                   */
  volatile uint8_t CPIOC;   /**< +0x10 Output polarity / pin control.       */
  volatile uint8_t _r4[3];  /**< Padding.                                   */
  volatile uint8_t CMPFIR;  /**< +0x14 Digital filter / noise reject.       */
  volatile uint8_t _r5[3];  /**< Padding.                                   */
} r_acmphs_regs_t;

/**
 * @brief Get pointer to ACMPHS channel `channel`.
 * @param[in] channel Channel index (0..5).
 * @return Volatile pointer to the channel register block, or `nullptr`
 *         if `channel` is out of range.
 */
static inline volatile r_acmphs_regs_t* ra_acmphs(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_acmphs_channel_count) {
    return nullptr;
  }
  return (volatile r_acmphs_regs_t*)(k_ra_acmphs0_base_addr +
                                     ((uintptr_t)channel * (uintptr_t)k_ra_acmphs_channel_stride));
}

#ifdef __cplusplus
}
#endif
