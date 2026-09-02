/**
 * @file test_ra8_wdt_sup_tx_shim_cov.c
 * @brief Coverage-lift tests for libs/ra8_wdt_supervisor/src/ra8_wdt_sup_tx_shim_internal.h
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives the two inline stubs in ra8_wdt_sup_tx_shim_internal.h that the existing
 * supervisor test executables cannot reach:
 *
 *  - tx_thread_sleep (source lines 288-291): the supervisor thread loop
 *    calls this inside a spawned TX_THREAD that host tests never start
 *    (tests drive the supervisor synchronously via ra8_wdt_supervisor_tick
 *    instead). Covered here by calling the shim directly with both a
 *    zero and a non-zero tick count.
 *
 *  - tx_time_get (source lines 307-309): called only from the private
 *    internal_default_now helper in ra8_wdt_supervisor.c. That helper is
 *    exercised only when no custom now hook is installed; the existing
 *    test_ra8_wdt_supervisor_cov.c does exercise it, but the .gcda for
 *    that binary is merged only against the supervisor .c TU, not against
 *    the header TU used here. Covered here by calling tx_time_get directly
 *    from this TU so gcovr attributes the execution to the header lines.
 *
 * All remaining shim functions (tx_mutex_create, tx_mutex_get,
 * tx_mutex_put, tx_mutex_delete, tx_thread_create, tx_thread_terminate,
 * tx_thread_delete) are reachable through the supervisor public API
 * (init / deinit / register_thread / tick) and are already covered by the
 * sibling test executables.
 *
 * No hardware registers are touched; no ra8_fake_mmap window is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_wdt_sup_tx_shim_internal.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * tx_thread_sleep
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tx_thread_sleep_zero_ticks
 *
 * @brief tx_thread_sleep with a zero tick count returns TX_SUCCESS.
 *
 * @details
 * The host stub ignores its argument and returns TX_SUCCESS (0) without
 * blocking. Passing 0 is the degenerate lower bound; the return value
 * must still be TX_SUCCESS.
 *
 * This call covers source lines 288, 290, and 291 of ra8_wdt_sup_tx_shim_internal.h.
 *
 * @par MC/DC:
 * tx_thread_sleep contains no compound boolean decisions. A single call
 * satisfies line coverage; a second call with a different argument (see
 * test_tx_thread_sleep_nonzero_ticks) confirms the argument is ignored.
 *
 * @pre None.
 * @post No process state is mutated; the host binary did not block.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_tx_thread_sleep_zero_ticks(void)
{
  TEST_BEGIN("tx_thread_sleep(0) returns TX_SUCCESS");
  const UINT rc = tx_thread_sleep(0UL);
  TEST_ASSERT_EQ(TX_SUCCESS, rc);
  TEST_END("tx_thread_sleep(0) returns TX_SUCCESS");
}

/**
 * @test test_tx_thread_sleep_nonzero_ticks
 *
 * @brief tx_thread_sleep with a non-zero tick count returns TX_SUCCESS.
 *
 * @details
 * Confirms that the stub does not inspect the tick argument and does not
 * call nanosleep, usleep, or any other blocking primitive. The return
 * value is TX_SUCCESS regardless of the argument value.
 *
 * @par MC/DC:
 * tx_thread_sleep contains no compound boolean decisions. Pairing with
 * test_tx_thread_sleep_zero_ticks exhausts all observable stub behaviour.
 *
 * @pre None.
 * @post No process state is mutated; the host binary did not block.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_tx_thread_sleep_nonzero_ticks(void)
{
  TEST_BEGIN("tx_thread_sleep(100) returns TX_SUCCESS");
  const UINT rc = tx_thread_sleep(100UL);
  TEST_ASSERT_EQ(TX_SUCCESS, rc);
  TEST_END("tx_thread_sleep(100) returns TX_SUCCESS");
}

/* ---------------------------------------------------------------------------
 * tx_time_get
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tx_time_get_returns_zero
 *
 * @brief tx_time_get returns 0 on the host stub.
 *
 * @details
 * The host shim has no kernel tick counter and unconditionally returns
 * 0UL. The supervisor's internal_default_now multiplies this value by the
 * 1 kHz tick constant (k_ra8_wdt_sup_default_tick_ms = 1), so all
 * deadline arithmetic sees t = 0 ms -- meaning registered threads are
 * never considered overdue under the default hook.
 *
 * This call covers source lines 307 and 309 of ra8_wdt_sup_tx_shim_internal.h.
 *
 * @par MC/DC:
 * tx_time_get contains no compound boolean decisions. A single call
 * satisfies line coverage.
 *
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_tx_time_get_returns_zero(void)
{
  TEST_BEGIN("tx_time_get returns 0UL on host stub");
  const ULONG t = tx_time_get();
  TEST_ASSERT_EQ(0UL, t);
  TEST_END("tx_time_get returns 0UL on host stub");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

int main(void)
{
  test_tx_thread_sleep_zero_ticks();
  test_tx_thread_sleep_nonzero_ticks();
  test_tx_time_get_returns_zero();
  return 0;
}
