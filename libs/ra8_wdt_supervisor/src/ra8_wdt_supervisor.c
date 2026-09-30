/**
 * @file ra8_wdt_supervisor.c
 * @brief ThreadX-aware watchdog supervisor implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Static-allocation registry with a ::fw_os_mutex_t guard and a single
 * ::fw_os_thread_t
 * that wakes every ``refresh_period_ms``. See ``ra8_wdt_supervisor.h`` for
 * the design rationale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_wdt_supervisor.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_wdt.h"
#include "fw_os.h"

/**
 * @enum ra8_wdt_sup_internal_t
 * @brief Internal numeric constants used by the implementation.
 */
typedef enum : uint32_t {
  k_ra8_wdt_sup_min_stack       = 512U,        /**< Minimum acceptable stack size.  */
  k_ra8_wdt_sup_max_priority    = 31U,         /**< Highest legal ThreadX priority. */
  k_ra8_wdt_sup_default_tick_ms = 1U,          /**< Default tick: 1 kHz kernel.     */
  k_ra8_wdt_sup_slot_free       = 0U,          /**< Slot tag: empty.                */
  k_ra8_wdt_sup_slot_used       = 1U,          /**< Slot tag: registered.           */
  k_ra8_wdt_sup_mutex_id        = 0x57445353U, /**< 'WDSS' marker for the mutex.    */
  k_ra8_wdt_sup_thread_id       = 0x57445354U, /**< 'WDST' marker for the thread.   */
} ra8_wdt_sup_internal_t;

/**
 * @struct ra8_wdt_sup_slot_t
 * @brief One row of the supervisor registry.
 */
typedef struct {
  uint8_t  state;                        /**< ``slot_free`` or ``slot_used``.   */
  char     name[k_ra8_wdt_sup_name_max]; /**< Diagnostic name (NUL-terminated). */
  uint32_t deadline_ms;                  /**< Max gap between check-ins.        */
  uint32_t last_checkin_ms;              /**< Monotonic time of last check-in.  */
} ra8_wdt_sup_slot_t;

/**
 * @enum ra8_wdt_sup_band_bound_t
 * @brief Quartile bounds that bucket a 0..31 priority into an ::fw_os band.
 *
 * @details
 * The legal numeric range is 0..``k_ra8_wdt_sup_max_priority`` (31), so the
 * quartiles fall at 8, 16 and 24. Named rather than literal per the
 * repository's no-magic-number rule.
 */
typedef enum : uint32_t {
  k_ra8_wdt_sup_band_high_max   = 8U,  /**< Below this is k_fw_os_priority_high.   */
  k_ra8_wdt_sup_band_normal_max = 16U, /**< Below this is k_fw_os_priority_normal. */
  k_ra8_wdt_sup_band_low_max    = 24U, /**< Below this is k_fw_os_priority_low.    */
} ra8_wdt_sup_band_bound_t;

/**
 * @struct ra8_wdt_sup_state_t
 * @brief Module state -- entirely static.
 */
typedef struct {
  bool                     initialized;                      /**< True after init succeeded. */
  bool                     started;                          /**< True once the thread runs. */
  ra8_wdt_sup_cfg_t        cfg;                              /**< Cached configuration.      */
  ra8_wdt_sup_slot_t       slots[k_ra8_wdt_sup_max_threads]; /**< Registry.                  */
  fw_os_mutex_t            mutex;                            /**< Guards ``slots``.          */
  fw_os_thread_t           thread;                           /**< Supervisor thread block.   */
  ra8_wdt_sup_now_fn_t     now;                              /**< Monotonic-time hook.       */
  ra8_wdt_sup_refresh_fn_t refresh;                          /**< WDT-refresh hook.          */
} ra8_wdt_sup_state_t;

/**
 * @var s_state
 * @brief Singleton module state.
 *
 * @note Static; do not access outside this TU.
 */
static ra8_wdt_sup_state_t s_state;

