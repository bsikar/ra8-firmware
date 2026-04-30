/**
 * @file ra_iirfa.h
 * @brief IIR Filter Accelerator (IIRFA) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped IIR Filter Accelerator driver. IIRFA
 * is the hardware biquad-cascade engine on certain RA SKUs. The
 * RA8D2 may not include this block; the driver wraps a tiny in-RAM
 * software cascade so example projects building against the FSP
 * ``r_iirfa`` API still link and produce sensible filter outputs in
 * tests.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. This
 *          driver computes filter output in software with no DMA --
 *          treat it as an API-compatibility shim, not a perf
 *          replacement for the real accelerator.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_iirfa_limits_t
 * @brief Maximum biquad-stage count supported by the placeholder.
 */
typedef enum : uint8_t {
  k_ra_iirfa_max_stages = 4U, /**< Up to 4 cascaded biquads.   */
} ra_iirfa_limits_t;

/**
 * @struct ra_iirfa_biquad_t
 * @brief Single biquad coefficient set (Direct-Form-1, Q15-style).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  int32_t b0; /**< Numerator b0. */
  int32_t b1; /**< Numerator b1. */
  int32_t b2; /**< Numerator b2. */
  int32_t a1; /**< Denominator a1 (sign already negated for direct sum). */
  int32_t a2; /**< Denominator a2. */
} ra_iirfa_biquad_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_iirfa_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t           stage_count;                   /**< Active biquad stages 1..4. */
  uint8_t           shift;                         /**< Output right-shift factor. */
  ra_iirfa_biquad_t stages[k_ra_iirfa_max_stages]; /**< Per-stage coefficients.   */
} ra_iirfa_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the IIRFA with a config descriptor.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->stage_count`` in 1..k_ra_iirfa_max_stages.
 * @post Driver state is open and per-stage history is zero.
 * @post Coefficient table is loaded.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iirfa_open(const ra_iirfa_cfg_t* cfg);

/**
 * @brief Close the IIRFA.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iirfa_close(void);

/**
 * @brief Reset the per-stage delay-line history.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iirfa_reset(void);

/**
 * @brief Push one input sample, get one output sample.
 * @param[in] in Input sample.
 * @param[out] out_sample Filtered output.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iirfa_step(int32_t in, int32_t* out_sample);

/**
 * @brief Read packed status word.
 * @param[out] out_status Bit 0 = open.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iirfa_get_status(uint8_t* out_status);

#ifdef __cplusplus
}
#endif
