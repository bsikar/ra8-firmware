/**
 * @file ra8_wdt_supervisor.h
 * @brief ThreadX-aware watchdog supervisor (per-thread check-in registry)
 * @ingroup grp_system
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * The hardware Watchdog Timer (``ra8_wdt``) demands a refresh inside the
 * configured window or it resets the chip. In a multi-threaded build
 * blindly refreshing from one place defeats the purpose -- if the
 * refreshing thread is alive but every other worker has wedged, the
 * WDT still gets kicked and the wedge goes undetected.
 *
 * This module implements the canonical "per-thread check-in" pattern:
 *
 *  1. Each long-running ThreadX thread registers itself with a
 *     ``deadline_ms`` budget that bounds how often it must check in.
 *  2. The thread calls ``ra8_wdt_supervisor_checkin`` at safe points in
 *     its loop (typically once per iteration).
 *  3. A dedicated supervisor thread wakes every ``refresh_period_ms``
 *     and only calls ``ra8_wdt_refresh_deferred`` if **every** registered
 *     thread has checked in within its deadline. If any thread is
 *     overdue the supervisor stops kicking, the WDT eventually
 *     underflows, and the chip reboots into the failure handler.
 *
 * The registry is statically allocated and protected by a TX_MUTEX,
 * so registration must happen during boot before secondary threads
 * start running.
 *
 * @par State Machine:
 * @dot
 * digraph ra8_wdt_supervisor_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *   __end [shape=doublecircle, width=0.20, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Uninit [label="Uninit"];
 *   Initialized [label="Initialized"];
 *   Running [label="Running"];
 *   Stalled [label="Stalled"];
 *
 *   __start -> Uninit;
 *   Uninit -> Initialized [label="ra8_wdt_supervisor_init"];
 *   Initialized -> Initialized [label="register_thread / checkin"];
 *   Initialized -> Running [label="ra8_wdt_supervisor_start"];
 *   Running -> Running [label="tick (all threads alive ->\\nrefresh WDT)"];
 *   Running -> Stalled [label="any thread overdue (no\\nrefresh issued)"];
 *   Stalled -> __end [label="WDT underflow / chip reset"];
 * }
 * @enddot
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
 * @enum ra8_wdt_sup_limits_t
 * @brief Compile-time limits on the supervisor registry.
 *
 * @details
 * The maximum number of registered worker threads is capped to keep the
 * registry walk bounded (NASA Rule 2). All slots are statically
 * allocated -- no malloc per CLAUDE.md.
 */
typedef enum : uint8_t {
  k_ra8_wdt_sup_max_threads = 8U,  /**< Max simultaneously registered threads.  */
  k_ra8_wdt_sup_name_max    = 16U, /**< Max bytes (incl. NUL) in a thread name. */
} ra8_wdt_sup_limits_t;

/**
 * @enum ra8_wdt_sup_handle_t
 * @brief Opaque handle returned by ``ra8_wdt_supervisor_register_thread``.
 *
 * @details
 * Handles are small integers in [0, k_ra8_wdt_sup_max_threads). The
 * sentinel ``k_ra8_wdt_sup_handle_invalid`` is returned in ``out_handle``
 * if registration fails.
 */
typedef enum : uint8_t {
  k_ra8_wdt_sup_handle_invalid = 0xFFU, /**< Invalid / unregistered handle. */
} ra8_wdt_sup_handle_t;

/**
 * @struct ra8_wdt_sup_cfg_t
 * @brief One-shot supervisor configuration block.
 *
 * @details
 * The supervisor uses a caller-supplied byte pool / stack region so the
 * application owns memory placement (TCM, SRAM, ECC SRAM, etc.). The
 * stack must be sized large enough for the supervisor's loop -- 1 KiB
 * is comfortable for the current implementation.
 *
 * @invariant ``stack_size_bytes`` >= 512.
 * @invariant ``refresh_period_ms`` > 0.
 * @invariant ``priority`` is a legal ThreadX priority (0..31).
 *
 * @code
 * static uint8_t s_sup_stack[1024];
 * const ra8_wdt_sup_cfg_t cfg = {
 *   .stack          = s_sup_stack,
 *   .stack_size_bytes = sizeof s_sup_stack,
 *   .priority       = 4,
 *   .refresh_period_ms = 50,
 * };
 * (void)ra8_wdt_supervisor_init(&cfg);
 * @endcode
 */
typedef struct {
  void*    stack;             /**< Caller-owned byte region for the supervisor's stack. */
  uint32_t stack_size_bytes;  /**< Size of ``stack`` in bytes (>= 512).                 */
  uint32_t priority;          /**< ThreadX priority for the supervisor thread.          */
  uint32_t refresh_period_ms; /**< Tick period of the supervisor's loop.                */
} ra8_wdt_sup_cfg_t;

