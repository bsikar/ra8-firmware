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
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_agt_start_free_run(uint8_t channel, uint16_t reload);

/**
 * @brief Stop an AGT channel.
 *
 * @param[in] channel AGT channel (0..9).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_agt_stop(uint8_t channel);

/**
 * @typedef ra_agt_event_fn_t
 * @brief AGT event callback.
 */
typedef void (*ra_agt_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Tear down one AGT channel.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_deinit(uint8_t channel);

/**
 * @brief Change the AGT reload value at runtime.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_set_reload(uint8_t channel, uint16_t reload);

/**
 * @brief Read the AGTCR status register.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach an AGT event callback (shared across channels).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_attach_handler(ra_agt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an AGT event -- fire callback.
 * @since 0.2.0
 */
void ra_agt_dispatch(uint8_t channel);

/**
 * @brief Put one channel into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_agt_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
