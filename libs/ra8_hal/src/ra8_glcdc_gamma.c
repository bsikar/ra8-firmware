/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_glcdc_gamma.c
 * @brief GLCDC piecewise-linear gamma correction driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements per-channel gamma correction for the RA8D2 GLCDC output
 * pipeline.  The hardware provides three independent 16-segment
 * piecewise-linear correction blocks (GAM[0]=R, GAM[1]=G, GAM[2]=B).
 * Each block exposes eight 32-bit LUT registers (two 11-bit gain
 * coefficients each) and eight 32-bit AREA registers (two 10-bit
 * boundary thresholds each), together defining a 16-point correction
 * curve per channel.  The global enable/disable switch lives in the
 * shared OUT block at k_ra8_glcdc_off_out_gamsw.
 *
 * Every register access is preceded by a HUM Ch 63 citation.
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_glcdc_regs.h"
#include "ra8_log.h"

static const char* s_tag = "GLCDC";

/* =============================================================================
 * Module-private constants
 * =============================================================================
 */

/**
 * @enum ra8_glcdc_gam_reg_count_t
 * @brief Number of LUT and AREA registers per gamma channel block.
 *
 * @details
 * HUM Ch 63 "GAMx_LUTn Gamma Correction LUT Register" p 3790 lists
 * LUT[0..7] (eight registers, two gains per register = 16 gain values).
 * HUM Ch 63 "GAMx_AREAn Gamma Correction Area Register" p 3793 lists
 * AREA[0..7] (eight registers, two thresholds per register = 16 values).
 * k_ra8_glcdc_gamma_lut_depth / 2 = 8 gives the register count.
 */
typedef enum : uint8_t {
  k_ra8_glcdc_gam_reg_count = 8U, /**< LUT/AREA registers per channel (depth/2). */
  k_ra8_glcdc_gam_pair_step = 2U, /**< Entries packed per register (2 per reg).  */
} ra8_glcdc_gam_reg_count_t;

/* =============================================================================
 * Internal helpers (one per loop body to stay inside the 60-line limit)
 * =============================================================================
 */

/**
 * @brief Write one channel's 8 LUT (gain-pair) registers.
 *
 * @details
 * Packs successive pairs from `gain[]` into GAMx_LUT[0..7].  The high
 * 16-bit field (GAIN0, bits[26:16]) receives gain[2*i]; the low 11-bit
 * field (GAIN1, bits[10:0]) receives gain[2*i+1].
 *
 * @param[in] gam  Pointer to the channel's gamma register block.
 * @param[in] gain Array of k_ra8_glcdc_gamma_lut_depth 11-bit gain values.
 *
 * @pre gam != nullptr (caller guarantees).
 * @pre gain != nullptr (caller guarantees).
 * @post All 8 GAMx_LUTn registers hold the packed gain pairs.
 * @post All 16 gain[] entries (indices 0..15) are consumed; no register outside GAMx_LUT[0..7] is written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_write_lut(volatile ra8_glcdc_gam_t* gam, const uint16_t* gain)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_glcdc_gam_reg_count; i++) {
    const uint8_t  hi_idx = (uint8_t)(i * (uint8_t)k_ra8_glcdc_gam_pair_step);
    const uint8_t  lo_idx = (uint8_t)(hi_idx + 1U);
    const uint32_t packed = ((uint32_t)gain[hi_idx] << k_ra8_glcdc_gam_lut_gain_h_shift) |
                            ((uint32_t)gain[lo_idx] & (uint32_t)k_ra8_glcdc_gam_lut_gain_l_mask);
    /* HUM Ch 63 "GAMx_LUTn Gamma Correction LUT Register" p 3790 */
    gam->LUT[i] = packed;
  }
}

/**
 * @brief Write one channel's 8 AREA (threshold-pair) registers.
 *
 * @details
 * Packs successive pairs from `threshold[]` into GAMx_AREA[0..7].
 * The high 16-bit field (TH1, bits[25:16]) receives threshold[2*i];
 * the low 10-bit field (TH2, bits[9:0]) receives threshold[2*i+1].
 *
 * @param[in] gam       Pointer to the channel's gamma register block.
 * @param[in] threshold Array of k_ra8_glcdc_gamma_lut_depth 10-bit threshold values.
 *
 * @pre gam != nullptr (caller guarantees).
 * @pre threshold != nullptr (caller guarantees).
 * @post All 8 GAMx_AREAn registers hold the packed threshold pairs.
 * @post All 16 threshold[] entries (indices 0..15) are consumed; no register outside GAMx_AREA[0..7] is written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_write_area(volatile ra8_glcdc_gam_t* gam, const uint16_t* threshold)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_glcdc_gam_reg_count; i++) {
    const uint8_t  hi_idx = (uint8_t)(i * (uint8_t)k_ra8_glcdc_gam_pair_step);
    const uint8_t  lo_idx = (uint8_t)(hi_idx + 1U);
    const uint32_t packed =
      ((uint32_t)threshold[hi_idx] << k_ra8_glcdc_gam_area_th_h_shift) |
      ((uint32_t)threshold[lo_idx] & (uint32_t)k_ra8_glcdc_gam_area_th_l_mask);
    /* HUM Ch 63 "GAMx_AREAn Gamma Correction Area Register" p 3793 */
    gam->AREA[i] = packed;
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_glcdc_set_gamma(ra8_glcdc_color_channel_t channel,
                              const uint16_t*           gain,
                              const uint16_t*           threshold,
                              uint8_t                   count)
{
  RA8_CHECK_NULL_PTR(gain, s_tag, "gain must not be nullptr");
  RA8_CHECK_NULL_PTR(threshold, s_tag, "threshold must not be nullptr");
  if ((uint8_t)channel >= (uint8_t)k_ra8_glcdc_ch_count) {
    return k_ra8_err_invalid_arg;
  }
  if (count != (uint8_t)k_ra8_glcdc_gamma_lut_depth) {
    return k_ra8_err_invalid_arg;
  }

  volatile ra8_glcdc_gam_t* gam = ra8_glcdc_gam_regs((uint8_t)channel);
  RA8_CHECK_NULL_PTR(gam, s_tag, "gamma register block inaccessible");

  internal_write_lut(gam, gain);
  internal_write_area(gam, threshold);

  ra8_log_info_val(s_tag, "set_gamma channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_gamma_enable(bool enable)
{
  volatile uint32_t* const reg = ra8_glcdc_reg32(k_ra8_glcdc_off_out_gamsw);
  RA8_CHECK_NULL_PTR(reg, s_tag, "GAMSW register inaccessible");

  /* HUM Ch 63 "OUT_GAMSW Gamma Correction Block Switch Register" p 3789 */
  if (enable) {
    *reg = (uint32_t)k_ra8_glcdc_gamsw_gamon;
  } else {
    *reg = 0U;
  }

  ra8_log_info_val(s_tag, "gamma_enable", enable ? 1UL : 0UL);
  return k_ra8_ok;
}
