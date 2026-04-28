/**
 * @file ra_nsc_io.h
 * @brief NSC veneers for the -6 I/O drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * retrofit veneers for the analog / safety / display /
 * audio / ethernet drivers (ra_gpt, ra_adc, ra_dac_b, ra_acmphs,
 * ra_crc, ra_glcdc, ra_pdm, ra_eth). Each veneer is a Non-Secure
 * Callable entry point that validates pointer arguments and
 * forwards to the secure-side Ring-3 driver.
 *
 * MTU/TPU listed in the original plan are **not present**
 * on the RA8D2 (see the scope-correction note in the
 * roadmap) so they have no veneers here.
 *
 * ships init + the most-used primitive per driver. The
 * remaining surface is straightforward to add by following the
 * same pattern; deferred to alongside the first NS
 * consumer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_acmphs.h"
#include "ra_adc.h"
#include "ra_crc.h"
#include "ra_dac_b.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_glcdc.h"
#include "ra_gpt.h"
#include "ra_nsc_veneer.h"
#include "ra_pdm.h"

/* =============================================================================
 * GPT
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up a GPT channel.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_gpt_init(uint8_t channel, const ra_gpt_cfg_t* cfg);

/**
 * @brief NSC veneer: read the current GPT counter value.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_gpt_read(uint8_t channel, uint32_t* out);

/* =============================================================================
 * ADC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the ADC.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_adc_init(void);

/**
 * @brief NSC veneer: read one ADC channel sample.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_adc_read_channel(uint8_t channel, uint16_t* out_raw);

/* =============================================================================
 * DAC_B
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the DAC.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_dac_b_init(void);

/**
 * @brief NSC veneer: write a 12-bit value to a DAC channel.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_dac_b_write(uint8_t channel, uint16_t value);

/* =============================================================================
 * ACMPHS
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the high-speed analog comparator block.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_acmphs_init(void);

/**
 * @brief NSC veneer: read a comparator output level.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_acmphs_read_output(uint8_t channel, ra_level_t* out);

/* =============================================================================
 * CRC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the CRC unit with a polynomial.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_crc_init(ra_crc_poly_t poly);

/**
 * @brief NSC veneer: compute CRC over a buffer.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_crc_compute(const uint8_t* data,
                                                        uint32_t       len,
                                                        uint32_t*      out_crc);

/* =============================================================================
 * GLCDC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the graphics LCD controller.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_glcdc_init(const ra_glcdc_config_t* cfg);

/* =============================================================================
 * PDM
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the PDM microphone interface.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_pdm_init(void);

/* =============================================================================
 * Ethernet (lifecycle only -- frame I/O lives in ra_nsc_eth.c)
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the ethernet switch module.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_eth_init(void);

#ifdef __cplusplus
}
#endif
