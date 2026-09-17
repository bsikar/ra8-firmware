/**
 * @file ra8_systick.h
 * @brief Cortex-M85 SysTick + DWT cycle-counter timebase primitive.
 * @ingroup grp_core
 *
 * @details
 * A single hardware primitive for the two architectural timekeeping blocks the
 * RA8D2's Cortex-M85 exposes in its System Control Space (PPB window
 * 0xE000Exxx), so no caller has to own a private address-cast accessor:
 *
 *   - **SysTick** -- the 24-bit down-counter used as a periodic tick source.
 *     ::ra8_systick_reload_for turns a core clock + tick rate into a reload
 *     value, ::ra8_systick_configure arms and starts the counter,
 *     ::ra8_systick_set_reload re-arms it for a new clock without touching the
 *     control bits, and ::ra8_systick_current_value reads the live count.
 *   - **DWT cycle counter** -- a free-running counter that ticks every CPU
 *     cycle regardless of PRIMASK, making it a PRIMASK-immune fine-grained
 *     time base. ::ra8_dwt_cyccnt_enable unlocks and starts it,
 *     ::ra8_dwt_cyccnt_reset zeroes it, and ::ra8_dwt_cyccnt_read samples it.
 *
 * These are Arm v8-M *architectural* registers, NOT RA8D2 peripherals, so they
 * carry references to the Arm v8-M Architecture Reference Manual (the "Arm
 * v8-M ARM") rather than the RA8D2 Hardware User's Manual. On a host build
 * (`RA8_OFF_TARGET`) the SCS window is backed by the fake MMIO map, so every
 * access here is observable to unit tests and the reload / range logic is
 * exercised exactly as on silicon.
 *
 * @note This primitive owns register access only; it does NOT own the tick
 *       counter, the SysTick IRQ body, or any wall-clock policy. The
 *       millisecond tick counter (`ra8_time_ms` / `ra8_delay_ms`) and the
 *       ThreadX kernel-tick retune are the consumers that layer that policy on
 *       top of these accesses.
 * @note Lives in `ra8_core` (a core SCB/timebase block), not `ra8_hal`, so the
 *       Ring-1 `ra8_time` timebase can consume it without an upward
 *       `ra8_core` -> `ra8_hal` layering dependency. Higher-ring code (the
 *       ThreadX port, apps) includes it by filename just the same.
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
 * @enum ra8_systick_limit_t
 * @brief Architectural range limit of the Cortex-M SysTick reload register.
 * @details Arm v8-M ARM: SYST_RVR.RELOAD is a 24-bit field; the top byte reads
 *          as zero. A reload larger than this cannot be represented, so the
 *          configure / set-reload / reload-for entry points reject it rather
 *          than truncating (which would silently make the tick far too fast).
 */
typedef enum : uint32_t {
  k_ra8_systick_rvr_max = 0x00FFFFFFUL, /**< Max SYST_RVR reload (24-bit). */
} ra8_systick_limit_t;

/**
 * @enum ra8_systick_clock_source_t
 * @brief Selects the SysTick counter clock (SYST_CSR.CLKSOURCE).
 * @details Arm v8-M ARM: SYST_CSR.CLKSOURCE selects between the processor clock
 *          and an implementation-defined external reference clock. On the
 *          RA8D2 the processor clock is CPUCLK0, which is what every millisecond
 *          timebase in this tree derives its reload against.
 */
typedef enum : uint8_t {
  k_ra8_systick_clk_external = 0U, /**< CLKSOURCE = 0: external reference clock. */
  k_ra8_systick_clk_cpu      = 1U, /**< CLKSOURCE = 1: processor (CPU) clock.    */
} ra8_systick_clock_source_t;

