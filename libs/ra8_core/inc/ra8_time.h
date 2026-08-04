/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_time.h
 * @brief SysTick-based tick counter, delay and timestamp helpers
 * @ingroup grp_core
 *
 * @details
 * Uses the Cortex-M SysTick peripheral (architectural, always
 * present) as a 1 kHz tick source. Exposes three entry points:
 *
 *  - `ra8_time_init(cpu_hz)`: programme SysTick to fire every 1 ms
 *    given the current CPU clock in Hz.
 *  - `ra8_time_ms()`: return the current tick count in milliseconds.
 *  - `ra8_delay_ms(ms)`: busy-wait for `ms` milliseconds.
 *
 * The tick counter is a 32-bit `uint32_t` and wraps every ~49.7 days.
 * Code that compares timestamps should use subtraction so the
 * comparison stays monotonic across wrap.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Initialise SysTick for a 1 kHz tick interrupt.
 *
 * @param[in] cpu_hz Current CPU clock in Hz. Used to compute the
 *                   SysTick reload value (`cpu_hz / 1000 - 1`).
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_invalid_arg` if `cpu_hz`
 *         is zero or would yield a zero reload, or the error propagated
 *         from `ra8_systick_configure()` when the reload does not fit the
 *         24-bit SysTick field (target build only; unreachable at any RA8D2
 *         clock rate).
 *
 * @note Must be called after `ra8_cgc_init()` so the CPU clock is
 *       stable.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_time_init(uint32_t cpu_hz);

/**
 * @brief Get the current 1 kHz tick count.
 *
 * @details Returns the SysTick-incremented `s_tick_ms` counter.
 *          Wraps every ~49.7 days; compare via subtraction.
 *
 * @return Milliseconds since `ra8_time_init()`, wrapping at 2^32.
 * @retval 0..UINT32_MAX  Monotonically increasing modulo 2^32.
 *
 * @pre `ra8_time_init()` has been called and SysTick IRQ is firing.
 * @pre Reader is OK with single-word atomicity.
 * @post No internal state modified.
 * @post Successive calls return non-decreasing values modulo 2^32.
 *
 * @note Thread-safe (single-word read on Cortex-M is atomic).
 *
 * @since 0.1.0
 */
uint32_t ra8_time_ms(void);

/**
 * @brief Busy-wait for at least `ms` milliseconds.
 *
 * @details Loops on `ra8_time_ms()` and issues `wfi` between checks.
 *
 * @param[in] ms Milliseconds to wait. Zero returns immediately.
 *
 * @pre `ra8_time_init()` has been called.
 * @pre IRQs are NOT globally masked.
 * @post At least `ms` milliseconds have elapsed.
 * @post No internal state modified.
 *
 * @note Wall-clock accuracy depends on the tick interrupt not being
 *       masked for the duration of the call. Thread-safe.
 *
 * @since 0.1.0
 */
void ra8_delay_ms(uint32_t ms);

/**
 * @brief SysTick IRQ handler -- called from the vector table.
 *
 * @details
 * Override the weak `SysTick_Handler` alias with `ra8_time_on_tick`
 * by declaring `void SysTick_Handler(void) { ra8_time_on_tick(); }`
 * somewhere in the application, OR link `ra8_time.c` with the
 * already-provided `SysTick_Handler` implementation in this file.
 *
 * @pre Invoked from SysTick IRQ context (or test equivalent).
 * @pre `ra8_time_init()` has set up the SysTick reload value.
 * @post `s_tick_ms` is incremented by exactly one.
 * @post No other state modified.
 *
 * @note IRQ-safe; SysTick cannot pre-empt itself.
 *
 * @since 0.1.0
 */
void ra8_time_on_tick(void);

/* =============================================================================
 * Short aliases (wrappers exposed by timer.c)
 * =============================================================================
 */

/**
 * @brief Short-form alias for `ra8_time_ms()`.
 *
 * @details Convenience wrapper used by code that prefers a "now" verb.
 *
 * @return Current millisecond tick count modulo 2^32.
 * @retval 0..UINT32_MAX  See `ra8_time_ms()`.
 *
 * @pre Same as `ra8_time_ms()`.
 * @pre None additional.
 * @post No state modified.
 * @post Result equals `ra8_time_ms()` at the moment of the call.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
uint32_t ra8_now_ms(void);

/**
 * @brief Short-form alias for `ra8_delay_ms()`.
 *
 * @details Convenience wrapper that forwards to `ra8_delay_ms()`.
 *
 * @param[in] ms Milliseconds to sleep. Zero returns immediately.
 *
 * @pre Same as `ra8_delay_ms()`.
 * @pre None additional.
 * @post At least `ms` milliseconds have elapsed.
 * @post No internal state modified.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
void ra8_sleep_ms(uint32_t ms);

/* =============================================================================
 * Concrete DI interface instance (defined in ra8_time.c)
 * =============================================================================
 */

#include "ra8_time_interface.h"

/**
 * @brief Production `ra8_time_interface_t` that forwards to the
 *        SysTick-backed `ra8_time_ms` / `ra8_delay_ms` helpers.
 */
extern const ra8_time_interface_t g_ra8_time_interface_systick;

#ifdef __cplusplus
}
#endif
