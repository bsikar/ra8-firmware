/**
 * @file ra_adc.h
 * @brief ADC_B polling driver public header
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
 * @brief Initialise the ADC_B peripheral for single-shot software-
 *        triggered conversions.
 *
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_adc_init(void);

/**
 * @brief Blocking single-channel sample.
 *
 * @param[in]  channel Analog channel index (0..47 on RA8D2 ADC_B).
 * @param[out] out_raw Pointer to receive the 14-bit result right-aligned.
 * @return `ra_err_t` error code.
 *
 * @note Stubbed while the ADC_B register layout is still a
 *       placeholder; the implementation logs a warning and returns
 *       `k_ra_err_not_supported`.
 */
[[nodiscard]] ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw);

#ifdef __cplusplus
}
#endif
