/**
 * @file ra8_acmphs_regs.h
 * @brief High-Speed Analog Comparator (ACMPHS) register layout for the RA8D2
 * @ingroup grp_hal_analog
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
 * |  0x40  | CPINTCTL | 8     | Interrupt control                    |
 * |  0x44  | CPMSKCTL | 8     | Interrupt mask control               |
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

#include "ra8_attributes.h"

/**
 * @enum ra8_acmphs_addr_t
 * @brief Memory-mapped base addresses for ACMPHS.
 */
typedef enum : uintptr_t {
  k_ra8_acmphs0_base_addr = 0x40236000UL, /**< ACMPHS channel 0 base (FSP R_ACMPHS0_BASE). */
} ra8_acmphs_addr_t;

/**
 * @enum ra8_acmphs_limits_t
 * @brief Channel count and stride.
 */
typedef enum : uint16_t {
  k_ra8_acmphs_channel_count  = 6U,     /**< Channels 0..5 on this package. */
  k_ra8_acmphs_channel_stride = 0x100U, /**< Bytes between channels.        */
  k_ra8_acmphs_reserved_mid   = 47U,    /**< Reserved gap CPIOC->CPINTCTL.  */
} ra8_acmphs_limits_t;

/**
 * @enum ra8_cmpctl_bit_t
 * @brief CMPCTL control-bit positions (verified against FSP
 *        R_ACMPHS0_CMPCTL_b, R7KA8D2KF_core0.h lines 3874-3884).
 */
typedef enum : uint8_t {
  k_ra8_acmphs_bit_cinv   = 0U, /**< Output polarity inversion.   */
  k_ra8_acmphs_bit_coe    = 1U, /**< Output enable (to pin).      */
  k_ra8_acmphs_bit_csten  = 2U, /**< Interrupt select.            */
  k_ra8_acmphs_bit_ceg0   = 3U, /**< CEG[1:0] edge selector lo.   */
  k_ra8_acmphs_bit_ceg1   = 4U, /**< CEG[1:0] edge selector hi.   */
  k_ra8_acmphs_bit_cdfs0  = 5U, /**< CDFS[1:0] noise filter lo.   */
  k_ra8_acmphs_bit_cdfs1  = 6U, /**< CDFS[1:0] noise filter hi.   */
  k_ra8_acmphs_bit_hcmpon = 7U, /**< Comparator operation enable. */
} ra8_cmpctl_bit_t;

/**
 * @enum ra8_acmphs_mask_t
 * @brief CMPCTL bit masks.
 */
typedef enum : uint8_t {
  k_ra8_acmphs_mask_cinv  = 0x01U, /**< CINV bit.                         */
  k_ra8_acmphs_mask_coe   = 0x02U, /**< COE bit.                          */
  k_ra8_acmphs_mask_csten = 0x04U, /**< CSTEN interrupt select.           */
  k_ra8_acmphs_mask_ceg   = 0x18U, /**< CEG[1:0] @ [4:3] edge selector.   */
  k_ra8_acmphs_mask_cdfs  = 0x60U, /**< CDFS[1:0] @ [6:5] noise filter.   */
  k_ra8_acmphs_mask_hcen  = 0x80U, /**< HCMPON operation enable.          */
  k_ra8_acmphs_mask_hcmon = 0x01U, /**< CMPMON.CMPMON: output level read. */
} ra8_acmphs_mask_t;

/**
 * @struct r_acmphs_regs_t
 * @brief Per-channel ACMPHS register window.
 *
 * @details
 * Verified against FSP R_ACMPHS0_Type in R7KA8D2KF_core0.h lines
 * 3870-3959 (size = 0x45). The header previously modeled CMPFIR at
 * offset 0x14; that register does not exist on RA8D2 -- it was a
 * legacy pre-RA8 field. The authoritative sequence is CMPCTL ->
 * CMPSEL0 -> CMPSEL1 -> CMPMON -> CPIOC with a 47-byte gap up to
 * CPINTCTL + CPMSKCTL at 0x40 / 0x44.
 */
typedef struct {
  volatile uint8_t CMPCTL;                         /**< +0x00 Control (enable, edges, filter). */
  volatile uint8_t _r0[3];                         /**< Padding.                               */
  volatile uint8_t CMPSEL0;                        /**< +0x04 Plus-input selection.            */
  volatile uint8_t _r1[3];                         /**< Padding.                               */
  volatile uint8_t CMPSEL1;                        /**< +0x08 Minus-input selection.           */
  volatile uint8_t _r2[3];                         /**< Padding.                               */
  volatile uint8_t CMPMON;                         /**< +0x0C Output monitor (read-only).      */
  volatile uint8_t _r3[3];                         /**< Padding.                               */
  volatile uint8_t CPIOC;                          /**< +0x10 Output polarity / pin control.   */
  volatile uint8_t _r4[k_ra8_acmphs_reserved_mid]; /**< Reserved.                              */
  volatile uint8_t CPINTCTL;                       /**< +0x40 Interrupt control.               */
  volatile uint8_t _r5[3];                         /**< Padding.                               */
  volatile uint8_t CPMSKCTL;                       /**< +0x44 Interrupt mask control.          */
} r_acmphs_regs_t;

/**
 * @brief Get pointer to ACMPHS channel `channel`.
 * @param[in] channel Channel index (0..5).
 * @return Volatile pointer to the channel register block, or `nullptr`
 *         if `channel` is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_acmphs_regs_t* ra8_acmphs(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return nullptr;
  }
  return (volatile r_acmphs_regs_t*)(k_ra8_acmphs0_base_addr +
                                     ((uintptr_t)channel * (uintptr_t)k_ra8_acmphs_channel_stride));
}

#ifdef __cplusplus
}
#endif
