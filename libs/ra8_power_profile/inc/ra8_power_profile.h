/**
 * @file ra8_power_profile.h
 * @brief Power-profiling helper -- public API
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Lightweight Dynamic-Trace-Style (DTS) power-profiling helper. The
 * library lets application code wrap regions of execution with
 * ``ra8_power_profile_mark_enter`` / ``ra8_power_profile_mark_exit``
 * pairs so that an external power analyser (e.g. a Joulescope or a
 * scope+shunt) can correlate measured current draw with named code
 * regions. Each enter/exit edge pulses a configured GPIO so the
 * analyser sees a clean trigger; on the firmware side the helper
 * also accumulates per-region elapsed time and entry counts, which
 * doubles as a coarse wall-clock budget tracker.
 *
 * The library is decoupled from any concrete GPIO or RTC HAL via
 * function-pointer hooks (Dependency Inversion -- see CLAUDE.md
 * "SOLID Principles for C"). Production code wires the hooks to
 * ``ra8_gpio_*`` and ``ra8_rtc_*``; unit tests inject mocks. There
 * are no calls into ra8_hal/ from this translation unit.
 *
 * Layout in this firmware:
 *   - ``inc/ra8_power_profile.h``  public API (this file)
 *   - ``src/ra8_power_profile.c``  implementation
 *
 * Region IDs are a fixed-size C23 typed enum capped at
 * ``k_ra8_power_profile_max_regions`` (16) to satisfy NASA Power-of-10
 * Rule 3 (no dynamic allocation): all per-region counters live in a
 * static array sized at compile time.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_power_profile_limits_t
 * @brief Static sizing constants for the profiler.
 *
 * @details
 * Used to bound the per-region accumulator array at compile time
 * (NASA Rule 3). Region IDs must be in ``[0, k_ra8_power_profile_max_regions)``.
 *
 * @invariant ``k_ra8_power_profile_max_regions`` is exactly 16.
 *
 * @see ra8_power_profile_region_id_t
 */
typedef enum : uint8_t {
  k_ra8_power_profile_max_regions = 16, /**< Maximum number of distinct regions tracked. */
} ra8_power_profile_limits_t;

/**
 * @enum ra8_power_profile_region_id_t
 * @brief Default named region IDs.
 *
 * @details
 * Applications are free to use raw integer values in
 * ``[0, k_ra8_power_profile_max_regions)`` instead of these names.
 * The enumeration provides a few suggested labels that match common
 * low-power-mode regions on the RA8D2: active, sleep, software-
 * standby, deep-software-standby, and snooze.
 *
 * @invariant Values fit in ``uint8_t``.
 */
typedef enum : uint8_t {
  k_ra8_power_profile_region_active           = 0, /**< CPU active / running mode.         */
  k_ra8_power_profile_region_sleep            = 1, /**< Sleep mode (CPU clock stopped).    */
  k_ra8_power_profile_region_software_standby = 2, /**< Software-standby mode.             */
  k_ra8_power_profile_region_deep_standby     = 3, /**< Deep-software-standby mode.        */
  k_ra8_power_profile_region_snooze           = 4, /**< Snooze (peripheral-driven wake).   */
  k_ra8_power_profile_region_user_0           = 5, /**< First freely-assignable user slot. */
} ra8_power_profile_region_id_t;

/**
 * @typedef ra8_power_profile_gpio_pulse_fn_t
 * @brief GPIO-toggle hook used to emit profiler edges.
 *
 * @details
 * Called twice per region transition (rising + falling edges) so the
 * external analyser can detect a precise boundary. The implementation
 * is expected to drive a single GPIO pin from the configured pulse
 * level back to the idle level.
 *
 * @param[in] ctx        Opaque hook context (passed through from cfg).
 * @param[in] region_id  Region being entered or exited.
 * @param[in] entering   ``true`` if marking entry, ``false`` if marking exit.
 *
 * @note Must not block. Must be safe to call from the same context
 *       as ``ra8_power_profile_mark_enter`` / ``_exit``.
 *
 * @since 0.1.0
 */
typedef void (*ra8_power_profile_gpio_pulse_fn_t)(void*                         ctx,
                                                  ra8_power_profile_region_id_t region_id,
                                                  bool                          entering);

/**
 * @typedef ra8_power_profile_now_us_fn_t
 * @brief Wall-clock timestamp hook (microseconds since some epoch).
 *
 * @details
 * Returns a monotonically non-decreasing 64-bit microsecond
 * timestamp. The profiler subtracts the enter timestamp from the
 * exit timestamp to compute time-in-region. The hook is typically
 * backed by RTC sub-second counters or by AGT/GPT.
 *
 * @param[in] ctx Opaque hook context (passed through from cfg).
 * @return Current time, in microseconds.
 *
 * @note Must be monotonically non-decreasing across calls.
 *
 * @since 0.1.0
 */