/**
 * @brief Map the caller's numeric priority onto a portable ::fw_os band.
 *
 * @details
 * ``ra8_wdt_sup_cfg_t::priority`` predates the ::fw_os seam: it is a
 * ThreadX priority number, 0 (most urgent) to 31, and the public contract
 * still validates that range. ::fw_os takes one of four bands instead, so
 * the number is bucketed by quartile. The mapping is deliberately lossy
 * and deliberately monotonic: a smaller number never yields a lower band,
 * so relative urgency between two configured supervisors survives.
 *
 * Retiring the numeric field in favour of ::fw_os_priority_t is a separate
 * change: it would break every caller that passes a ThreadX number today,
 * including ``wdt_supervisor_demo`` and the extended test's
 * out-of-range rejection case.
 *
 * @param[in] priority Numeric priority from the cached configuration.
 *
 * @return Portable ::fw_os band for @p priority.
 *
 * @pre @p priority has passed ::internal_validate_cfg.
 * @post No state is mutated.
 * @note Thread-safe; pure function.
 * @since 0.1.0
 */
RA8_INTERNAL static fw_os_priority_t internal_band_for(uint32_t priority)
{
  if (priority < (uint32_t)k_ra8_wdt_sup_band_high_max) {
    return k_fw_os_priority_high;
  }
  if (priority < (uint32_t)k_ra8_wdt_sup_band_normal_max) {
    return k_fw_os_priority_normal;
  }
  if (priority < (uint32_t)k_ra8_wdt_sup_band_low_max) {
    return k_fw_os_priority_low;
  }
  return k_fw_os_priority_idle;
}

