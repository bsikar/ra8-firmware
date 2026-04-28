/**
 * @file ra_ulpt.h
 * @brief Ultra-Low-Power Timer (ULPT) driver header
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ulpt_regs.h"
#include "ra_err.h"

/**
 * @brief Reset both ULPT channels to their power-on state.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_init(void);

/**
 * @brief Start a ULPT channel counting down from `period`.
 *
 * @param[in] channel Channel index (0..1).
 * @param[in] period  32-bit reload value loaded into ULPT before start.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_start(uint8_t channel, uint32_t period);

/**
 * @brief Stop a ULPT channel.
 *
 * @param[in] channel Channel index (0..1).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_stop(uint8_t channel);

/**
 * @typedef ra_ulpt_event_fn_t
 * @brief ULPT event callback.
 */
typedef void (*ra_ulpt_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Tear down a ULPT channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_deinit(uint8_t channel);

/**
 * @brief Change the ULPT period at runtime.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_set_period(uint8_t channel, uint32_t period);

/**
 * @brief Read ULPTCR status bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach a ULPT event callback (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_attach_handler(ra_ulpt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a ULPT event -- fire callback.
 * @since 0.1.0
 */
void ra_ulpt_dispatch(uint8_t channel);

/**
 * @brief Put one channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ulpt_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
