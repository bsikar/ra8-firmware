/**
 * @file port/threadx/inc/ra8_threadx.h
 * @brief RA8 <-> Eclipse ThreadX glue: SysTick kernel-tick retuning.
 *
 * @details
 * The project-tuned `tx_initialize_low_level.S` programs SysTick from a
 * compile-time clock assumption (`RA8_BOOT_CLOCK_HZ`, 1 GHz -- the
 * post-CGC CPUCLK0 target). That is correct only if the app raised
 * CPUCLK0 to exactly that rate before `tx_kernel_enter()`. An app that
 * enters the kernel on a different clock -- the boot-default MOCO
 * (~8 MHz), or any other CGC target -- would run the ThreadX kernel tick
 * at the wrong rate: a nominal 1 ms tick scaled by the ratio of the
 * compile-time assumption to the live clock (~119x too slow on MOCO).
 *
 * ::ra8_threadx_systick_retune closes that gap. Called from
 * `tx_application_define` -- which ThreadX runs AFTER
 * `_tx_initialize_low_level` but BEFORE the first scheduling decision --
 * it re-derives SysTick.LOAD from the *live* CPUCLK0 rate reported by
 * ::ra8_cgc_get_clock_hz, so the kernel tick is accurate regardless of
 * which clock the app brought up. If the live clock is so high that the
 * per-tick reload cannot fit the 24-bit SYST_RVR register, the function
 * returns an error rather than silently truncating the reload -- the
 * caller can then halt instead of drifting.
 *
 * ::ra8_threadx_systick_reload_for is the pure arithmetic core, split out
 * so the range/validity logic is host-unit-testable without touching the
 * Cortex-M System Control Space.
 *
 * @note All functions here run in a single-threaded init context
 *       (`tx_application_define`, IRQs masked). None are thread-safe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_threadx_tick_t
 * @brief ThreadX kernel tick rate used to derive the SysTick reload.
 *
 * @details
 * Mirrors `TX_TIMER_TICKS_PER_SECOND` in `port/threadx/inc/tx_user.h`.
 * The two MUST agree: `tx_user.h` declares the rate ThreadX assumes for
 * `tx_thread_sleep` / timer math, and this value is what
 * ::ra8_threadx_systick_retune programs SysTick to fire at.
 *
 * @invariant Equal to `TX_TIMER_TICKS_PER_SECOND`.
 * @see ra8_threadx_systick_retune
 */
typedef enum : uint32_t {
  k_ra8_threadx_tick_hz = 1000UL, /**< 1 kHz -> 1 ms kernel tick. */
} ra8_threadx_tick_t;

/**
 * @enum ra8_systick_reload_limit_t
 * @brief Architectural range limit of the Cortex-M SysTick reload register.
 *
 * @details
 * SYST_RVR (0xE000E014) has a 24-bit RELOAD field (bits [23:0]); the top
 * byte reads as zero. A reload larger than this cannot be represented, so
 * ::ra8_threadx_systick_reload_for rejects it instead of truncating.
 *
 * @invariant Equal to `(1 << 24) - 1`.
 */
typedef enum : uint32_t {
  k_ra8_systick_reload_max = 0x00FFFFFFUL, /**< Max SYST_RVR reload (24-bit). */
} ra8_systick_reload_limit_t;

