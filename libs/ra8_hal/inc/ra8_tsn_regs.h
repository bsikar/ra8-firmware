/**
 * @file ra8_tsn_regs.h
 * @brief Temperature Sensor (TSN) register layout for the Renesas RA8D2
 * @ingroup grp_hal_analog
 *
 * @details
 * The RA8D2 temperature sensor has two halves:
 *
 *  1. A **control block** at `R_TSN_CTRL_BASE` (`0x40235000`) which
 *     holds the enable and calibration-mode bits. The sensor output
 *     is routed through ADC_B channel "temperature sensor" -- the
 *     actual raw code is read from ADC_B, not from here.
 *  2. A **calibration block** at `R_TSN_CAL_BASE` (`0x02C1EDA0`)
 *     which lives in the MRAM "trimming" area and holds factory-
 *     programmed reference values used to convert the raw ADC code
 *     into degrees Celsius.
 *
 * ## Calibration maths
 *
 * @f[
 *   T(degC) = \frac{(V_{raw} - V_{ref}) \times slope}{1000} + T_{ref}
 * @f]
 *
 * where `V_raw` is the raw ADC sample (14-bit right-aligned),
 * `V_ref` is `TSCDR` (reference code at `T_ref` degC), and
 * `slope` is `BSP_FEATURE_TSN_SLOPE` = 4000 uV/degC for the RA8D2.
 *
 * ## Register map
 *
 * | Block    | Offset | Reg    | Width | Purpose                 |
 * |----------|-------:|--------|------:|-------------------------|
 * | TSN_CTRL | 0x00   | TSCR   | 8     | Sensor control (TSEN)   |
 * | TSN_CAL  | 0x00   | TSCDR  | 32    | Reference code @ T_ref  |
 *
 * @note `TSCDR` is a single-cell memory-mapped value, not a struct.
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

typedef enum : uintptr_t {
  k_ra8_tsn_ctrl_base_addr = 0x40235000UL, /**< TSN control block.   */
  k_ra8_tsn_cal_base_addr  = 0x02C1EDA0UL, /**< Factory calibration. */
} ra8_tsn_addr_t;

/**
 * @enum ra8_tscr_bit_t
 * @brief Bit positions in TSCR (Temperature Sensor Control Register).
 *
 * @details HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register",
 *          p 3498.
 */
typedef enum : uint8_t {
  k_ra8_tscr_bit_tsen = 4U, /**< 1 = sensor enabled, 0 = off. */
  k_ra8_tscr_bit_tsoe = 7U, /**< 1 = output enabled to ADC_B. */
} ra8_tscr_bit_t;

/**
 * @enum ra8_tscr_mask_t
 * @brief Bit masks for TSCR (Temperature Sensor Control Register).
 *
 * @details
 * Pre-shifted masks corresponding to the bit positions in
 * ::ra8_tscr_bit_t. ``k_ra8_tscr_mask_all`` covers every documented
 * bit so callers (and unit tests) can ignore any reserved bits the
 * silicon happens to surface (HUM Ch 55.2.1 "TSCR" p 3498).
 */
typedef enum : uint8_t {
  k_ra8_tscr_mask_tsen = (uint8_t)(1U << 4U), /**< TSEN bit (sensor enable).        */
  k_ra8_tscr_mask_tsoe = (uint8_t)(1U << 7U), /**< TSOE bit (output enable to ADC). */
  k_ra8_tscr_mask_all  = (uint8_t)((1U << 4U) | (1U << 7U)), /**< TSEN | TSOE. */
} ra8_tscr_mask_t;

/**
 * @enum ra8_tscdr_field_t
 * @brief Field widths for TSCDR / TSCDR2 calibration words.
 *
 * @details
 * HUM Ch 55.2.2 "TSCDR : Temperature Sensor Calibration Data Register",
 * p 3498-3499. Only the lower 12 bits are meaningful; reserved bits
 * read 0. The same width applies to live conversion samples that the
 * ADC produces for the temperature-sensor channel.
 */
typedef enum : uint16_t {
  k_ra8_tscdr_data_mask = 0x0FFFU, /**< 12-bit data mask (HUM Ch 55.2.2 p 3498). */
} ra8_tscdr_field_t;

/**
 * @struct r_tsn_ctrl_regs_t
 * @brief TSN control block register window.
 *
 * @details HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register",
 *          p 3498.
 */
typedef struct {
  volatile uint8_t TSCR; /**< +0x00 Temperature Sensor Control Register. */
} r_tsn_ctrl_regs_t;