/**
 * @brief Default monotonic-time hook -- reads the ::fw_os uptime.
 *
 * @details
 * ::fw_os_uptime_ms already reports milliseconds whatever rate the
 * underlying kernel ticks at, so nothing is scaled here. Override via
 * ``ra8_wdt_supervisor_set_now_hook`` to inject a test clock.
 *
 * @return Current monotonic time in milliseconds.
 *
 * @pre None.
 * @post No state is mutated.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_default_now(void)
{
  return fw_os_uptime_ms();
}

/**
 * @brief Default WDT-refresh hook.
 *
 * @details
 * Wraps ``ra8_wdt_refresh_deferred`` so the test build can swap it out
 * without pulling the WDT register layout into the unit test image.
 *
 * @pre WDT block has been armed.
 * @post WDTRR has been written with the unlock sequence.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_default_refresh(void)
{
  ra8_wdt_refresh_deferred();
}

/**
 * @brief Validate the public configuration block.
 *
 * @param[in] cfg Caller-supplied configuration.
 *
 * @return ``k_ra8_ok`` if the block is acceptable, else a specific error.
 * @retval k_ra8_err_null_ptr     ``cfg`` or ``cfg->stack`` was null.
 * @retval k_ra8_err_invalid_arg  Stack too small / period zero / priority bad.
 *
 * @pre None.
 * @post No state is mutated.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_cfg(const ra8_wdt_sup_cfg_t* cfg)
{
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->stack == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->stack_size_bytes < (uint32_t)k_ra8_wdt_sup_min_stack) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->refresh_period_ms == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->priority > (uint32_t)k_ra8_wdt_sup_max_priority) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Compute whether ``now - last_checkin`` exceeds ``deadline``.
 *
 * @details
 * Uses unsigned subtraction so 32-bit wrap-around is handled implicitly
 * as long as the gap is < 2^31 ms (~24.8 days), which is always the
 * case for the supervisor's ms-scale ticks.
 *
 * @param[in] now          Current monotonic time (ms).
 * @param[in] last_checkin Time of slot's last check-in (ms).
 * @param[in] deadline     Slot's deadline budget (ms).
 *
 * @return ``true`` if the slot is overdue, ``false`` if alive.
 *
 * @pre None.
 * @post No state is mutated.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_overdue(uint32_t now, uint32_t last_checkin, uint32_t deadline)
{
  const uint32_t gap = now - last_checkin;
  return gap > deadline;
}

/* GCOVR_EXCL_START -- the host binding does not run the entry callback itself */
/**
 * @brief The supervisor thread's entry point.
 *
 * @details
 * Loops forever, sleeping ``refresh_period_ms`` between ticks. Each
 * iteration calls ``ra8_wdt_supervisor_tick`` to evaluate the registry
 * and conditionally refresh the WDT.
 *
 * @param[in] arg Unused (::fw_os hands the thread a single void*).
 *
 * @pre ``s_state.initialized`` is true.
 * @post Loops forever (NASA Rule 2: bounded body, unbounded outer).
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_thread_entry(void* arg)
{
  (void)arg;
  /* NASA Rule 2: outer loop is the canonical "main control loop"
   * exception. Body is finite. */
  while (true) {
    bool refreshed = false;
    (void)ra8_wdt_supervisor_tick(&refreshed);
    fw_os_thread_sleep_ms(s_state.cfg.refresh_period_ms);
  }
}
/* GCOVR_EXCL_STOP */

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_wdt_supervisor_init(const ra8_wdt_sup_cfg_t* cfg)
{
  const ra8_err_t cfg_err = internal_validate_cfg(cfg);
  if (cfg_err != k_ra8_ok) {
    return cfg_err;
  }
  if (s_state.initialized) {
    return k_ra8_err_busy;
  }

  (void)memset(&s_state.slots[0], 0, sizeof s_state.slots);
  s_state.cfg     = *cfg;
  s_state.now     = internal_default_now;
  s_state.refresh = internal_default_refresh;
  s_state.started = false;

  const ra8_err_t mx = fw_os_mutex_init(&s_state.mutex, false);
  if (mx != k_ra8_ok) {
    return k_ra8_err_rtos_mutex;
  }

  s_state.initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_wdt_supervisor_deinit(void)
{
  if (s_state.initialized) {
    if (s_state.started) {
      (void)fw_os_thread_delete(&s_state.thread);
    }
    (void)fw_os_mutex_deinit(&s_state.mutex);
  }
  (void)memset(&s_state, 0, sizeof s_state);
  return k_ra8_ok;
}

/**
 * @brief Initialise an unused slot with the caller's parameters.
 *
 * @details
 * Helper extracted from ::ra8_wdt_supervisor_register_thread so the
 * outer function fits the project size threshold (NASA Power-of-10
 * Rule 4 / clang-tidy ``readability-function-size``). The caller MUST
 * already hold ``s_state.mutex``.
 *
 * @param[in] idx         Index of a free slot in ``s_state.slots``.
 * @param[in] name        Caller-supplied thread name (NUL-terminated).
 * @param[in] deadline_ms Per-thread deadline in milliseconds.
 *
 * @pre Caller holds ``s_state.mutex``.
 * @pre ``idx`` < ::k_ra8_wdt_sup_max_threads.
 * @post ``s_state.slots[idx]`` is in the ``slot_used`` state.
 *
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_slot(uint8_t idx, const char* name, uint32_t deadline_ms)
{
  s_state.slots[idx].state           = (uint8_t)k_ra8_wdt_sup_slot_used;
  s_state.slots[idx].deadline_ms     = deadline_ms;
  s_state.slots[idx].last_checkin_ms = s_state.now();
  /* Bounded copy with explicit NUL termination. */
  (void)memset(s_state.slots[idx].name, 0, sizeof s_state.slots[idx].name);
  const size_t cap = (size_t)k_ra8_wdt_sup_name_max - 1U;
  for (size_t k = 0U; k < cap; ++k) {
    const char ch = name[k];
    if (ch == '\0') {
      break;
    }
    s_state.slots[idx].name[k] = ch;
  }
}

