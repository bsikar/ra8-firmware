/**
 * @file ra8_nsc_io.h
 * @brief NSC veneers for the I/O drivers
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Retrofit veneers for the analog / safety / display /
 * audio / ethernet drivers (ra8_gpt, ra8_adc, ra8_dac_b, ra8_acmphs,
 * ra8_crc, ra8_glcdc, ra8_pdm, ra8_eth). Each veneer is a Non-Secure
 * Callable entry point that validates pointer arguments and
 * forwards to the secure-side Ring-3 driver.
 *
 * MTU/TPU listed in the original plan are **not present**
 * on the RA8D2 (see the scope-correction note in the
 * roadmap) so they have no veneers here.
 *
 * This layer ships init + the most-used primitive per driver. The
 * remaining surface is straightforward to add by following the
 * same pattern; deferred to land alongside the first NS
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

#include "ra8_acmphs.h"
#include "ra8_adc.h"
#include "ra8_crc.h"
#include "ra8_dac_b.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_glcdc.h"
#include "ra8_gpt.h"
#include "ra8_nsc_veneer.h"
#include "ra8_pdm.h"

/* =============================================================================
 * GPT
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up a GPT channel.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_gpt_init(uint8_t channel, const ra8_gpt_cfg_t* cfg);

/**
 * @brief NSC veneer: read the current GPT counter value.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_gpt_read(uint8_t channel, uint32_t* out);

/* =============================================================================
 * ADC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the ADC.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_adc_init(void);

/**
 * @brief NSC veneer: read one ADC channel sample.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_adc_read_channel(uint8_t channel, uint16_t* out_raw);

/* =============================================================================
 * DAC_B
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the DAC.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dac_b_init(void);

/**
 * @brief NSC veneer: write a 12-bit value to a DAC channel.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dac_b_write(uint8_t channel, uint16_t value);

/* =============================================================================
 * ACMPHS
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the high-speed analog comparator block.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_acmphs_init(void);

/**
 * @brief NSC veneer: read a comparator output level.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_acmphs_read_output(uint8_t      channel,
                                                                  ra8_level_t* out);

/* =============================================================================
 * CRC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the CRC unit with a polynomial.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_crc_init(ra8_crc_poly_t poly);

/**
 * @brief NSC veneer: compute CRC over a buffer.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_crc_compute(const uint8_t* data,
                                                           uint32_t       len,
                                                           uint32_t*      out_crc);

/* =============================================================================
 * GLCDC
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the graphics LCD controller.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_glcdc_init(const ra8_glcdc_config_t* cfg);

/* =============================================================================
 * PDM
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the PDM microphone interface.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_pdm_init(void);

/* =============================================================================
 * Ethernet (lifecycle only -- frame I/O lives in ra8_nsc_eth.c)
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the ethernet switch module.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_init(void);

#ifdef __cplusplus
}
#endif
