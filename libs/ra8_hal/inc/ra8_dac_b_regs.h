/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dac_b_regs.h
 * @brief 12-bit DAC_B register layout for the Renesas RA8D2
 * @ingroup grp_hal_analog
 *
 * @details
 * The RA8D2 has two separate DAC_B instances DAC_B0 / DAC_B1 at
 * base addresses 0x40233000 and 0x40233100 (stride 0x100). Each
 * instance drives ONE 12-bit channel. This is a different IP block
 * from the pre-RA8 "DAC12" (which had a single block with two
 * DADR0/DADR1 registers and a shared DACR control byte).
 *
 * Per-instance register map, verified against FSP R_DAC_B0_Type
 * (R7KA8D2KF_core0.h around line 5396):
 *
 *   | Offset | Name   | Width | Bit fields                         |
 *   |-------:|--------|------:|------------------------------------|
 *   |  0x00  | DADR   |  16   | DADR[15:0] 12-bit data             |
 *   |  0x02  | (rsvd) |  16   | RESERVED (must read as 0)          |
 *   |  0x04  | DACR0  |  32   | DACEN[0], DAE[15], DAOUTDIS[31]    |
 *   |  0x08  | DACR1  |  32   | DPSEL[16] (data placement)         |
 *   |  0x0C  | DACR2  |  32   | OFSSEL[8] (DAC-HM operating mode)  |
 *
 * Register offsets and bit positions tracked against HUM Ch 54
 * "12-Bit D/A Converter (DAC12)" p 3490..3496 and against FSP
 * `R_DAC_B0_DACR{0,1,2}_*_Msk` constants.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_dac_b_addr_t
 * @brief Memory-mapped base addresses for DAC_B0 / DAC_B1.
 */
typedef enum : uintptr_t {
  k_ra8_dac_b0_base_addr = 0x40233000UL, /**< FSP R_DAC_B0_BASE.                */
  k_ra8_dac_b1_base_addr = 0x40233100UL, /**< FSP R_DAC_B1_BASE (stride 0x100). */
} ra8_dac_b_addr_t;

/**
 * @enum ra8_dac_b_limits_t
 * @brief Channel count, stride, and value range.
 */
typedef enum : uint16_t {
  k_ra8_dac_b_channel_count = 2U,     /**< Two independent DAC_B instances. */
  k_ra8_dac_b_stride        = 0x100U, /**< Bytes between DAC_B0 and DAC_B1. */
  k_ra8_dac_b_max_value     = 4095U,  /**< 12-bit full-scale count.         */
} ra8_dac_b_limits_t;

/**
 * @enum ra8_dacr0_bit_t
 * @brief DACR0 bit positions (per-channel control register 0).
 *
 * @details
 * Verified against FSP `R_DAC_B0_DACR0_*_Pos` -- see HUM Ch 54
 * "12-Bit D/A Converter (DAC12)" p 3490..3496.
 */
typedef enum : uint8_t {
  k_ra8_dacr0_bit_dacen    = 0U,  /**< DACEN: channel enable.         */
  k_ra8_dacr0_bit_dae      = 15U, /**< DAE: batch conversion control. */
  k_ra8_dacr0_bit_daoutdis = 31U, /**< DAOUTDIS: output disable.      */
} ra8_dacr0_bit_t;

/**
 * @enum ra8_dacr0_mask_t
 * @brief DACR0 bit masks.
 *
 * @details
 * Mirror FSP `R_DAC_B0_DACR0_*_Msk`.
 */
typedef enum : uint32_t {
  k_ra8_dacr0_mask_dacen    = 0x00000001UL, /**< Mirror of FSP DACEN_Msk.    */
  k_ra8_dacr0_mask_dae      = 0x00008000UL, /**< Mirror of FSP DAE_Msk.      */
  k_ra8_dacr0_mask_daoutdis = 0x80000000UL, /**< Mirror of FSP DAOUTDIS_Msk. */
} ra8_dacr0_mask_t;

/**
 * @enum ra8_dacr1_bit_t
 * @brief DACR1 bit positions (data placement).
 *
 * @details
 * DPSEL selects right- (0) vs left-justified (1) 12-bit data in the
 * 16-bit DADR field. Verified against FSP `R_DAC_B0_DACR1_DPSEL_Pos`.
 */
typedef enum : uint8_t {
  k_ra8_dacr1_bit_dpsel = 16U, /**< DPSEL: data placement select. */
} ra8_dacr1_bit_t;

/**
 * @enum ra8_dacr1_mask_t
 * @brief DACR1 bit masks.
 */
typedef enum : uint32_t {
  k_ra8_dacr1_mask_dpsel = 0x00010000UL, /**< Mirror of FSP DPSEL_Msk. */
} ra8_dacr1_mask_t;

/**
 * @enum ra8_dacr2_bit_t
 * @brief DACR2 bit positions (DAC-HM operating-voltage mode).
 *
 * @details
 * OFSSEL selects the high-voltage reference range. FSP exposes only
 * two values: NORMAL (>= 2.7V, OFSSEL = 0) and LOW (< 2.7V, OFSSEL = 1).
 * Verified against FSP `R_DAC_B0_DACR2_OFSSEL_Pos`.
 */
typedef enum : uint8_t {
  k_ra8_dacr2_bit_ofssel = 8U, /**< OFSSEL: VREFH range select. */
} ra8_dacr2_bit_t;

/**
 * @enum ra8_dacr2_mask_t
 * @brief DACR2 bit masks.
 */
typedef enum : uint32_t {
  k_ra8_dacr2_mask_ofssel = 0x00000100UL, /**< Mirror of FSP OFSSEL_Msk. */
} ra8_dacr2_mask_t;

/**
 * @struct r_dac_b_regs_t
 * @brief Per-instance DAC_B register window.
 *
 * @details
 * Layout matches FSP `R_DAC_B0_Type` (size 0x10, including the
 * 16-bit reserved gap between DADR and DACR0).
 */
typedef struct {
  volatile uint16_t DADR;          /**< +0x00 12-bit data value (low 12 bits). */
  volatile uint16_t reserved_dadr; /**< +0x02 RESERVED (FSP field).            */
  volatile uint32_t DACR0;         /**< +0x04 Control 0 (DACEN/DAE/DAOUTDIS).  */
  volatile uint32_t DACR1;         /**< +0x08 Control 1 (DPSEL bit 16).        */
  volatile uint32_t DACR2;         /**< +0x0C Control 2 (OFSSEL bit 8).        */
} r_dac_b_regs_t;

/**
 * @brief Get pointer to DAC_B instance N (0 or 1).
 *
 * @param[in] instance 0 = DAC_B0, 1 = DAC_B1.
 * @return Volatile pointer to the DAC_B register window, or
 *         ``nullptr`` if instance is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_dac_b_regs_t* ra8_dac_b(uint8_t instance)
{
  if ((uint16_t)instance >= k_ra8_dac_b_channel_count) {
    return nullptr;
  }
  return (volatile r_dac_b_regs_t*)(k_ra8_dac_b0_base_addr +
                                    ((uintptr_t)instance * (uintptr_t)k_ra8_dac_b_stride));
}

#ifdef __cplusplus
}
#endif