/**
 * @typedef ra8_wdt_sup_now_fn_t
 * @brief Hook used by tests to inject monotonic-time readings.
 *
 * @details
 * Production builds default to a hook that wraps ``tx_time_get`` and
 * scales it by the kernel tick. Unit tests override this with a
 * deterministic counter so deadline arithmetic is reproducible. NASA
 * Rule 9 deviation: function pointer used for Dependency Inversion.
 *
 * @return Monotonic time in milliseconds. Wraps mod 2^32.
 */
typedef uint32_t (*ra8_wdt_sup_now_fn_t)(void);

/**
 * @typedef ra8_wdt_sup_refresh_fn_t
 * @brief Hook used to call into ``ra8_wdt_refresh_deferred``.
 *
 * @details
 * Default points at ``ra8_wdt_refresh_deferred``. Tests override this so
 * the test body can count refresh calls without mmapping fake WDT
 * registers.
 */
typedef void (*ra8_wdt_sup_refresh_fn_t)(void);

/**
 * @brief Initialise the supervisor registry and runtime hooks.
 *
 * @details
 * Validates ``cfg``, zeroes the static registry, creates the registry
 * mutex, and stores ``cfg`` for later use by ``..._start``. Does not
 * spawn the supervisor thread yet -- callers register their workers
 * first, then call ``ra8_wdt_supervisor_start``.
 *
 * Algorithm:
 * 1. Reject null ``cfg`` and out-of-range fields.
 * 2. Clear every registry slot to ``free``.
 * 3. Create the TX_MUTEX guarding the registry.
 * 4. Stash ``cfg`` in module state.
 *
 * @param[in] cfg Pointer to a populated configuration block.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Supervisor initialized, ready for register / start.
 * @retval k_ra8_err_null_ptr ``cfg`` was null or ``cfg->stack`` was null.
 * @retval k_ra8_err_invalid_arg Stack too small / period zero / bad priority.
 * @retval k_ra8_err_busy ``ra8_wdt_supervisor_init`` was already called.
 *
 * @pre ``cfg`` is non-null and points to a populated block.
 * @pre Caller is in single-threaded init context.
 *
 * @post Registry is empty (every slot ``free``).
 * @post Internal mutex is created and unlocked.
 *
 * @note Not thread-safe; call once during boot.
 *
 * @par Example:
 * @code
 * static uint8_t sup_stack[1024];
 * const ra8_wdt_sup_cfg_t cfg = {
 *   .stack = sup_stack, .stack_size_bytes = sizeof sup_stack,
 *   .priority = 4, .refresh_period_ms = 50,
 * };
 * (void)ra8_wdt_supervisor_init(&cfg);
 * @endcode
 *
 * @see ra8_wdt_supervisor_register_thread
 * @see ra8_wdt_supervisor_start
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_init(const ra8_wdt_sup_cfg_t* cfg);

/**
 * @brief Reset the supervisor to its uninitialized state (test helper).
 *
 * @details
 * Tears down the registry, deletes the mutex, and clears module state.
 * Intended for unit tests that need a fresh slate between cases. Safe
 * to call when never initialized.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre None.
 * @post Module state is identical to its pre-init posture.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_deinit(void);

/**
 * @brief Register a worker thread with the supervisor.
 *
 * @details
 * Allocates one of the ``k_ra8_wdt_sup_max_threads`` static slots, copies
 * up to ``k_ra8_wdt_sup_name_max - 1`` bytes of ``name`` for diagnostics,
 * stamps the deadline budget, and primes ``last_checkin_ms`` to "now"
 * so the first tick does not count the thread as overdue before it has
 * had a chance to run.
 *
 * @param[in]  name        Short diagnostic name (must be NUL-terminated).
 * @param[in]  deadline_ms Maximum allowed gap between consecutive
 *                         check-ins, in milliseconds. Must be > 0.
 * @param[out] out_handle  Receives the registered handle on success.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Slot allocated, ``*out_handle`` populated.
 * @retval k_ra8_err_null_ptr ``name`` or ``out_handle`` was null.
 * @retval k_ra8_err_invalid_arg ``deadline_ms`` was zero.
 * @retval k_ra8_err_no_mem All slots are taken.
 * @retval k_ra8_err_not_initialized ``ra8_wdt_supervisor_init`` not called.
 *
 * @pre ``name`` is NUL-terminated and non-null.
 * @pre ``deadline_ms`` > 0.
 *
 * @post On success, registry holds one more populated slot.
 * @post On failure, ``*out_handle`` is set to ``k_ra8_wdt_sup_handle_invalid``.
 *
 * @note Thread-safe via TX_MUTEX.
 *
 * @see ra8_wdt_supervisor_checkin
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_wdt_supervisor_register_thread(const char* name, uint32_t deadline_ms, uint8_t* out_handle);

/**
 * @brief Record a thread check-in, resetting its deadline window.
 *
 * @details
 * Called by registered worker threads at known points in their loops
 * (typically once per iteration, before any blocking call). Updates
 * the slot's ``last_checkin_ms`` to the current monotonic time so the
 * supervisor sees the thread as alive on its next tick.
 *
 * @param[in] handle Handle returned by ``ra8_wdt_supervisor_register_thread``.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Check-in recorded.
 * @retval k_ra8_err_invalid_arg ``handle`` is out of range.
 * @retval k_ra8_err_not_found ``handle`` refers to a free slot.
 * @retval k_ra8_err_not_initialized ``ra8_wdt_supervisor_init`` not called.
 *
 * @pre ``handle`` < ``k_ra8_wdt_sup_max_threads``.
 * @pre Slot was previously registered.
 *
 * @post Slot's ``last_checkin_ms`` equals current monotonic time.
 *
 * @note Thread-safe via TX_MUTEX.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_checkin(uint8_t handle);

/**
 * @brief Spawn the supervisor thread.
 *
 * @details
 * Creates the supervisor's TX_THREAD with the stack region from
 * ``cfg``, then returns. The supervisor's loop:
 *
 *  1. ``tx_thread_sleep(refresh_period_ms)`` worth of ticks.
 *  2. Snapshot every populated slot under the mutex.
 *  3. For each slot, evaluate ``now - last_checkin_ms <= deadline_ms``.
 *  4. If every slot passes, call the refresh hook -> ``ra8_wdt_refresh_deferred``.
 *     Otherwise skip the refresh and let the WDT do its job.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Supervisor thread created.
 * @retval k_ra8_err_not_initialized ``ra8_wdt_supervisor_init`` not called.
 * @retval k_ra8_err_busy Already started.
 * @retval k_ra8_err_rtos_thread_create Underlying ``tx_thread_create`` failed.
 *
 * @pre ``ra8_wdt_supervisor_init`` returned ``k_ra8_ok``.
 * @pre At least one worker has been registered.
 *
 * @post Supervisor thread is runnable.
 * @post First refresh occurs at ``t = refresh_period_ms``.
 *
 * @note Not thread-safe; call once after registration.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_start(void);

/**
 * @brief Run one supervisor tick synchronously (test / introspection hook).
 *
 * @details
 * Production builds run the same logic from the supervisor thread's
 * loop. Exposing it as a callable function lets unit tests drive the
 * supervisor without spinning a real ThreadX kernel and lets a host
 * application probe "would I refresh right now?" out-of-band.
 *
 * @param[out] out_did_refresh Optional. Receives ``true`` if every
 *                             registered thread was within deadline and
 *                             the refresh hook was therefore called.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Tick complete.
 * @retval k_ra8_err_not_initialized ``ra8_wdt_supervisor_init`` not called.
 *
 * @pre ``ra8_wdt_supervisor_init`` returned ``k_ra8_ok``.
 * @post ``*out_did_refresh`` reflects whether the refresh hook ran.
 * @post Module state is otherwise unchanged.
 *
 * @note Thread-safe via TX_MUTEX.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_tick(bool* out_did_refresh);

/**
 * @brief Override the monotonic-time hook (test injection point).
 *
 * @details
 * Pass ``nullptr`` to restore the default ThreadX-tick reader. NASA
 * Rule 9 deviation: function pointer used for Dependency Inversion.
 *
 * @param[in] now Replacement hook or ``nullptr``.
 *
 * @return Always ``k_ra8_ok``.
 *
 * @pre Caller is in single-writer init context.
 * @post Subsequent supervisor logic uses ``now``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_set_now_hook(ra8_wdt_sup_now_fn_t now);

/**
 * @brief Override the WDT-refresh hook (test injection point).
 *
 * @details
 * Pass ``nullptr`` to restore the default that calls
 * ``ra8_wdt_refresh_deferred``. NASA Rule 9 deviation: function pointer
 * used for Dependency Inversion.
 *
 * @param[in] refresh Replacement hook or ``nullptr``.
 *
 * @return Always ``k_ra8_ok``.
 *
 * @pre Caller is in single-writer init context.
 * @post Subsequent supervisor ticks call ``refresh`` in place of the WDT.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_supervisor_set_refresh_hook(ra8_wdt_sup_refresh_fn_t refresh);

/**
 * @brief Read the number of currently-registered worker threads.
 *
 * @details
 * Diagnostic accessor for tests and dump helpers.
 *
 * @return Count in [0, k_ra8_wdt_sup_max_threads].
 *
 * @pre None.
 * @post No state is mutated.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
uint8_t ra8_wdt_supervisor_thread_count(void);

#ifdef __cplusplus
}
#endif
