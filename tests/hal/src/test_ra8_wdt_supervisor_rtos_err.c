/**
 * @file test_ra8_wdt_supervisor_rtos_err.c
 * @brief RTOS failure taxonomy tests for libs/ra8_wdt_supervisor
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Covers the five ThreadX failure branches in ``ra8_wdt_supervisor.c``
 * that no host input could reach before the shim gained a one-shot
 * forced-failure slot (issue #1231). Each case arms one ThreadX call to
 * fail, drives the public entry point that makes that call, and asserts
 * the published per-object code rather than the old catch-all
 * ``k_ra8_err_rtos_error``:
 *
 *  - ``ra8_wdt_supervisor_init``            -> ``tx_mutex_create``
 *  - ``ra8_wdt_supervisor_register_thread`` -> ``tx_mutex_get``
 *  - ``ra8_wdt_supervisor_checkin``         -> ``tx_mutex_get``
 *  - ``ra8_wdt_supervisor_start``           -> ``tx_thread_create``
 *  - ``ra8_wdt_supervisor_tick``            -> ``tx_mutex_get``
 *
 * The forced failure is one-shot, so each case also asserts that the
 * immediately following call succeeds; that is what keeps a forced
 * failure from leaking into the next case.
 *
 * No hardware registers are touched beyond the WDT refresh the tick path
 * already performs through the sibling test files' fake mmap window.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wdt_sup_tx_shim_internal.h"
#include "ra8_wdt_supervisor.h"
#include "unity_minimal.h"

/**
 * @enum t_sup_rtos_err_t
 * @brief Fixture sizes shared by every case in this TU.
 */
typedef enum : uint16_t {
  k_t_rtos_stack_bytes  = 1024U, /**< Supervisor task stack, bytes.   */
  k_t_rtos_period_ms    = 50U,   /**< Refresh period for the fixture. */
  k_t_rtos_deadline_ms  = 200U,  /**< Worker deadline for the fixture.*/
  k_t_rtos_priority     = 4U,    /**< Supervisor thread priority.     */
} t_sup_rtos_err_t;

/**
 * @var s_rtos_stack
 * @brief Stack region handed to the supervisor by every case.
 */
static uint8_t s_rtos_stack[(uint32_t)k_t_rtos_stack_bytes];

/**
 * @var s_rtos_now_ms
 * @brief Monotonic time returned by the injected now hook.
 */
static uint32_t s_rtos_now_ms = 0U;

/**
 * @brief Monotonic-time hook: returns the test-controlled clock.
 *
 * @return Current fake monotonic time in milliseconds.
 *
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static uint32_t hook_rtos_now(void)
{
  return s_rtos_now_ms;
}

/**
 * @brief Refresh hook: records nothing, keeps the WDT out of the tick.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void hook_rtos_refresh(void)
{
}

/**
 * @brief Bring the supervisor up with hooks installed and one worker.
 *
 * @param[out] out_handle Receives the registered worker handle.
 *
 * @return Nothing.
 *
 * @pre The supervisor is deinitialised or uninitialised.
 * @post The supervisor is initialised, hooked and holds one worker.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void bring_up(uint8_t* out_handle)
{
  (void)ra8_wdt_supervisor_deinit();
  s_rtos_now_ms = 0U;

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_rtos_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_rtos_stack),
    .priority          = (uint8_t)k_t_rtos_priority,
    .refresh_period_ms = (uint32_t)k_t_rtos_period_ms,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_now_hook(hook_rtos_now));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_refresh_hook(hook_rtos_refresh));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_wdt_supervisor_register_thread("rtos_w",
                                                    (uint32_t)k_t_rtos_deadline_ms,
                                                    out_handle));
}

/**
 * @brief init reports the mutex code when tx_mutex_create fails.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The supervisor is left initialised by the retry.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_init_mutex_create_failure(void)
{
  TEST_BEGIN("init maps tx_mutex_create failure to k_ra8_err_rtos_mutex");

  (void)ra8_wdt_supervisor_deinit();

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_rtos_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_rtos_stack),
    .priority          = (uint8_t)k_t_rtos_priority,
    .refresh_period_ms = (uint32_t)k_t_rtos_period_ms,
  };

  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_mutex_create);
  TEST_ASSERT_EQ(k_ra8_err_rtos_mutex, ra8_wdt_supervisor_init(&cfg));

  /* The forced failure is one-shot: the retry must come up clean, and the
   * failed attempt must not have latched s_state.initialized. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));

  TEST_END("init maps tx_mutex_create failure to k_ra8_err_rtos_mutex");
}

/**
 * @brief register_thread reports the mutex code when the lock fails.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The supervisor holds the worker registered by the retry.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_register_mutex_get_failure(void)
{
  TEST_BEGIN("register_thread maps tx_mutex_get failure to k_ra8_err_rtos_mutex");

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  bring_up(&h);

  uint8_t h2 = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_mutex_get);
  TEST_ASSERT_EQ(k_ra8_err_rtos_mutex,
                 ra8_wdt_supervisor_register_thread("rtos_w2",
                                                    (uint32_t)k_t_rtos_deadline_ms,
                                                    &h2));
  /* The out-parameter is cleared before the lock is taken, so a lock
   * failure must leave the caller with the invalid handle. */
  TEST_ASSERT_EQ((uint8_t)k_ra8_wdt_sup_handle_invalid, h2);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_wdt_supervisor_register_thread("rtos_w2",
                                                    (uint32_t)k_t_rtos_deadline_ms,
                                                    &h2));

  TEST_END("register_thread maps tx_mutex_get failure to k_ra8_err_rtos_mutex");
}