typedef uint64_t (*ra8_power_profile_now_us_fn_t)(void* ctx);

/**
 * @struct ra8_power_profile_region_stats_t
 * @brief Per-region accumulator returned by ``ra8_power_profile_get_stats``.
 *
 * @details
 * Captures everything the profiler tracks per region: how many
 * enter/exit pairs were observed, total time accumulated, and the
 * timestamp / count of any unmatched enter events still in-flight.
 *
 * @invariant ``entries >= exits``. The difference is the number of
 *            currently-open regions for that ID.
 * @invariant ``total_time_us`` only counts fully-closed regions
 *            (enter+exit pairs); open regions do not contribute.
 *
 * @see ra8_power_profile_stats_t
 * @since 0.1.0
 */
typedef struct {
  uint64_t entries;       /**< Total number of ``mark_enter`` calls for this region.      */
  uint64_t exits;         /**< Total number of ``mark_exit`` calls for this region.       */
  uint64_t total_time_us; /**< Sum of (exit - enter) over all closed pairs, microseconds. */
  uint64_t last_enter_us; /**< Timestamp of the most recent unmatched ``mark_enter``.     */
  bool     is_open;       /**< ``true`` if a ``mark_enter`` is awaiting a ``mark_exit``.  */
} ra8_power_profile_region_stats_t;

/**
 * @struct ra8_power_profile_stats_t
 * @brief Aggregate snapshot of all per-region accumulators.
 *
 * @details
 * Flat array indexed by ``ra8_power_profile_region_id_t`` value.
 * Slots that were never used hold all-zero accumulators.
 *
 * @invariant ``regions`` is exactly ``k_ra8_power_profile_max_regions`` long.
 *
 * @since 0.1.0
 */
typedef struct {
  /** Per-region accumulators. */
  ra8_power_profile_region_stats_t regions[k_ra8_power_profile_max_regions];
} ra8_power_profile_stats_t;

/**
 * @struct ra8_power_profile_config_t
 * @brief Initialisation configuration.
 *
 * @details
 * Wires the profiler to the application's GPIO and time-base hooks.
 * Either hook may be ``nullptr`` -- a null GPIO hook turns the helper
 * into a pure software accumulator (useful in unit tests); a null
 * time-base hook disables ``total_time_us`` accumulation while still
 * counting entries.
 *
 * @invariant ``user_ctx_gpio`` and ``user_ctx_time`` are passed
 *            through verbatim to the hooks; the profiler never
 *            inspects them.
 *
 * @code
 * static void board_pulse(void* c, ra8_power_profile_region_id_t r, bool e) {
 *     (void)c; (void)r;
 *     ra8_gpio_write(k_ra8_port_4, 0, e ? k_ra8_level_high : k_ra8_level_low);
 * }
 * static uint64_t board_now_us(void* c) { (void)c; return ra8_rtc_now_us(); }
 *
 * ra8_power_profile_config_t cfg = {
 *     .pulse    = board_pulse,
 *     .now_us   = board_now_us,
 * };
 * ra8_power_profile_init(&cfg);
 * @endcode
 *
 * @see ra8_power_profile_init
 * @since 0.1.0
 */
typedef struct {
  ra8_power_profile_gpio_pulse_fn_t pulse;         /**< GPIO edge hook (may be ``nullptr``). */
  ra8_power_profile_now_us_fn_t     now_us;        /**< Clock hook, us (may be ``nullptr``). */
  void*                             user_ctx_gpio; /**< Opaque pointer passed to ``pulse``.  */
  void*                             user_ctx_time; /**< Opaque pointer passed to ``now_us``. */
} ra8_power_profile_config_t;

/**
 * @brief Initialise the power profiler.
 *
 * @details
 * Stores the supplied hooks, zeroes every per-region accumulator, and
 * marks the module as initialized. Calling ``ra8_power_profile_init``
 * a second time re-initialises (the previous accumulators are
 * discarded). The function never touches hardware directly; all
 * side effects flow through the hooks in ``cfg``.
 *
 * Algorithm:
 *  1. Validate ``cfg`` is non-NULL.
 *  2. Copy ``cfg`` into the module-static state.
 *  3. Zero ``s_stats`` so subsequent ``get_stats`` returns a clean snapshot.
 *  4. Set ``s_initialized = true``.
 *
 * @param[in] cfg Pointer to a fully-populated config descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Success.
 * @retval k_ra8_err_null_ptr     ``cfg`` is ``nullptr``.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Caller is single-threaded (init context).
 * @post The module is initialized.
 * @post All per-region accumulators are zeroed.
 *
 * @note Not thread-safe. Run from system init.
 *
 * @see ra8_power_profile_mark_enter
 * @see ra8_power_profile_reset_stats
 *
 * @since 0.1.0
 */
