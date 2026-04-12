/**
 * @file ra8d2_tsn_regs.h
 * @brief Temperature Sensor (TSN) register layout for the Renesas RA8D2
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

typedef enum : uintptr_t {
  k_ra_tsn_ctrl_base_addr = 0x40235000UL, /**< TSN control block. */
  k_ra_tsn_cal_base_addr  = 0x02C1EDA0UL, /**< Factory calibration. */
} ra_tsn_addr_t;

/**
 * @enum ra_tscr_bit_t
 * @brief Bit positions in TSCR (Temperature Sensor Control Register).
 */
typedef enum : uint8_t {
  k_ra_tscr_bit_tsen = 4U, /**< 1 = sensor enabled, 0 = off.        */
  k_ra_tscr_bit_tsoe = 7U, /**< 1 = output enabled to ADC_B.        */
} ra_tscr_bit_t;

/**
 * @struct r_tsn_ctrl_regs_t
 * @brief TSN control block register window.
 */
typedef struct {
  volatile uint8_t TSCR; /**< +0x00 Temperature Sensor Control Register. */
} r_tsn_ctrl_regs_t;

/** @brief Get pointer to the TSN control block. */
static inline volatile r_tsn_ctrl_regs_t* ra_tsn(void)
{
  return (volatile r_tsn_ctrl_regs_t*)k_ra_tsn_ctrl_base_addr;
}

/** @brief Get pointer to the factory-programmed calibration word (TSCDR). */
static inline volatile const uint32_t* ra_tsn_tscdr(void)
{
  return (volatile const uint32_t*)k_ra_tsn_cal_base_addr;
}

/**
 * @enum ra_tsn_cal_t
 * @brief Calibration constants for the degC conversion.
 *
 * @details
 * From `BSP_FEATURE_TSN_SLOPE` (4000 uV/degC) and the reference
 * temperature embedded in the RA8D2 HUM section "Temperature Sensor".
 */
typedef enum : int32_t {
  k_ra_tsn_slope_uv_per_c = 4000, /**< Slope in microvolts per degC.     */
  k_ra_tsn_ref_temp_degc  = 25,   /**< Reference temperature (degC).     */
} ra_tsn_cal_t;

#ifdef __cplusplus
}
#endif