/**
 * @brief Compute the SysTick reload for a given core clock and tick rate.
 *
 * @details
 * Derives the SYST_RVR reload value as `cpu_hz / tick_hz - 1`, the pure
 * arithmetic every SysTick consumer needs. Validates that neither input is
 * zero, that the clock is at least one tick period (`cpu_hz >= tick_hz`), and
 * that the resulting reload fits the 24-bit SYST_RVR field. No register is
 * touched, so this half is fully deterministic and host-testable on its own.
 *
 * @param[in]  cpu_hz     Core (SysTick) clock in Hz.
 * @param[in]  tick_hz    Desired tick rate in Hz (e.g. 1000 for a 1 ms tick).
 * @param[out] out_reload On success, receives the SYST_RVR reload value.
 *
 * @return Error code.
 * @retval k_ra8_ok              Reload computed and written to @p out_reload.
 * @retval k_ra8_err_null_ptr    @p out_reload is NULL.
 * @retval k_ra8_err_invalid_arg @p cpu_hz or @p tick_hz is zero, or @p cpu_hz
 *                               is below one tick period.
 * @retval k_ra8_err_out_of_range The reload exceeds ::k_ra8_systick_rvr_max
 *                               (@p cpu_hz far too high for @p tick_hz).
 *
 * @pre @p out_reload is a valid, writable `uint32_t`.
 * @pre @p tick_hz is the rate the caller will actually program.
 * @post On ::k_ra8_ok, `*out_reload <= k_ra8_systick_rvr_max`.
 * @post On any error, @p out_reload is left unmodified.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @par Example:
 * @code
 * uint32_t reload = 0U;
 * // 1 GHz core, 1 kHz tick -> reload = 999999.
 * (void)ra8_systick_reload_for(1000000000U, 1000U, &reload);
 * @endcode
 *
 * @see ra8_systick_configure
 * @see ra8_systick_set_reload
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_systick_reload_for(uint32_t cpu_hz, uint32_t tick_hz, uint32_t* out_reload);

/**
 * @brief Arm and start SysTick with a reload value and clock source.
 *
 * @details
 * Performs the full bring-up sequence: disable the counter, program SYST_RVR
 * with @p reload, clear SYST_CVR so counting restarts from the new reload, then
 * write SYST_CSR to enable the counter with the requested clock source and,
 * optionally, the tick interrupt. This is the exact sequence a millisecond
 * timebase performs once at init.
 *
 * @param[in] reload   SYST_RVR reload value; must be `<= k_ra8_systick_rvr_max`.
 * @param[in] src      Counter clock source (::ra8_systick_clock_source_t).
 * @param[in] tick_irq When true, enable SYST_CSR.TICKINT so the counter reaching
 *                     zero raises the SysTick exception; when false, the counter
 *                     free-runs and callers poll ::ra8_systick_current_value.
 *
 * @return Error code.
 * @retval k_ra8_ok               SysTick armed and counting.
 * @retval k_ra8_err_out_of_range @p reload exceeds ::k_ra8_systick_rvr_max.
 *
 * @pre @p reload was produced by ::ra8_systick_reload_for or is otherwise known
 *      to fit the 24-bit field.
 * @pre The SCS region is accessible (always true on Cortex-M).
 * @post On ::k_ra8_ok, SYST_RVR holds @p reload and the counter is enabled.
 * @post On any error, no SysTick register is modified.
 *
 * @note Not thread-safe; intended for single-threaded init or an IRQ-masked
 *       critical section.
 *
 * @see ra8_systick_set_reload
 * @see ra8_systick_reload_for
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_systick_configure(uint32_t reload, ra8_systick_clock_source_t src, bool tick_irq);

/**
 * @brief Re-arm SysTick with a new reload, leaving the control bits untouched.
 *
 * @details
 * Writes SYST_RVR with @p reload and clears SYST_CVR so the next count starts
 * from the new reload. Unlike ::ra8_systick_configure this does NOT touch
 * SYST_CSR, so the enable / clock-source / tick-interrupt bits already in force
 * are preserved. This is the retune path: adjust the reload for a live clock
 * change without a disable/enable glitch on the running tick.
 *
 * @param[in] reload New SYST_RVR reload value; `<= k_ra8_systick_rvr_max`.
 *
 * @return Error code.
 * @retval k_ra8_ok               Reload re-armed; current value cleared.
 * @retval k_ra8_err_out_of_range @p reload exceeds ::k_ra8_systick_rvr_max.
 *
 * @pre SysTick has already been configured (its control bits are set).
 * @pre @p reload fits the 24-bit SYST_RVR field.
 * @post On ::k_ra8_ok, SYST_RVR holds @p reload and SYST_CVR reads zero.
 * @post On any error, no SysTick register is modified.
 *
 * @note Not thread-safe; write from a single context.
 *
 * @see ra8_systick_configure
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_systick_set_reload(uint32_t reload);

/**
 * @brief Read the live SysTick current-value register (SYST_CVR).
 *
 * @details Returns the counter's current value, which counts down from SYST_RVR
 *          to zero and reloads. Reading it never clears it (only a write does),
 *          so this is a side-effect-free sample of the running counter.
 *
 * @return The 24-bit SYST_CVR value in the low bits of the result.
 * @retval 0..k_ra8_systick_rvr_max Current down-counter value.
 *
 * @pre SysTick has been configured and is counting.
 * @pre The SCS region is accessible.
 * @post No architectural state is modified (pure read).
 * @post Successive samples move monotonically down until the reload wrap.
 *
 * @note Thread-safe; single-word read of a volatile register.
 *
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_systick_current_value(void);

/**
 * @brief Enable the DWT free-running cycle counter (DWT_CYCCNT).
 *
 * @details
 * Sets DEMCR.TRCENA to unlock the DWT unit, then sets DWT_CTRL.CYCCNTENA to
 * start DWT_CYCCNT counting every CPU cycle. Both are read-modify-write so any
 * other trace/debug bits already set are preserved. The counter is PRIMASK-immune:
 * it keeps advancing even while interrupts are globally masked, which is what
 * makes it a usable delay reference in early boot and IRQ-off critical sections.
 *
 * @pre The Cortex-M85 core is running.
 * @pre The SCS region is accessible.
 * @post DEMCR.TRCENA and DWT_CTRL.CYCCNTENA are both set.
 * @post DWT_CYCCNT is advancing once per CPU cycle.
 *
 * @note Not thread-safe (read-modify-write of two registers); call once at init.
 *
 * @see ra8_dwt_cyccnt_read
 * @see ra8_dwt_cyccnt_reset
 *
 * @since 0.1.0
 */
