/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ulpt.h
 * @brief Ultra-Low-Power Timer (ULPT) driver header
 * @ingroup grp_hal_timers
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ulpt_regs.h"

/**
 * @brief Reset both ULPT channels to their power-on state.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_init(void);

/**
 * @brief Start a ULPT channel counting down from `period`.
 *
 * @param[in] channel Channel index (0..1).
 * @param[in] period  32-bit reload value loaded into ULPT before start.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_start(uint8_t channel, uint32_t period);

/**
 * @brief Stop a ULPT channel and confirm the counter has halted.
 *
 * @details
 * Toggles ULPTCR.TSTOP (HUM Ch 25.2.1 "ULPTCR", p 1190) to force the
 * down-counter to stop, clears ULPTCR, then polls the count-status flag
 * ULPTCR.TCSTF until it reads 0. TCSTF lags the stop request by a few
 * ULPTLCLK (32.768 kHz) cycles, so the confirm poll is bounded by
 * ``k_ra8_ulpt_stop_poll_max`` (NASA Power of 10 Rule 2). Confirming the
 * halt matters before a caller re-arms the channel for Software Standby:
 * re-arming while TCSTF is still set hangs on the second wake.
 *
 * @param[in] channel Channel index (0..1).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              Counter stopped and TCSTF confirmed low.
 * @retval k_ra8_err_invalid_arg `channel` >= 2.
 * @retval k_ra8_err_hw_timeout  TCSTF stayed set past the poll bound.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre Channel was previously started via `ra8_ulpt_start`.
 *
 * @post On `k_ra8_ok` the counter is stopped (ULPTCR.TCSTF == 0).
 * @post ULPTCR.TSTART is clear (the channel is not re-armed).
 *
 * @note Not thread-safe.
 * @see ra8_ulpt_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_stop(uint8_t channel);

/**
 * @typedef ra8_ulpt_event_fn_t
 * @brief ULPT event callback.
 */
typedef void (*ra8_ulpt_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Tear down a ULPT channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_deinit(uint8_t channel);

/**
 * @brief Change the ULPT period at runtime.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_set_period(uint8_t channel, uint32_t period);

/**
 * @brief Read ULPTCR status bits.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach a ULPT event callback (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_attach_handler(ra8_ulpt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a ULPT event -- fire callback.
 *
 * @details
 * Called from the ULPT compare-match / underflow ISR (HUM Ch 26
 * "Ultra-Low Power Timer (ULPT)", p 1273) to invoke the registered
 * handler. Out-of-range ``channel`` and unattached slots are silently
 * ignored so spurious IRQs are harmless.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``channel`` < 2.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post No ULPT register state is mutated by this call.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra8_ulpt_dispatch(uint8_t channel);

/**
 * @brief Put one channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ulpt_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
