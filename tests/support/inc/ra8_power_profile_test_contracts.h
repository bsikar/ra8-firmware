/**
 * @file ra8_power_profile_test_contracts.h
 * @brief Private contracts for the power-profile unit-test helpers.
 * @details Declares file-local callbacks, fixture setup, and test vectors so
 * their complete contracts remain readable without inflating the test body.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Record one profiler GPIO edge in the bounded mock log.
 * @details Appends the region and direction while capacity remains; excess
 * pulses are deliberately ignored to keep the callback infallible.
 * @param[in] ctx Pointer to the active `mock_gpio_t` fixture.
 * @param[in] region_id Region whose marker edge was emitted.
 * @param[in] entering True for entry and false for exit.
 * @pre `ctx` addresses a live writable mock GPIO fixture.
 * @pre The callback executes synchronously in the single-threaded test.
 * @post At most one bounded pulse record is appended.
 * @post No state outside the supplied fixture is modified.
 * @note Models an optional production observation hook.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_mock_gpio_pulse(void* ctx, ra8_power_profile_region_id_t region_id, bool entering);

/**
 * @brief Return the current timestamp held by the RTC mock.
 * @details Reads the caller-controlled microsecond value without advancing it.
 * @param[in] ctx Pointer to the active `mock_rtc_t` fixture.
 * @return Current mock timestamp in microseconds.
 * @retval UINT64_MAX A valid value when explicitly stored by the test.
 * @pre `ctx` addresses a live readable RTC fixture.
 * @pre The caller owns any sequencing of timestamp updates.
 * @post The returned value equals the fixture's `now_us` field.
 * @post No fixture or profiler state is modified.
 * @note The mock is monotonic only when the calling vector makes it so.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_mock_rtc_now_us(void* ctx);

/**
 * @brief Clear both injected-hook fixtures.
 * @details Resets the pulse log, count, and current timestamp to zero.
 * @pre The file-local GPIO and RTC fixtures are available.
 * @pre No profiler call is active concurrently.
 * @post Every byte of both fixtures is zero.
 * @post No production profiler state is changed directly.
 * @note Called before each configuration-based vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_mocks(void);

/**
 * @brief Reset the fixtures and initialize the profiler with both mocks.
 * @details Binds the pulse and clock callbacks with their respective contexts.
 * @return Profiler initialization status.
 * @retval k_ra8_ok The mock-backed profiler was initialized.
 * @pre The profiler accepts reinitialization between unit-test vectors.
 * @pre The file-local fixtures have static lifetime.
 * @post Both fixtures are reset before the initialization attempt.
 * @post On success, subsequent marks use the mock callbacks.
 * @note Centralizes the normal test configuration.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_init_with_mocks(void);

/**
 * @brief Verify initialization rejects a null configuration.
 * @details Exercises the public null-pointer guard without touching fixtures.
 * @pre The profiler implementation is linked into the test.
 * @pre Unity-style assertions are available.
 * @post `k_ra8_err_null_ptr` is observed.
 * @post No hook configuration is published.
 * @note Covers the initialization trust boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_rejects_null_cfg(void);

/**
 * @brief Verify initialization clears every public statistics slot.
 * @details Initializes with mocks and inspects all region counters and state.
 * @pre The region enumeration fits the public statistics array.
 * @pre Mock-backed initialization succeeds.
 * @post Every entry, exit, duration, and open flag is zero.
 * @post The mock pulse log remains empty.
 * @note Iterates the complete configured region range.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_zeroes_stats(void);

/**
 * @brief Verify enter and exit marks emit ordered GPIO pulses.
 * @details Marks one active interval and checks both recorded edge payloads.
 * @pre Mock-backed initialization succeeds.
 * @pre The pulse-log capacity exceeds two records.
 * @post Exactly one entering and one exiting record are present in order.
 * @post Both records identify the active region.
 * @note Timestamp changes also exercise the ordinary mark path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_enter_exit_pulses_gpio(void);

/**
 * @brief Verify multiple spans accumulate their elapsed time.
 * @details Records two sleep intervals and inspects the resulting counters.
 * @pre Mock-backed initialization succeeds.
 * @pre Each exit timestamp is not earlier than its paired entry.
 * @post Two entries and two exits are reported.
 * @post Total sleep time equals the sum of both intervals and is closed.
 * @note Uses distinct spans to prove accumulation rather than replacement.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_time_accumulation(void);

/**
 * @brief Verify exiting a closed region reports invalid state.
 * @details Calls exit without a matching entry and inspects its counters.
 * @pre Mock-backed initialization succeeds.
 * @pre The snooze region starts closed.
 * @post The exit returns `k_ra8_err_invalid_state`.
 * @post The region records one exit and zero entries.
 * @note Confirms diagnostic accounting on the rejection path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_exit_without_enter_reports_invalid_state(void);

/**
 * @brief Verify region identifiers at and above the limit are rejected.
 * @details Exercises both enter and exit range checks with invalid values.
 * @pre Mock-backed initialization succeeds.
 * @pre The maximum-region enumerator is the first invalid identifier.
 * @post Both operations return `k_ra8_err_range_check_failed`.
 * @post No out-of-bounds statistics slot is accessed.
 * @note Covers the public identifier trust boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_region_id_range_check(void);

/**
 * @brief Verify statistics retrieval rejects a null destination.
 * @details Calls the public getter after normal initialization.
 * @pre Mock-backed initialization succeeds.
 * @pre No other profiler operation is active.
 * @post The getter returns `k_ra8_err_null_ptr`.
 * @post Internal counters and fixtures remain unchanged.
 * @note Isolates the output-pointer guard.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_get_stats_null(void);

/**
 * @brief Verify reset clears previously accumulated statistics.
 * @details Creates a completed active interval, resets, and checks all slots.
 * @pre Mock-backed initialization succeeds.
 * @pre A complete interval can be recorded before reset.
 * @post Every region's entry, exit, and elapsed counters are zero.
 * @post The public reset call returns `k_ra8_ok`.
 * @note The callback fixtures are intentionally not the reset target.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_reset_stats_clears_accumulators(void);

/**
 * @brief Verify both optional hooks may be absent safely.
 * @details Initializes with null callbacks, records one interval, and reads it.
 * @pre A zero-initialized configuration denotes absent optional hooks.
 * @pre The selected user region is within range.
 * @post One entry and one exit are counted with zero elapsed time.
 * @post No callback is invoked through a null pointer.
 * @note Confirms instrumentation can be disabled without disabling accounting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_null_hooks_safe(void);

/**
 * @brief Verify nested regions accumulate independently.
 * @details Interleaves standby inside sleep and inspects both elapsed totals.
 * @pre Mock-backed initialization succeeds.
 * @pre The supplied timeline is monotonic for both nested intervals.
 * @post Sleep reports 400 microseconds.
 * @post Software standby reports 150 microseconds.
 * @note Demonstrates region state is not stored in one shared timer slot.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_multiple_regions_independent(void);
