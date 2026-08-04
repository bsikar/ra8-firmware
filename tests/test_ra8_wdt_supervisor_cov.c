/**
 * @file test_ra8_wdt_supervisor_cov.c
 * @brief Coverage-lift tests for libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Exercises the two static helpers not covered by the sibling test files:
 *
 *  - ``internal_default_now`` (source lines 97, 99): invoked via
 *    ``s_state.now()`` when no custom hook has been installed.  Covered by
 *    initialising without calling ``ra8_wdt_supervisor_set_now_hook`` and
 *    then calling ``ra8_wdt_supervisor_register_thread``, which routes
 *    through ``internal_fill_slot`` -> ``s_state.now()``.  A subsequent
 *    ``ra8_wdt_supervisor_tick`` also calls ``s_state.now()`` via the same
 *    path to reinforce coverage.
 *
 *  - ``internal_default_refresh`` (source lines 117-120): invoked via
 *    ``s_state.refresh()`` when no custom refresh hook has been installed
 *    and the tick finds all registered threads alive.  Covered by
 *    initialising without calling ``ra8_wdt_supervisor_set_refresh_hook``,
 *    registering one alive worker, and calling ``ra8_wdt_supervisor_tick``.
 *    ``ra8_wdt_refresh_deferred`` writes to the WDT peripheral register at
 *    0x40202600, which falls inside the fake's anonymous-mmap
 *    peripheral window (0x40000000 + 8 MiB); the write succeeds on the
 *    Linux test runner.
 *
 * All remaining uncovered lines require a ThreadX stub to return a
 * non-zero status code.  The host shim (``ra8_wdt_sup_tx_shim_internal.h``) provides
 * ``static inline`` stubs that unconditionally return ``TX_SUCCESS``; no
 * host input can cause them to fail.  Those lines carry ``GCOVR_EXCL_LINE``
 * or ``GCOVR_EXCL_START/STOP`` annotations in the source file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_wdt_supervisor.h"
#include "unity_minimal.h"

/**
 * @enum t_sup_cov_t
 * @brief Supervisor task stack size.
 */
typedef enum : uint16_t {
  k_t_stack_bytes = 1024U, /**< Supervisor task stack, bytes. */
} t_sup_cov_t;

/* ---------------------------------------------------------------------------
 * Shared scaffolding
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_cov_stack
 * @brief Stack region shared by all tests in this TU.
 * @note Do not access directly outside the test body.
 * @since 0.1.0
 */
static uint8_t s_cov_stack[k_t_stack_bytes];

/**
 * @var s_cov_now_ms
 * @brief Fake monotonic time driven by ``hook_cov_now``.
 * @note Tests freeze it at 0 so deadline arithmetic is deterministic.
 * @since 0.1.0
 */
static uint32_t s_cov_now_ms;

/**
 * @var s_cov_refresh_calls
 * @brief Call counter incremented by ``hook_cov_noop_refresh``.
 * @since 0.1.0
 */
static uint32_t s_cov_refresh_calls;

/**
 * @brief Deterministic monotonic-time hook.
 *
 * @details
 * Returns the current value of ``s_cov_now_ms``.  Tests freeze
 * the clock at zero so that every registered thread is always within
 * its deadline when ``ra8_wdt_supervisor_tick`` evaluates the registry.
 *
 * @return ``s_cov_now_ms`` -- current fake time in milliseconds.
 *
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe; all tests are single-threaded.
 * @since 0.1.0
 */
static uint32_t hook_cov_now(void)
{
  return s_cov_now_ms;
}

/**
 * @brief No-operation refresh hook that counts calls.
 *
 * @details
 * Used in tests that exercise ``internal_default_now`` but do not want to
 * invoke the real ``ra8_wdt_refresh_deferred`` and its register writes.
 * Increments ``s_cov_refresh_calls`` so the test body can assert the
 * refresh path was taken.
 *
 * @pre None.
 * @post ``s_cov_refresh_calls`` is incremented by one.
 * @note Not thread-safe; all tests are single-threaded.
 * @since 0.1.0
 */