/**
 * @struct r_tsn_cal_regs_t
 * @brief TSN factory-calibration block (read-only on hardware).
 *
 * @details
 * HUM Ch 55.2.2 "TSCDR / TSCDR2 : Temperature Sensor Calibration
 * Data Registers", p 3498-3499. The two words are programmed at
 * the factory and capture the raw ADC code observed at the high
 * and low calibration temperatures (HUM Ch 55.3.1 p 3499-3500):
 *
 *  - ``TSCDR``  -- code at the high reference (105 or 125 degC).
 *  - ``TSCDR2`` -- code at the low reference (-40 degC).
 *
 * Both words are 32-bit memory cells in the MRAM trim area but only
 * the lower 12 bits (``k_ra8_tscdr_data_mask``) carry the calibration
 * code; the upper bits read 0.
 *
 * @note The cells live in the MRAM trim region (``0x02C1EDA0``).
 *       Hardware exposes them as read-only; tests inject values by
 *       writing to the fake mmap backing store.
 *
 * @see ra8_tsn_cal()
 */
typedef struct {
  volatile uint32_t TSCDR;  /**< +0x00 Calibration code at high ref temperature. */
  volatile uint32_t TSCDR2; /**< +0x04 Calibration code at low ref temperature.  */
} r_tsn_cal_regs_t;

/** @brief Get pointer to the TSN control block (HUM Ch 55.2.1 p 3498). */
RA8_HW_REGISTER_ACCESS
static inline volatile r_tsn_ctrl_regs_t* ra8_tsn(void)
{
  return (volatile r_tsn_ctrl_regs_t*)k_ra8_tsn_ctrl_base_addr;
}

/**
 * @brief Get pointer to the factory-calibration block (TSCDR + TSCDR2).
 *
 * @details HUM Ch 55.2.2 "TSCDR / TSCDR2", p 3498-3499. The block is
 *          read-only on hardware -- callers must treat the pointer's
 *          targets as ``const`` even though the struct fields are
 *          plain ``volatile uint32_t`` so unit tests can poke them.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile const r_tsn_cal_regs_t* ra8_tsn_cal(void)
{
  return (volatile const r_tsn_cal_regs_t*)k_ra8_tsn_cal_base_addr;
}

/** @brief Get pointer to the factory-programmed calibration word (TSCDR). */
RA8_HW_REGISTER_ACCESS
static inline volatile const uint32_t* ra8_tsn_tscdr(void)
{
  return (volatile const uint32_t*)k_ra8_tsn_cal_base_addr;
}

/**
 * @enum ra8_tsn_cal_t
 * @brief Calibration constants for the degC conversion.
 *
 * @details
 * From `BSP_FEATURE_TSN_SLOPE` (4000 uV/degC) and the reference
 * temperature embedded in the RA8D2 HUM section "Temperature Sensor".
 */
typedef enum : int32_t {
  k_ra8_tsn_slope_uv_per_c = 4000, /**< Slope in microvolts per degC. */
  k_ra8_tsn_ref_temp_degc  = 25,   /**< Reference temperature (degC). */
} ra8_tsn_cal_t;

/**
 * @enum ra8_tsn_adc_const_t
 * @brief ADC-side scaling constants used by the two-point conversion.
 *
 * @details
 * The two-point trim formula from HUM Ch 55.3.1 (p 3499-3500) needs
 * three magic-free numbers:
 *
 *  - ``k_ra8_tsn_adc_avcc_uv``    -- AVCC0 / VREFH0 in microvolts.
 *    The EK-RA8D2 board ties both rails to a 3.3 V LDO (HUM Ch 69
 *    "Electrical Characteristics" lists 3.3 V typical). Using
 *    microvolts keeps the divide in 64-bit integer math.
 *  - ``k_ra8_tsn_adc_full_scale`` -- 12-bit ADC full-scale (4096).
 *    Matches the ADC-side resolution that the temperature-sensor
 *    channel produces (HUM Ch 53 "16-bit A/D Converter" the TSN
 *    channel runs in 12-bit mode and Ch 55.2.2 p 3498 confirms the
 *    calibration words are 12-bit too).
 *  - ``k_ra8_tsn_uv_per_mv``      -- 1000 scale factor that promotes
 *    a degC value into milli-degC. Naming reflects the equivalent
 *    microvolt-per-millivolt ratio so the conversion math stays
 *    self-documenting.
 */
typedef enum : int32_t {
  k_ra8_tsn_adc_avcc_uv    = 3300000, /**< AVCC = 3.3 V expressed in uV. */
  k_ra8_tsn_adc_full_scale = 4096,    /**< 12-bit ADC full-scale.        */
  k_ra8_tsn_uv_per_mv      = 1000,    /**< 1000-x scale (degC -> mC).    */
} ra8_tsn_adc_const_t;

#ifdef __cplusplus
}
#endif