/**
 * @brief Compute the SysTick reload for a given core clock + tick rate.
 *
 * @details
 * Derives the SYST_RVR reload value as `cpuclk_hz / tick_hz - 1`, the
 * count that makes SysTick underflow once per tick period. Validates that
 * the inputs are non-zero, that the clock is fast enough for at least one
 * tick period (`cpuclk_hz >= tick_hz`), and that the resulting reload fits
 * the 24-bit SYST_RVR field. Pure arithmetic -- touches no hardware -- so
 * it is exercised directly by the host MC/DC unit tests.
 *
 * @param[in]  cpuclk_hz  Live core (CPUCLK0) frequency in Hz. Must be > 0.
 * @param[in]  tick_hz    Kernel tick rate in Hz (see ::k_ra8_threadx_tick_hz).
 *                        Must be > 0.
 * @param[out] out_reload On success, the SYST_RVR reload value to program.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok              Reload computed and written to @p out_reload.
 * @retval k_ra8_err_null_ptr    @p out_reload is NULL.
 * @retval k_ra8_err_invalid_arg @p cpuclk_hz or @p tick_hz is 0, or the
 *                               clock is slower than one tick period.
 * @retval k_ra8_err_out_of_range Reload would exceed the 24-bit SYST_RVR
 *                               range (@p cpuclk_hz far too high for @p tick_hz).
 *
 * @pre @p out_reload is a valid, writable `uint32_t`.
 * @pre Caller supplies the *live* clock, not a compile-time assumption.
 * @post On ::k_ra8_ok, `*out_reload <= k_ra8_systick_reload_max`.
 * @post On any error, @p out_reload is left unmodified.
 *
 * @note Thread safety: pure function, trivially thread-safe.
 *
 * @par Example:
 * @code
 * uint32_t reload = 0U;
 * // 1 GHz core, 1 kHz tick -> 999999.
 * (void)ra8_threadx_systick_reload_for(1000000000U, 1000U, &reload);
 * @endcode
 *
 * @see ra8_threadx_systick_retune
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_threadx_systick_reload_for(uint32_t cpuclk_hz, uint32_t tick_hz, uint32_t* out_reload);

/**
 * @brief Reprogram SysTick.LOAD from the live CPUCLK0 rate.
 *
 * @details
 * Queries the live CPUCLK0 frequency via ::ra8_cgc_get_clock_hz, derives
 * the reload with ::ra8_threadx_systick_reload_for for the ThreadX tick
 * rate (::k_ra8_threadx_tick_hz), then writes SYST_RVR and clears SYST_CVR
 * so the new period takes effect on the next reload. The SysTick enable /
 * clock-source / interrupt bits programmed by `_tx_initialize_low_level.S`
 * are left untouched.
 *
 * Call this from `tx_application_define` (which ThreadX invokes after
 * `_tx_initialize_low_level` but before the first scheduling decision) so
 * that whatever clock the app raised via `ra8_cgc_init()` -- or did not --
 * the kernel tick is accurate. Calling it earlier (from `main` before
 * `tx_kernel_enter`) has no effect: `_tx_initialize_low_level` reprograms
 * SYST_RVR afterwards and would overwrite it.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok              SysTick reprogrammed for the live clock.
 * @retval k_ra8_err_invalid_arg CPUCLK0 query returned a nonsensical rate
 *                               (0, or too slow for one tick period).
 * @retval k_ra8_err_out_of_range Live clock too high for a 24-bit reload.
 *
 * @pre `_tx_initialize_low_level` has already armed SysTick (i.e. this runs
 *      from `tx_application_define` or later).
 * @pre The CGC published-clock table reflects the live clock (i.e.
 *      `ra8_cgc_init()` has run if the app raises the clock).
 * @post On ::k_ra8_ok, SYST_RVR == `CPUCLK0 / k_ra8_threadx_tick_hz - 1`.
 * @post On ::k_ra8_ok, SYST_CVR == 0 (counter restarts at the new reload).
 *
 * @note Thread safety: not thread-safe; single-threaded init context only.
 * @note On the host unit-test build (`RA8_OFF_TARGET`) the SYST_RVR /
 *       SYST_CVR writes are skipped -- the System Control Space is not
 *       mapped -- but the clock query + reload validation still run.
 *
 * @par Example:
 * @code
 * void tx_application_define(void* unused) {
 *   (void)unused;
 *   if (ra8_threadx_systick_retune() != k_ra8_ok) { for (;;) { } }
 *   // ... tx_thread_create(...) ...
 * }
 * @endcode
 *
 * @see ra8_threadx_systick_reload_for
 * @see ra8_cgc_get_clock_hz
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_threadx_systick_retune(void);

#ifdef __cplusplus
}
#endif