/**
 * @brief checkin reports the mutex code when the lock fails.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The worker's check-in timestamp is set by the retry.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_checkin_mutex_get_failure(void)
{
  TEST_BEGIN("checkin maps tx_mutex_get failure to k_ra8_err_rtos_mutex");

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  bring_up(&h);

  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_mutex_get);
  TEST_ASSERT_EQ(k_ra8_err_rtos_mutex, ra8_wdt_supervisor_checkin(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));

  TEST_END("checkin maps tx_mutex_get failure to k_ra8_err_rtos_mutex");
}

/**
 * @brief start reports the thread-create code when the spawn fails.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The supervisor is started by the retry.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_start_thread_create_failure(void)
{
  TEST_BEGIN("start maps tx_thread_create failure to k_ra8_err_rtos_thread_create");

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  bring_up(&h);

  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_thread_create);
  TEST_ASSERT_EQ(k_ra8_err_rtos_thread_create, ra8_wdt_supervisor_start());

  /* A failed spawn must not latch s_state.started, otherwise the retry
   * would answer k_ra8_err_busy instead of starting the supervisor. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_start());

  TEST_END("start maps tx_thread_create failure to k_ra8_err_rtos_thread_create");
}

/**
 * @brief tick reports the mutex code and clears the refresh flag.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The supervisor is left initialised with one checked-in worker.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_tick_mutex_get_failure(void)
{
  TEST_BEGIN("tick maps tx_mutex_get failure to k_ra8_err_rtos_mutex");

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  bring_up(&h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));

  bool did_refresh = true;
  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_mutex_get);
  TEST_ASSERT_EQ(k_ra8_err_rtos_mutex, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(0, did_refresh);

  /* Same call with nothing armed: the worker is inside its deadline, so
   * the tick refreshes and the failure really was the lock. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, did_refresh);

  TEST_END("tick maps tx_mutex_get failure to k_ra8_err_rtos_mutex");
}

/**
 * @brief A disarmed shim never injects a failure.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The supervisor is left initialised and started.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_disarmed_shim_is_transparent(void)
{
  TEST_BEGIN("disarmed shim leaves every ThreadX call succeeding");

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  ra8_wdt_supervisor_test_force_rtos_failure(k_ra8_wdt_sup_tx_call_none);
  bring_up(&h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_start());

  bool did_refresh = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, did_refresh);

  TEST_END("disarmed shim leaves every ThreadX call succeeding");
}

int main(void)
{
  test_init_mutex_create_failure();
  test_register_mutex_get_failure();
  test_checkin_mutex_get_failure();
  test_start_thread_create_failure();
  test_tick_mutex_get_failure();
  test_disarmed_shim_is_transparent();
  (void)ra8_wdt_supervisor_deinit();
  return 0;
}