ra8_err_t
ra8_wdt_supervisor_register_thread(const char* name, uint32_t deadline_ms, uint8_t* out_handle)
{
  if (out_handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_handle = (uint8_t)k_ra8_wdt_sup_handle_invalid;

  if (name == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (deadline_ms == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t mx = fw_os_mutex_lock(&s_state.mutex, K_FW_OS_WAIT_FOREVER);
  if (mx != k_ra8_ok) {
    return k_ra8_err_rtos_mutex;
  }

  ra8_err_t result = k_ra8_err_no_mem;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_wdt_sup_max_threads; ++i) {
    if (s_state.slots[i].state == (uint8_t)k_ra8_wdt_sup_slot_free) {
      internal_fill_slot(i, name, deadline_ms);
      *out_handle = i;
      result      = k_ra8_ok;
      break;
    }
  }

  (void)fw_os_mutex_unlock(&s_state.mutex);
  return result;
}

ra8_err_t ra8_wdt_supervisor_checkin(uint8_t handle)
{
  if (handle >= (uint8_t)k_ra8_wdt_sup_max_threads) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t mx = fw_os_mutex_lock(&s_state.mutex, K_FW_OS_WAIT_FOREVER);
  if (mx != k_ra8_ok) {
    return k_ra8_err_rtos_mutex;
  }

  ra8_err_t result = k_ra8_err_not_found;
  if (s_state.slots[handle].state == (uint8_t)k_ra8_wdt_sup_slot_used) {
    s_state.slots[handle].last_checkin_ms = s_state.now();
    result                                = k_ra8_ok;
  }

  (void)fw_os_mutex_unlock(&s_state.mutex);
  return result;
}

ra8_err_t ra8_wdt_supervisor_start(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if (s_state.started) {
    return k_ra8_err_busy;
  }

  const fw_os_thread_cfg_t thread_cfg = {
    .name        = "ra8_wdt_sup",
    .entry       = internal_thread_entry,
    .arg         = nullptr,
    .stack       = s_state.cfg.stack,
    .stack_bytes = (size_t)s_state.cfg.stack_size_bytes,
    .priority    = internal_band_for(s_state.cfg.priority),
  };
  const ra8_err_t started = fw_os_thread_create(&s_state.thread, &thread_cfg);
  if (started != k_ra8_ok) {
    return k_ra8_err_rtos_thread_create;
  }

  s_state.started = true;
  return k_ra8_ok;
}

ra8_err_t ra8_wdt_supervisor_tick(bool* out_did_refresh)
{
  if (!s_state.initialized) {
    if (out_did_refresh != nullptr) {
      *out_did_refresh = false;
    }
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t mx = fw_os_mutex_lock(&s_state.mutex, K_FW_OS_WAIT_FOREVER);
  if (mx != k_ra8_ok) {
    if (out_did_refresh != nullptr) {
      *out_did_refresh = false;
    }
    return k_ra8_err_rtos_mutex;
  }

  bool           all_alive   = true;
  bool           any_present = false;
  const uint32_t now_ms      = s_state.now();
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_wdt_sup_max_threads; ++i) {
    if (s_state.slots[i].state != (uint8_t)k_ra8_wdt_sup_slot_used) {
      continue;
    }
    any_present = true;
    if (internal_is_overdue(now_ms,
                            s_state.slots[i].last_checkin_ms,
                            s_state.slots[i].deadline_ms)) {
      all_alive = false;
      break;
    }
  }

  (void)fw_os_mutex_unlock(&s_state.mutex);

  /* Only refresh if at least one thread is registered AND every
   * registered thread is alive. With zero workers we deliberately do
   * NOT kick the dog -- the supervisor would mask a degenerate config. */
  const bool will_refresh = any_present && all_alive;
  // mcdc-deactivated: ra8_wdt_supervisor_tick refresh dispatch gate; s_state.refresh is registered at init and remains non-NULL for the lifetime of the supervisor (it is the WDT kick callback, mandatory per the public-API contract). will_refresh varies independently and is fully covered, but the second condition is invariant on every reachable post-init path.
  if (will_refresh && (s_state.refresh != nullptr)) {
    s_state.refresh();
  }
  if (out_did_refresh != nullptr) {
    *out_did_refresh = will_refresh;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_wdt_supervisor_set_now_hook(ra8_wdt_sup_now_fn_t now)
{
  s_state.now = (now != nullptr) ? now : internal_default_now;
  return k_ra8_ok;
}

ra8_err_t ra8_wdt_supervisor_set_refresh_hook(ra8_wdt_sup_refresh_fn_t refresh)
{
  s_state.refresh = (refresh != nullptr) ? refresh : internal_default_refresh;
  return k_ra8_ok;
}

uint8_t ra8_wdt_supervisor_thread_count(void)
{
  uint8_t count = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_wdt_sup_max_threads; ++i) {
    if (s_state.slots[i].state == (uint8_t)k_ra8_wdt_sup_slot_used) {
      ++count;
    }
  }
  return count;
}

#ifdef RA8_OFF_TARGET
#endif /* RA8_OFF_TARGET */
