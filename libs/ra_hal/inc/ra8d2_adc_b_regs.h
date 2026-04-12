/**
 * @file ra8d2_adc_b_regs.h
 * @brief ADC_B (12 / 14-bit SAR ADC) register layout for the RA8D2
 *
 * @details
 * The RA8D2 implements the "ADC_B" peripheral variant (not the older
 * "ADC" or newer "ADC_D"). Base address is `0x40338000`. Two ADC
 * units live on the same block, distinguished by channel masks in
 * `ADCERx`.
 *
 * This header models the subset needed for software-triggered
 * single-channel conversions. DMA hooks, group scans, sample-and-
 * hold timing, comparator mode, and self-diagnostic registers land
 * with the first driver that needs them.
 *
 * ## Register map (partial)
 *
 * | Offset | Name     | Width | Purpose                                 |
 * |-------:|----------|------:|-----------------------------------------|
 * | 0x000  | ADCSR    | 16    | ADC Control / Status                    |
 * | 0x004  | ADANSA0  | 16    | Analog Channel Select A (unit 0)        |
 * | 0x010  | ADSSTR00 | 8     | Sampling Start Time, channel 0          |
 * | ...    | ADSSTRxx | 8     | One per channel (0..47)                 |
 * | 0x020  | ADDR0    | 16    | Conversion result, channel 0            |
 * | ...    | ADDRxx   | 16    | One per channel                         |
 * | 0x05C  | ADCER    | 16    | Extended Control (resolution, alignment)|
 * | 0x060  | ADCMPCR  | 16    | Comparator Control                      |
 *
 * Full field layouts come from the RA8D2 HUM section "12/14-bit
 * A/D Converter (ADC_B)".
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
  k_ra_adc_b_base_addr = 0x40338000UL,
} ra_adc_b_addr_t;

typedef enum : uint8_t {
  k_ra_adc_b_unit_count   = 2U,  /**< Two independent ADC units. */
  k_ra_adc_b_max_channels = 48U, /**< 0..47 channels (varies per package). */
} ra_adc_b_limits_t;

/**
 * @enum ra_adc_b_off_t
 * @brief Register offsets inside the ADC_B block.
 */
typedef enum : uint16_t {
  k_ra_adc_b_off_adcsr    = 0x000U, /**< Control / Status.              */
  k_ra_adc_b_off_adansa0  = 0x004U, /**< Channel select A low half.     */
  k_ra_adc_b_off_adansa1  = 0x006U, /**< Channel select A high half.    */
  k_ra_adc_b_off_adsstr00 = 0x010U, /**< Sampling time, ch 0.           */
  k_ra_adc_b_off_addr0    = 0x020U, /**< Result reg, ch 0.              */
  k_ra_adc_b_off_adcer    = 0x05CU, /**< Extended control.              */
  k_ra_adc_b_off_adcmpcr  = 0x060U, /**< Comparator control.            */
} ra_adc_b_off_t;

/**
 * @enum ra_adcsr_bit_t
 * @brief ADCSR bit positions.
 */
typedef enum : uint8_t {
  k_ra_adcsr_bit_trge = 9U,  /**< 1 = hardware trigger enable.    */
  k_ra_adcsr_bit_exce = 10U, /**< External trigger select.         */
  k_ra_adcsr_bit_adie = 12U, /**< Conversion-end IRQ enable.       */
  k_ra_adcsr_bit_adcs = 13U, /**< Scan mode (0=single, 1=group).   */
  k_ra_adcsr_bit_adst = 15U, /**< 1 = start conversion / busy.     */
} ra_adcsr_bit_t;

/**
 * @enum ra_adcer_bit_t
 * @brief ADCER bit positions for resolution / alignment.
 */
typedef enum : uint8_t {
  k_ra_adcer_bit_adprc0 = 1U,  /**< Resolution bit 0.                */
  k_ra_adcer_bit_adprc1 = 2U,  /**< Resolution bit 1.                */
  k_ra_adcer_bit_acbit  = 5U,  /**< 1 = self-diagnostic bit.         */
  k_ra_adcer_bit_adrfmt = 15U, /**< 1 = right-aligned 16-bit result.*/
} ra_adcer_bit_t;

/**
 * @enum ra_adcer_resolution_t
 * @brief ADPRC[1:0] resolution codes.
 */
typedef enum : uint8_t {
  k_ra_adcer_res_12bit = 0U,
  k_ra_adcer_res_10bit = 1U,
  k_ra_adcer_res_14bit = 2U,
} ra_adcer_resolution_t;

/**
 * @brief Pointer to ADCSR (16-bit).
 */
static inline volatile uint16_t* ra_adc_b_adcsr(void)
{
  return (volatile uint16_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcsr);
}

/**
 * @brief Pointer to the first analog-select half.
 */
static inline volatile uint16_t* ra_adc_b_adansa0(void)
{
  return (volatile uint16_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adansa0);
}

/**
 * @brief Pointer to ADCER (16-bit).
 */
static inline volatile uint16_t* ra_adc_b_adcer(void)
{
  return (volatile uint16_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcer);
}

/**
 * @brief Pointer to `ADDRxx` -- conversion result for channel `ch`.
 *
 * @param[in] ch Channel index (0..47).
 * @return Volatile pointer to the 16-bit result register, or `nullptr`
 *         if `ch` is out of range.
 */
static inline volatile uint16_t* ra_adc_b_addr(uint8_t ch)
{
  if (ch >= (uint8_t)k_ra_adc_b_max_channels) {
    return nullptr;
  }
  return (volatile uint16_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_addr0 +
                              ((uintptr_t)ch * sizeof(uint16_t)));
}

/**
 * @brief Pointer to `ADSSTRxx` -- sampling time for channel `ch`.
 */
static inline volatile uint8_t* ra_adc_b_adsstr(uint8_t ch)
{
  if (ch >= (uint8_t)k_ra_adc_b_max_channels) {
    return nullptr;
  }
  return (volatile uint8_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adsstr00 + (uintptr_t)ch);
}

#ifdef __cplusplus
}
#endif
