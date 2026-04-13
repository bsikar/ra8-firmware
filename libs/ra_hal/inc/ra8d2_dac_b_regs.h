/**
 * @file ra8d2_dac_b_regs.h
 * @brief 12-bit DAC_B register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 ships with a pair of 12-bit DAC channels in the "DAC_B"
 * variant (successor to the older DAC12 block). Channels 0 and 1
 * share a common control register. The block lives at
 * `0x40334000`.
 *
 * Register map:
 *
 * | Offset | Name   | Width | Purpose                               |
 * |-------:|--------|------:|---------------------------------------|
 * |  0x00  | DADR0  | 16    | Channel 0 data register (12-bit val)  |
 * |  0x02  | DADR1  | 16    | Channel 1 data register (12-bit val)  |
 * |  0x04  | DACR   | 8     | Control (DAOEnn, DAE, ...)            |
 * |  0x05  | DADPR  | 8     | Data padding register (L/R align)     |
 * |  0x06  | DAADSCR| 8     | ADC synchronous start control         |
 * |  0x07  | DAVREFCR| 8    | Reference voltage control             |
 *
 * TODO: verify against HUM section 37 (DAC_B) -- offsets confirmed
 * against FSP `r_dac.h` for RA8 family.
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
 * @enum ra_dac_b_addr_t
 * @brief Memory-mapped base address for DAC_B.
 */
typedef enum : uintptr_t {
  k_ra_dac_b_base_addr = 0x40334000UL, /**< DAC_B peripheral base. */
} ra_dac_b_addr_t;

/**
 * @enum ra_dac_b_limits_t
 * @brief Channel count and value range.
 */
typedef enum : uint16_t {
  k_ra_dac_b_channel_count = 2U,    /**< Channels 0 and 1.        */
  k_ra_dac_b_max_value     = 4095U, /**< 12-bit full-scale count. */
} ra_dac_b_limits_t;

/**
 * @enum ra_dacr_bit_t
 * @brief DACR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_dac_b_bit_dae   = 5U, /**< DAC enable (global).         */
  k_ra_dac_b_bit_daoe0 = 6U, /**< Channel 0 output enable.     */
  k_ra_dac_b_bit_daoe1 = 7U, /**< Channel 1 output enable.     */
} ra_dacr_bit_t;

/**
 * @enum ra_dacr_mask_t
 * @brief DACR bit masks.
 */
typedef enum : uint8_t {
  k_ra_dac_b_mask_dae   = 0x20U, /**< DAE bit.   */
  k_ra_dac_b_mask_daoe0 = 0x40U, /**< DAOE0 bit. */
  k_ra_dac_b_mask_daoe1 = 0x80U, /**< DAOE1 bit. */
} ra_dacr_mask_t;

/**
 * @struct r_dac_b_regs_t
 * @brief DAC_B register window.
 */
typedef struct {
  volatile uint16_t DADR0;    /**< +0x00 Channel 0 data (12-bit).     */
  volatile uint16_t DADR1;    /**< +0x02 Channel 1 data (12-bit).     */
  volatile uint8_t  DACR;     /**< +0x04 Control register.            */
  volatile uint8_t  DADPR;    /**< +0x05 Data padding register.       */
  volatile uint8_t  DAADSCR;  /**< +0x06 ADC sync start control.      */
  volatile uint8_t  DAVREFCR; /**< +0x07 Reference voltage control.   */
} r_dac_b_regs_t;

/**
 * @brief Get pointer to the DAC_B register block.
 * @return Volatile pointer to the DAC_B window.
 */
static inline volatile r_dac_b_regs_t* ra_dac_b(void)
{
  return (volatile r_dac_b_regs_t*)k_ra_dac_b_base_addr;
}

#ifdef __cplusplus
}
#endif
