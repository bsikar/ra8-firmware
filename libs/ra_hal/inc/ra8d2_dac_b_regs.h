/**
 * @file ra8d2_dac_b_regs.h
 * @brief 12-bit DAC_B register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 has two separate DAC_B instances DAC_B0 / DAC_B1 at
 * base addresses 0x40233000 and 0x40233100 (stride 0x100). Each
 * instance drives ONE 12-bit channel. This is a different IP block
 * from the pre-RA8 "DAC12" (which had a single block with two
 * DADR0/DADR1 registers and a shared DACR control byte).
 *
 * Per-instance register map, verified against FSP R_DAC_B0_Type
 * (R7KA8D2KF_core0.h lines 5396+):
 *
 *   | Offset | Name   | Width | Purpose                              |
 *   |-------:|--------|------:|--------------------------------------|
 *   |  0x00  | DADR   |  16   | Converted data (12-bit value)        |
 *   |  0x04  | DACR0  |  32   | Control 0 (DACEN, DAE, DAOUTDIS)     |
 *   |  0x08  | DACR1  |  32   | Control 1 (DPSEL data placement)     |
 *   |  0x0C  | DACR2  |  32   | Control 2 (reference / adc sync)     |
 *
 * Register offsets tracked against HUM Ch 54 "12-Bit D/A
 * Converter (DAC12)" p 3490.
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
 * @brief Memory-mapped base addresses for DAC_B0 / DAC_B1.
 */
typedef enum : uintptr_t {
  k_ra_dac_b0_base_addr = 0x40233000UL, /**< FSP R_DAC_B0_BASE.         */
  k_ra_dac_b1_base_addr = 0x40233100UL, /**< FSP R_DAC_B1_BASE (stride 0x100). */
} ra_dac_b_addr_t;

/**
 * @enum ra_dac_b_limits_t
 * @brief Channel count, stride, and value range.
 */
typedef enum : uint16_t {
  k_ra_dac_b_channel_count = 2U,     /**< Two independent DAC_B instances. */
  k_ra_dac_b_stride        = 0x100U, /**< Bytes between DAC_B0 and DAC_B1. */
  k_ra_dac_b_max_value     = 4095U,  /**< 12-bit full-scale count.         */
} ra_dac_b_limits_t;

/**
 * @enum ra_dacr0_bit_t
 * @brief DACR0 bit positions (per-channel control register 0).
 */
typedef enum : uint8_t {
  k_ra_dacr0_bit_dacen    = 0U,  /**< DACEN: channel enable.        */
  k_ra_dacr0_bit_dae      = 15U, /**< DAE: batch conversion control. */
  k_ra_dacr0_bit_daoutdis = 31U, /**< DAOUTDIS: output disable.      */
} ra_dacr0_bit_t;

/**
 * @enum ra_dacr0_mask_t
 * @brief DACR0 bit masks.
 */
typedef enum : uint32_t {
  k_ra_dacr0_mask_dacen    = 0x00000001UL,
  k_ra_dacr0_mask_dae      = 0x00008000UL,
  k_ra_dacr0_mask_daoutdis = 0x80000000UL,
} ra_dacr0_mask_t;

/**
 * @struct r_dac_b_regs_t
 * @brief Per-instance DAC_B register window.
 */
typedef struct {
  volatile uint16_t DADR; /**< +0x00 12-bit data value (low 12 bits). */
  volatile uint16_t _r0;
  volatile uint32_t DACR0; /**< +0x04 Control 0.                     */
  volatile uint32_t DACR1; /**< +0x08 Control 1 (data placement).    */
  volatile uint32_t DACR2; /**< +0x0C Control 2 (ref / ADC sync).    */
} r_dac_b_regs_t;

/**
 * @brief Get pointer to DAC_B instance N (0 or 1).
 *
 * @param[in] instance 0 = DAC_B0, 1 = DAC_B1.
 * @return Volatile pointer to the DAC_B register window, or
 *         ``nullptr`` if instance is out of range.
 */
static inline volatile r_dac_b_regs_t* ra_dac_b(uint8_t instance)
{
  if ((uint16_t)instance >= k_ra_dac_b_channel_count) {
    return nullptr;
  }
  return (volatile r_dac_b_regs_t*)(k_ra_dac_b0_base_addr +
                                    ((uintptr_t)instance * (uintptr_t)k_ra_dac_b_stride));
}

#ifdef __cplusplus
}
#endif
