/**
 * @file ra_gpt.h
 * @brief General PWM Timer (GPT) driver public header
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
 * @brief Configure a GPT channel as a free-running up-counter.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[in] period  Period in timer ticks (write to GTPR).
 * @return `ra_err_t` error code.
 *
 * @note The counter runs from PCLKD (or a configurable source) with
 *       no prescaler. The caller is responsible for choosing a
 *       sensible period value given the current PCLKD.
 */
[[nodiscard]] ra_err_t ra_gpt_start_free_run(uint8_t channel, uint32_t period);

/**
 * @brief Stop a GPT channel.
 *
 * @param[in] channel GPT channel (0..13).
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_gpt_stop(uint8_t channel);

/**
 * @brief Read the current counter value.
 *
 * @param[in]  channel GPT channel (0..13).
 * @param[out] out     Receive counter value.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_gpt_read(uint8_t channel, uint32_t* out);

#ifdef __cplusplus
}
#endif
