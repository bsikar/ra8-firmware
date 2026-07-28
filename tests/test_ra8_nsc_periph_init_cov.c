/**
 * @file test_ra8_nsc_periph_init_cov.c
 * @brief Coverage-focused unit tests for libs/ra8_nsc/src/ra8_nsc_periph_init.c
 *
 * @details
 * Supplements test_ra8_nsc.c with a dedicated test binary so that every
 * reachable line in ra8_nsc_periph_init() accumulates gcov hits from an
 * independent executable.
 *
 * The function under test has two reachable branches:
 *
 * 1. First call (s_initialized == false): runs
 *    ra8_mstp_init -> ra8_pwr_init -> ra8_isr_init -> ra8_dma_init, sets
 *    s_initialized = true, and returns k_ra8_ok.
 * 2. Subsequent calls (s_initialized == true): returns k_ra8_ok immediately
 *    via the idempotent fast path.
 *
 * The four error-return branches after each init call are GCOVR_EXCL_START /
 * GCOVR_EXCL_STOP excluded in the source because all four substrate init
 * functions always return k_ra8_ok in RA8_OFF_TARGET -- the host fake
 * backs MMIO with ordinary RAM and read-back checks always pass on the first
 * iteration. Those paths exist for the Cortex-M85 target only.
 *
 * @par MC/DC:
 * Decision: ``if (s_initialized)`` in ra8_nsc_periph_init() (single condition)
 * - Vector 1: s_initialized == false -> takes init path (first call)
 * - Vector 2: s_initialized == true  -> takes fast path (subsequent calls)
 * Single-condition decisions require only 2 vectors for full MC/DC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

/* =============================================================================
 * Fixture
 * =============================================================================
 */

/**
 * @brief Reset the fake MMIO backing store before each test group.
 *
 * @details
 * Zeroes every register window so peripheral drivers start from a clean
 * state. Does NOT reset the s_initialized flag inside ra8_nsc_periph_init.c --
 * that flag is file-static and its reset to false only occurs at process start.
 * Tests are therefore ordered: the first call in this binary exercises the
 * full init path; later calls exercise the idempotent fast path.
 *
 * @pre Binary is running under RA8_OFF_TARGET.
 * @post All MMIO backing regions read zero.
 *
 * @note Not thread-safe; tests are single-threaded.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @brief ra8_nsc_periph_init: first call executes the full substrate init path.
 *
 * @details
 * When s_initialized is false (process start, never called before in this
 * binary), ra8_nsc_periph_init() must run the four-step substrate sequence:
 * ra8_mstp_init, ra8_pwr_init, ra8_isr_init, ra8_dma_init.  On the fake all
 * four always succeed, so the call must return k_ra8_ok and set the internal
 * s_initialized flag to true.
 *
 * This test exercises lines 116..117, 124..125, 132..133, 140..141, 158..160
 * of ra8_nsc_periph_init.c (non-excluded happy-path body).
 *
 * @par MC/DC:
 * Decision: ``if (s_initialized)`` -- Vector 1: s_initialized == false
 * (this is the first call in the process, so the condition evaluates false and
 * the full init body runs). Pairs with test_periph_init_idempotent_second_call
 * for the true branch.
 *
 * @pre ra8_fake_mmap_reset() has been called.
 * @pre ra8_nsc_periph_init() has never been called in this process.
 * @post ra8_nsc_periph_init() returned k_ra8_ok.
 * @post Internal s_initialized is now true.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_periph_init_first_call(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: first call -- full substrate init path");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_END("ra8_nsc_periph_init: first call -- full substrate init path");
}

/**
 * @brief ra8_nsc_periph_init: subsequent calls return k_ra8_ok via fast path.
 *
 * @details
 * After the first successful call has set s_initialized to true, every
 * following call must return k_ra8_ok immediately without re-running the
 * four substrate init steps.  This exercises line 112 (if check with
 * s_initialized == true) and line 113 (early return k_ra8_ok).
 *
 * Three back-to-back calls confirm the idempotent contract is stable
 * across repeated invocations.
 *
 * @par MC/DC:
 * Decision: ``if (s_initialized)`` -- Vector 2: s_initialized == true
 * (set by test_periph_init_first_call). The condition evaluates true and
 * the function returns early. Pairs with test_periph_init_first_call.
 *
 * @pre test_periph_init_first_call() has already run and returned k_ra8_ok.
 * @post ra8_nsc_periph_init() returns k_ra8_ok on every call.
 * @post Internal module state is unchanged (no re-initialisation).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_periph_init_idempotent_second_call(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: idempotent -- three fast-path calls");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_END("ra8_nsc_periph_init: idempotent -- three fast-path calls");
}

/**
 * @brief ra8_nsc_periph_init: return value is always k_ra8_ok on the fake.
 *
 * @details
 * Verifies that repeated ra8_nsc_periph_init() calls in different orderings
 * (with intervening ra8_fake_mmap_reset() calls) still return k_ra8_ok.
 * ra8_fake_mmap_reset() zeroes register windows but does NOT reset the static
 * s_initialized flag, so these calls all take the idempotent fast path.
 *
 * @pre test_periph_init_first_call() has already run.
 * @post All calls return k_ra8_ok.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decision is under test in this case -- it re-exercises the
 * idempotent fast path; the sole decision `if (s_initialized)` in
 * ra8_nsc_periph_init() is single-condition, and its two-vector MC/DC is covered
 * by test_periph_init_first_call (false) + test_periph_init_idempotent_second_call
 * (true). No `&&` or `||` is involved.)
 */
static void test_periph_init_always_ok_after_first(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: always k_ra8_ok after first successful init");
  prep();
  ra8_err_t err = ra8_nsc_periph_init();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  prep();
  err = ra8_nsc_periph_init();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("ra8_nsc_periph_init: always k_ra8_ok after first successful init");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs all test functions in sequence. Tests must be ordered so that
 * test_periph_init_first_call() is the very first ra8_nsc_periph_init()
 * call in the process, ensuring the full init path is exercised.
 *
 * @return int32_t Zero on success; the test framework calls exit(1) on the
 *         first assertion failure so this only reaches the fprintf on a
 *         complete pass.
 *
 * @retval 0 All tests passed.
 *
 * @pre RA8_OFF_TARGET is defined (test build).
 * @post All reachable lines in ra8_nsc_periph_init.c have been exercised.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_periph_init_first_call();
  test_periph_init_idempotent_second_call();
  test_periph_init_always_ok_after_first();
  (void)fprintf(stderr, "[OK ] test_ra8_nsc_periph_init_cov.c\n");
  return 0;
}
