/**
 * @file ra_time.h
 * @brief SysTick-based tick counter, delay and timestamp helpers
 *
 * @details
 * Uses the Cortex-M SysTick peripheral (architectural, always
 * present) as a 1 kHz tick source. Exposes three entry points:
 *
 *  - `ra_time_init(cpu_hz)`: programme SysTick to fire every 1 ms
 *    given the current CPU clock in Hz.
 *  - `ra_time_ms()`: return the current tick count in milliseconds.
 *  - `ra_delay_ms(ms)`: busy-wait for `ms` milliseconds.
 *
 * The tick counter is a 32-bit `uint32_t` and wraps every ~49.7 days.
 * Code that compares timestamps should use subtraction so the
 * comparison stays monotonic across wrap.
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
 * @brief Initialise SysTick for a 1 kHz tick interrupt.
 *
 * @param[in] cpu_hz Current CPU clock in Hz. Used to compute the
 *                   SysTick reload value (`cpu_hz / 1000 - 1`).
 *
 * @return `k_ra_ok` on success, `k_ra_err_invalid_arg` if `cpu_hz`
 *         is zero or would yield a zero reload.
 *
 * @note Must be called after `ra_cgc_init()` so the CPU clock is
 *       stable.
 */
[[nodiscard]] ra_err_t ra_time_init(uint32_t cpu_hz);

/**
 * @brief Get the current 1 kHz tick count.
 *
 * @return Milliseconds since `ra_time_init()`, wrapping at 2^32.
 *
 * @note Thread-safe (single-word read).
 */
uint32_t ra_time_ms(void);

/**
 * @brief Busy-wait for at least `ms` milliseconds.
 *
 * @param[in] ms Milliseconds to wait. Zero returns immediately.
 *
 * @note Wall-clock accuracy depends on the tick interrupt not being
 *       masked for the duration of the call. In practice the delay
 *       can be off by up to one tick on either side.
 */
void ra_delay_ms(uint32_t ms);

/**
 * @brief SysTick IRQ handler -- called from the vector table.
 *
 * @details
 * Override the weak `SysTick_Handler` alias with `ra_time_on_tick`
 * by declaring `void SysTick_Handler(void) { ra_time_on_tick(); }`
 * somewhere in the application, OR link `ra_time.c` with the
 * already-provided `SysTick_Handler` implementation in this file.
 */
void ra_time_on_tick(void);

/* =============================================================================
 * Short aliases (wrappers exposed by timer.c)
 * =============================================================================
 */

/** @brief Short-form alias for `ra_time_ms()`. */
uint32_t ra_now_ms(void);

/** @brief Short-form alias for `ra_delay_ms()`. */
void ra_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
