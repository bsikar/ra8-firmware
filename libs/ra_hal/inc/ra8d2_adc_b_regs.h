/**
 * @file ra8d2_adc_b_regs.h
 * @brief ADC_B (12 / 14-bit SAR ADC) register layout for the RA8D2
 *
 * @details
 * The RA8D2 ADC_B is the post-RA8 "Type-B" successor to the legacy
 * ADC12/ADC16H block. Instead of a single ADCSR + ADCER + ADANSA,
 * FSP R_ADC_B0_Type (R7KA8D2KF_core0.h lines 20460-24938; total
 * size 0x2224 = 8740 bytes) spreads config across:
 *
 *  | Offset | Name       | Width | Purpose                            |
 *  |-------:|------------|------:|------------------------------------|
 *  | 0x0000 | ADCLKENR   | 32    | Clock Enable                       |
 *  | 0x0008 | ADCLKCR    | 32    | Clock Control (CLKSEL, DIVR)       |
 *  | 0x000C | ADSYCR     | 32    | Synchronous-op Control             |
 *  | 0x0020 | ADERINTCR  | 32    | Error Interrupt Enable             |
 *  | 0x0040 | ADMDR      | 32    | Converter Mode Selection           |
 *  | 0x005C | ADINTCR    | 32    | Scan-End Interrupt Enable          |
 *  | 0x0600 | ADCHCR[24] | 32x24 | Per-channel conversion config      |
 *  | 0x2000 | ADDR[23]   | 32x23 | Conversion results                 |
 *
 * The full FSP struct is ~200 registers; this header only exposes
 * the subset the driver actually uses. Extensions land with
 * whatever consumer needs them.
 *
 * The previous version of this header modeled the legacy ADC16H
 * layout (ADCSR/ADANSA0/ADCER/ADDR0..47/ADSSTR00..47) which does
 * not exist at all on RA8D2 ADC_B.
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
  k_ra_adc_b_base_addr = 0x40338000UL, /**< FSP R_ADC_B0_BASE. */
} ra_adc_b_addr_t;

typedef enum : uint8_t {
  k_ra_adc_b_unit_count    = 2U,  /**< Two independent ADC units. */
  k_ra_adc_b_max_channels  = 24U, /**< ADCHCR0..23 channel config slots. */
  k_ra_adc_b_result_regs   = 23U, /**< ADDR[0..22] result slots.         */
  k_ra_adc_b_adchcr_stride = 16U, /**< ADCHCRn occupies 16 bytes each.    */
  k_ra_adc_b_addr_stride   = 4U,  /**< ADDR[n] is uint32_t per slot.      */
} ra_adc_b_limits_t;

/**
 * @enum ra_adc_b_off_t
 * @brief Register offsets inside the ADC_B block (subset in use).
 */
typedef enum : uint16_t {
  k_ra_adc_b_off_adclkenr  = 0x0000U, /**< Clock Enable Register.         */
  k_ra_adc_b_off_adclkcr   = 0x0008U, /**< Clock Control Register.         */
  k_ra_adc_b_off_adsycr    = 0x000CU, /**< Synchronous Operation Control.  */
  k_ra_adc_b_off_aderintcr = 0x0020U, /**< Error Interrupt Enable.         */
  k_ra_adc_b_off_admdr     = 0x0040U, /**< Mode Selection.                 */
  k_ra_adc_b_off_adintcr   = 0x005CU, /**< Scan End Interrupt Enable.      */
  k_ra_adc_b_off_adchcr0   = 0x0600U, /**< ADCHCR[0] -- per-channel config.*/
  k_ra_adc_b_off_addr0     = 0x2000U, /**< ADDR[0] -- conversion results.  */
} ra_adc_b_off_t;

/**
 * @enum ra_adclkenr_bit_t
 * @brief ADCLKENR bit positions (FSP R_ADC_B0_ADCLKENR_b).
 */
typedef enum : uint8_t {
  k_ra_adclkenr_bit_clken = 0U, /**< Clock enable. */
} ra_adclkenr_bit_t;

/**
 * @enum ra_adclkenr_mask_t
 */
typedef enum : uint32_t {
  k_ra_adclkenr_mask_clken = 0x00000001UL,
} ra_adclkenr_mask_t;

/**
 * @enum ra_admdr_bit_t
 * @brief ADMDR bit positions (partial).
 */
typedef enum : uint8_t {
  k_ra_admdr_bit_adtrgmd  = 0U,  /**< Trigger mode select.             */
  k_ra_admdr_bit_adscanmd = 1U,  /**< Scan mode select.               */
  k_ra_admdr_bit_adbusy   = 15U, /**< Busy flag (RO).                  */
} ra_admdr_bit_t;

typedef enum : uint32_t {
  k_ra_admdr_mask_adtrgmd  = 0x00000001UL,
  k_ra_admdr_mask_adscanmd = 0x00000002UL,
  k_ra_admdr_mask_adbusy   = 0x00008000UL,
} ra_admdr_mask_t;

/**
 * @enum ra_adchcr_bit_t
 * @brief ADCHCRn bit positions (partial).
 *
 * @details
 * Each per-channel ADCHCR has a "start conversion" bit in the low
 * position and a resolution select field. The exact layout per
 * FSP R_ADC_B0_ADCHCRn varies slightly by revision; the driver
 * only touches CVEN (bit 0) to kick a software conversion and
 * RESSEL[5:4] for resolution.
 */
typedef enum : uint8_t {
  k_ra_adchcr_bit_cven    = 0U, /**< Channel conversion enable. */
  k_ra_adchcr_bit_ressel0 = 4U, /**< Resolution select bit 0.   */
  k_ra_adchcr_bit_ressel1 = 5U, /**< Resolution select bit 1.   */
} ra_adchcr_bit_t;

typedef enum : uint32_t {
  k_ra_adchcr_mask_cven   = 0x00000001UL,
  k_ra_adchcr_mask_ressel = 0x00000030UL,
} ra_adchcr_mask_t;

/**
 * @brief Pointer to the 32-bit ADCLKENR register.
 */
static inline volatile uint32_t* ra_adc_b_adclkenr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adclkenr);
}

/**
 * @brief Pointer to the 32-bit ADCLKCR register.
 */
static inline volatile uint32_t* ra_adc_b_adclkcr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adclkcr);
}

/**
 * @brief Pointer to the 32-bit ADMDR register.
 */
static inline volatile uint32_t* ra_adc_b_admdr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_admdr);
}

/**
 * @brief Pointer to the 32-bit ADINTCR register.
 */
static inline volatile uint32_t* ra_adc_b_adintcr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adintcr);
}

/**
 * @brief Pointer to the per-channel ADCHCR register.
 * @param[in] ch Channel index (0..23).
 * @return Volatile pointer, or ``nullptr`` if ``ch`` is out of range.
 */
static inline volatile uint32_t* ra_adc_b_adchcr(uint8_t ch)
{
  if (ch >= k_ra_adc_b_max_channels) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adchcr0 +
                              ((uintptr_t)ch * (uintptr_t)k_ra_adc_b_adchcr_stride));
}

/**
 * @brief Pointer to the per-channel ADDR register.
 * @param[in] ch Channel index (0..22).
 * @return Volatile pointer to the 32-bit result slot, or ``nullptr``
 *         if ``ch`` is out of range.
 */
static inline volatile uint32_t* ra_adc_b_addr(uint8_t ch)
{
  if (ch >= k_ra_adc_b_result_regs) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_addr0 +
                              ((uintptr_t)ch * (uintptr_t)k_ra_adc_b_addr_stride));
}

#ifdef __cplusplus
}
#endif