static void hook_cov_noop_refresh(void)
{
  ++s_cov_refresh_calls;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_default_now_hook_called_by_register
 *
 * @brief ``internal_default_now`` is exercised when no custom now hook is set.
 *
 * @details
 * After ``ra8_wdt_supervisor_init`` the module holds
 * ``s_state.now = internal_default_now``.  A call to
 * ``ra8_wdt_supervisor_register_thread`` routes through
 * ``internal_fill_slot``, which stamps ``last_checkin_ms`` by calling
 * ``s_state.now()``.  Because no custom hook was installed, the call
 * reaches ``internal_default_now``, covering source lines 97 and 99.
 *
 * A subsequent ``ra8_wdt_supervisor_tick`` also dispatches through
 * ``s_state.now()`` so both call-sites inside the default function are
 * confirmed live.
 *
 * ``internal_default_now`` calls the shim ``tx_time_get()``, which
 * returns 0.  With both ``last_checkin_ms`` and ``now_ms`` equal to 0
 * the single registered thread is always within its 200 ms deadline
 * (gap = 0), so ``will_refresh`` is true and the no-op refresh hook
 * is called once.
 *
 * @par MC/DC:
 * ``internal_default_now`` contains no compound boolean decisions.
 * Coverage is satisfied by a single invocation of the function.
 *
 * @pre None.
 * @post Module is left initialised; caller must deinit or each test
 *       calls ``ra8_wdt_supervisor_deinit`` at the top of its own body.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_default_now_hook_called_by_register(void)
{
  TEST_BEGIN("internal_default_now via register_thread (no custom now hook)");

  (void)ra8_wdt_supervisor_deinit();
  s_cov_refresh_calls = 0U;

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_cov_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_cov_stack),
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));

  /* Intentionally skip set_now_hook: s_state.now == internal_default_now. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_refresh_hook(hook_cov_noop_refresh));

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  /* register_thread -> internal_fill_slot -> s_state.now() = internal_default_now(). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("cov_t1", 200U, &h));
  TEST_ASSERT_EQ(0U, h);
  TEST_ASSERT_EQ(1U, ra8_wdt_supervisor_thread_count());

  /* tick also calls s_state.now() = internal_default_now().
   * tx_time_get() returns 0 -> now=0, last_checkin=0, gap=0 <= 200 -> alive. */
  bool did_refresh = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, did_refresh);
  TEST_ASSERT_EQ(1U, s_cov_refresh_calls);

  TEST_END("internal_default_now via register_thread (no custom now hook)");
}

/**
 * @test test_default_refresh_hook_called_by_tick
 *
 * @brief ``internal_default_refresh`` is exercised when no custom refresh hook is set.
 *
 * @details
 * After ``ra8_wdt_supervisor_init`` the module holds
 * ``s_state.refresh = internal_default_refresh``.  When
 * ``ra8_wdt_supervisor_tick`` evaluates the registry and finds all threads
 * alive, it calls ``s_state.refresh()``.  Because no custom refresh hook
 * was installed, the call reaches ``internal_default_refresh``, which
 * calls ``ra8_wdt_refresh_deferred``.  This covers source lines 117, 119,
 * and 120.
 *
 * A deterministic now hook is installed so time is frozen at 0.  Both
 * ``internal_fill_slot`` (called by ``register_thread``) and the tick
 * evaluation see ``now_ms = 0``.  With ``last_checkin_ms = 0`` and
 * ``deadline_ms = 200``, ``gap = 0`` satisfies ``gap <= deadline``, so
 * ``all_alive`` remains true and ``will_refresh`` is set.
 *
 * ``ra8_wdt_refresh_deferred`` writes to the WDT peripheral WDTRR register
 * at 0x40202600.  The ``ra8_fake_mmap`` constructor maps the full peripheral
 * bus (0x40000000, 8 MiB) as anonymous RAM before ``main`` executes, so
 * the write is safe on the Linux test runner.
 *
 * @par MC/DC:
 * ``internal_default_refresh`` contains no compound boolean decisions.
 * Coverage is satisfied by a single invocation of the function.
 *
 * @pre None.
 * @post Module is left initialised; the WDT register backing store
 *       (fake RAM) may hold 0xFF from the refresh sequence.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_default_refresh_hook_called_by_tick(void)
{
  TEST_BEGIN("internal_default_refresh via tick (no custom refresh hook)");

  (void)ra8_wdt_supervisor_deinit();
  s_cov_now_ms = 0U;

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_cov_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_cov_stack),
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_now_hook(hook_cov_now));
  /* Intentionally skip set_refresh_hook: s_state.refresh == internal_default_refresh. */

  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("cov_t2", 200U, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));

  /* tick: any_present=T, all_alive=T (gap=0 <= 200) -> will_refresh=T.
   * s_state.refresh() == internal_default_refresh() -> ra8_wdt_refresh_deferred(). */
  bool did_refresh = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(1, did_refresh);

  TEST_END("internal_default_refresh via tick (no custom refresh hook)");
}

int32_t main(void)
{
  test_default_now_hook_called_by_register();
  test_default_refresh_hook_called_by_tick();
  (void)fprintf(stderr, "[OK  ] test_ra8_wdt_supervisor_cov.c\n");
  return 0;
}