ra8_err_t ra8_power_profile_init(const ra8_power_profile_config_t* cfg);

/**
 * @brief Mark entry into a tracked region.
 *
 * @details
 * Pulses the configured GPIO with ``entering=true`` and records the
 * current timestamp. If a previous enter for the same region had no
 * matching exit, the previous open timestamp is silently overwritten
 * (the entries counter still increments so the imbalance is
 * detectable in the snapshot).
 *
 * Algorithm:
 *  1. Validate module is initialized and ``region_id`` is in range.
 *  2. Increment ``regions[region_id].entries``.
 *  3. Stamp ``last_enter_us`` from the time hook (if any).
 *  4. Set ``is_open = true``.
 *  5. Fire the GPIO hook with ``entering=true`` (if any).
 *
 * @param[in] region_id Region identifier in ``[0,
 * k_ra8_power_profile_max_regions)``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                       Success.
 * @retval k_ra8_err_not_initialized      ``ra8_power_profile_init`` was not
 * called.
 * @retval k_ra8_err_range_check_failed   ``region_id`` is out of range.
 *
 * @pre Module is initialized.
 * @pre ``region_id < k_ra8_power_profile_max_regions``.
 * @post ``regions[region_id].entries`` increased by 1.
 * @post ``regions[region_id].is_open == true``.
 *
 * @note Not thread-safe -- callers must serialise enter/exit per region.
 *
 * @see ra8_power_profile_mark_exit
 *
 * @since 0.1.0
 */
ra8_err_t ra8_power_profile_mark_enter(ra8_power_profile_region_id_t region_id);

/**
 * @brief Mark exit from a tracked region.
 *
 * @details
 * Pulses the configured GPIO with ``entering=false``, computes the
 * elapsed time since the matching ``mark_enter`` (using the time
 * hook), and folds the delta into ``total_time_us``. If no enter is
 * outstanding for ``region_id`` the function still increments
 * ``exits`` and returns ``k_ra8_err_invalid_state`` so the caller can
 * surface the imbalance.
 *
 * Algorithm:
 *  1. Validate module is initialized and ``region_id`` is in range.
 *  2. Increment ``regions[region_id].exits``.
 *  3. If region was open, accumulate ``now - last_enter_us`` and
 *     clear ``is_open``.
 *  4. Fire the GPIO hook with ``entering=false`` (if any).
 *
 * @param[in] region_id Region identifier in ``[0,
 * k_ra8_power_profile_max_regions)``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                       Success: matching enter was open.
 * @retval k_ra8_err_invalid_state        No matching open enter for this region.
 * @retval k_ra8_err_not_initialized      ``ra8_power_profile_init`` was not
 * called.
 * @retval k_ra8_err_range_check_failed   ``region_id`` is out of range.
 *
 * @pre Module is initialized.
 * @pre ``region_id < k_ra8_power_profile_max_regions``.
 * @post ``regions[region_id].exits`` increased by 1.
 * @post ``regions[region_id].is_open == false``.
 *
 * @note Not thread-safe -- callers must serialise enter/exit per region.
 *
 * @see ra8_power_profile_mark_enter
 *
 * @since 0.1.0
 */
ra8_err_t ra8_power_profile_mark_exit(ra8_power_profile_region_id_t region_id);

/**
 * @brief Copy the current statistics snapshot to the caller.
 *
 * @details
 * Performs a structure copy of the internal accumulator array. The
 * snapshot reflects all enter/exit calls observed so far, including
 * still-open regions (whose ``total_time_us`` has not yet been
 * updated for the current open span).
 *
 * @param[out] out_stats Caller-supplied buffer to receive the snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Success.
 * @retval k_ra8_err_null_ptr        ``out_stats`` is ``nullptr``.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre Module is initialized.
 * @pre ``out_stats`` is non-NULL.
 * @post ``*out_stats`` is a consistent snapshot of internal state.
 *
 * @note Safe to call between mark_enter / mark_exit pairs.
 *
 * @see ra8_power_profile_reset_stats
 *
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
ra8_err_t ra8_power_profile_get_stats(ra8_power_profile_stats_t* out_stats);

/**
 * @brief Zero every accumulator (preserves the configured hooks).
 *
 * @details
 * Useful between profiling phases when the application wants a fresh
 * window of measurements without paying the cost of a full re-init.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Success.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre Module is initialized.
 * @post Every region's ``entries``/``exits``/``total_time_us`` is 0.
 * @post Every region's ``is_open`` is ``false``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 */
ra8_err_t ra8_power_profile_reset_stats(void);

#ifdef __cplusplus
}
#endif