void ra8_dwt_cyccnt_enable(void);

/**
 * @brief Zero the DWT cycle counter (DWT_CYCCNT).
 *
 * @details Writes zero to DWT_CYCCNT so a subsequent ::ra8_dwt_cyccnt_read
 *          measures cycles elapsed from this call. Does not enable or disable
 *          the counter; ::ra8_dwt_cyccnt_enable must already have started it for
 *          the reset to matter.
 *
 * @pre ::ra8_dwt_cyccnt_enable has been called (or the counter is otherwise on).
 * @pre The SCS region is accessible.
 * @post DWT_CYCCNT reads approximately zero (it advances immediately after).
 * @post No other DWT or DEMCR bit is modified.
 *
 * @note Not thread-safe; intended to bracket a measured region.
 *
 * @see ra8_dwt_cyccnt_read
 *
 * @since 0.1.0
 */
void ra8_dwt_cyccnt_reset(void);

/**
 * @brief Sample the DWT cycle counter (DWT_CYCCNT).
 *
 * @details Reads the free-running 32-bit cycle counter. Wraps every 2^32 CPU
 *          cycles (~4.3 s at 1 GHz); compare two samples with unsigned
 *          subtraction so the elapsed count stays correct across a wrap.
 *
 * @return The current DWT_CYCCNT value.
 * @retval 0..UINT32_MAX CPU cycles counted since the counter was last zeroed,
 *                       modulo 2^32.
 *
 * @pre ::ra8_dwt_cyccnt_enable has been called.
 * @pre The SCS region is accessible.
 * @post No architectural state is modified (pure read).
 * @post Successive samples are non-decreasing modulo 2^32.
 *
 * @note Thread-safe; single-word read of a volatile register.
 *
 * @see ra8_dwt_cyccnt_enable
 *
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_dwt_cyccnt_read(void);

#ifdef __cplusplus
}
#endif
