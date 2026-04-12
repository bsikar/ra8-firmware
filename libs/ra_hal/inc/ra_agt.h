/**
 * @file ra_agt.h
 * @brief Asynchronous General-Purpose Timer (AGT) driver header
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
 * @brief Start an AGT channel in free-running mode.
 *
 * @param[in] channel AGT channel (0..9).
 * @param[in] reload  16-bit reload value (counter starts here and
 *                    counts down toward zero).
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_agt_start_free_run(uint8_t channel, uint16_t reload);

/**
 * @brief Stop an AGT channel.
 *
 * @param[in] channel AGT channel (0..9).
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_agt_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
