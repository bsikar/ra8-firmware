/**
 * @file test_ra8_nsc_periph_init_cov.c
 * @brief Coverage-focused unit tests for libs/ra8_nsc/src/ra8_nsc_periph_init.c
 *
 * @details
 * Supplements test_ra8_nsc.c with a dedicated test binary so that every
 * reachable line in ra8_nsc_periph_init() accumulates gcov hits from an
 * independent executable.
 *
 * The function under test has an idempotent fast path, a complete successful
 * initialization path, and four dependency-error paths:
 *
 * 1. First call (s_initialized == false): runs
 *    ra8_mstp_init -> ra8_pwr_init -> ra8_isr_init -> ra8_dma_init, sets
 *    s_initialized = true, and returns k_ra8_ok.
 * 2. Subsequent calls (s_initialized == true): returns k_ra8_ok immediately
 *    via the idempotent fast path.
 *
 * The error-return branch after ra8_mstp_init() is host-drivable: the bounded
 * MSTPCR read-back poll routes its loop-exit decision through the
 * ra8_fake_mmio seam, so arming ra8_fake_mmio_fail_wait() on MSTPCRA makes
 * ra8_mstp_init() report k_ra8_err_hw_timeout and ra8_nsc_periph_init()
 * report k_ra8_err_hw_init_failed. Because s_initialized latches on the first
 * successful call, that case has to run before any successful one, which is
 * why internal_test_periph_init_mstp_fault_first_call heads main(). The
 * remaining three error legs are driven by link wrapping in this focused test
 * executable. Each wrapper either forwards to the real implementation or
 * returns a deterministic error; production code and other tests are
 * unaffected.
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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

/** @brief Dependency selected to fail in the link-wrapped init chain. */
typedef enum : uint8_t {
  k_nsc_dep_none = 0U, /**< Wrapped dependencies call their real implementation. */
  k_nsc_dep_pwr,       /**< ra8_pwr_init returns a deterministic fault.          */
  k_nsc_dep_isr,       /**< ra8_isr_init returns a deterministic fault.          */
  k_nsc_dep_dma,       /**< ra8_dma_init returns a deterministic fault.          */
} nsc_dep_fault_t;

static nsc_dep_fault_t s_dep_fault;

/** @brief Linker-provided entry point for the real power initializer. */
extern ra8_err_t ra8_test_real_pwr_init(void) __asm__("__real_ra8_pwr_init");

/** @brief Linker-provided entry point for the real interrupt initializer. */
extern ra8_err_t ra8_test_real_isr_init(void) __asm__("__real_ra8_isr_init");

/** @brief Linker-provided entry point for the real DMA initializer. */
extern ra8_err_t ra8_test_real_dma_init(void) __asm__("__real_ra8_dma_init");

/** @brief Inject or forward the power-initialization result. */
RA8_TEST_HELPER ra8_err_t ra8_test_pwr_init_intercept(void) __asm__("__wrap_ra8_pwr_init");

RA8_TEST_HELPER ra8_err_t ra8_test_pwr_init_intercept(void)
{
  return s_dep_fault == k_nsc_dep_pwr ? k_ra8_err_hw_timeout : ra8_test_real_pwr_init();
}

/** @brief Inject or forward the interrupt-initialization result. */
RA8_TEST_HELPER ra8_err_t ra8_test_isr_init_intercept(void) __asm__("__wrap_ra8_isr_init");

RA8_TEST_HELPER ra8_err_t ra8_test_isr_init_intercept(void)
{
  return s_dep_fault == k_nsc_dep_isr ? k_ra8_err_hw_timeout : ra8_test_real_isr_init();
}

/** @brief Inject or forward the DMA-initialization result. */
RA8_TEST_HELPER ra8_err_t ra8_test_dma_init_intercept(void) __asm__("__wrap_ra8_dma_init");

RA8_TEST_HELPER ra8_err_t ra8_test_dma_init_intercept(void)
{
  return s_dep_fault == k_nsc_dep_dma ? k_ra8_err_hw_timeout : ra8_test_real_dma_init();
}

/* =============================================================================
 * Fixture
 * =============================================================================
 */

/**
 * @brief Reset the fake MMIO backing store before each test group.
 *
 * @details
 * Zeroes every register window and disarms every armed MMIO fault so
 * peripheral drivers start from a clean state. Does NOT reset the
 * s_initialized flag inside ra8_nsc_periph_init.c --
 * that flag is file-static and its reset to false only occurs at process start.
 * Tests are therefore ordered: the first call in this binary exercises the
 * full init path; later calls exercise the idempotent fast path.
 *
 * @pre Binary is running under RA8_OFF_TARGET.
 * @pre No other test concurrently accesses fake MMIO.
 * @post All MMIO backing regions read zero and no register is armed to fault.
 * @post The production initialization latch is left unchanged.
 *
 * @note Not thread-safe; tests are single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  s_dep_fault = k_nsc_dep_none;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @brief ra8_nsc_periph_init: a module-stop read-back fault surfaces as
 *        k_ra8_err_hw_init_failed and does NOT latch s_initialized.
 *
 * @details
 * ra8_mstp_init() writes the all-stopped pattern into MSTPCRA..MSTPCRE and
 * then read-back-polls each register (HUM Ch 11.2.6 Note 2). On the host that
 * poll routes its loop-exit decision through the ra8_fake_mmio seam, so
 * arming MSTPCRA to never satisfy makes the very first register time out.
 * ra8_nsc_periph_init() must translate that into k_ra8_err_hw_init_failed and
 * leave the substrate un-latched so a retry re-runs the whole sequence. The
 * next case in main() is what proves the latch stayed open: it still takes
 * the full init path rather than the idempotent fast path.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the ra8_mstp_init() call in
 * ra8_nsc_periph_init() (1 condition).
 * - Vector 1: MSTPCRA read-back settles -> ra8_mstp_init() returns k_ra8_ok
 *   -> the decision is false and bring-up continues (control; covered by
 *   internal_test_periph_init_first_call immediately below).
 * - Vector 2: MSTPCRA armed to never satisfy -> ra8_mstp_init() returns
 *   k_ra8_err_hw_timeout -> the decision is true and the veneer reports
 *   k_ra8_err_hw_init_failed (this case).
 * N = 1 condition, N+1 = 2 vectors. The two differ only in the read-back
 * outcome and give opposite results, so `err` independently affects the
 * decision.
 *
 * @pre This runs before any successful ra8_nsc_periph_init() in the process.
 * @pre ra8_fake_mmap_reset() has been called.
 * @post ra8_nsc_periph_init() returned k_ra8_err_hw_init_failed.
 * @post The production initialization latch is still false.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_periph_init_mstp_fault_first_call(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: MSTP read-back fault reports hw_init_failed");
  internal_prep();
  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&ra8_mstp()->MSTPCRA));
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_nsc_periph_init());
  ra8_fake_mmio_reset();
  TEST_END("ra8_nsc_periph_init: MSTP read-back fault reports hw_init_failed");
}

/** @brief Each wrapped downstream dependency failure remains retryable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_periph_init_dependency_faults(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: downstream dependency faults");
  const nsc_dep_fault_t faults[] = {
    k_nsc_dep_pwr,
    k_nsc_dep_isr,
    k_nsc_dep_dma,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof faults / sizeof faults[0]); ++i) {
    internal_prep();
    s_dep_fault = faults[i];
    TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_nsc_periph_init());
    s_dep_fault = k_nsc_dep_none;
  }
  TEST_END("ra8_nsc_periph_init: downstream dependency faults");
}

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
 * the full init body runs). Pairs with internal_test_periph_init_idempotent_second_call
 * for the true branch.
 *
 * @pre ra8_fake_mmap_reset() has been called.
 * @pre No earlier ra8_nsc_periph_init() call in this process has SUCCEEDED
 *      (internal_test_periph_init_mstp_fault_first_call ran and failed).
 * @post ra8_nsc_periph_init() returned k_ra8_ok.
 * @post Internal s_initialized is now true.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_periph_init_first_call(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: first call -- full substrate init path");
  internal_prep();
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
 * (set by internal_test_periph_init_first_call). The condition evaluates true and
 * the function returns early. Pairs with internal_test_periph_init_first_call.
 *
 * @pre internal_test_periph_init_first_call() has already run and returned k_ra8_ok.
 * @pre The production initialization latch is therefore true.
 * @post ra8_nsc_periph_init() returns k_ra8_ok on every call.
 * @post Internal module state is unchanged (no re-initialisation).
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_periph_init_idempotent_second_call(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: idempotent -- three fast-path calls");
  internal_prep();
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
 * @pre internal_test_periph_init_first_call() has already run.
 * @pre The test process still owns the fake MMIO mappings.
 * @post All calls return k_ra8_ok.
 * @post The production initialization latch remains true.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decision is under test in this case -- it re-exercises the
 * idempotent fast path; the sole decision `if (s_initialized)` in
 * ra8_nsc_periph_init() is single-condition, and its two-vector MC/DC is covered
 * by internal_test_periph_init_first_call (false) + internal_test_periph_init_idempotent_second_call
 * (true). No `&&` or `||` is involved.)
 */
RA8_INTERNAL
static void internal_test_periph_init_always_ok_after_first(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: always k_ra8_ok after first successful init");
  internal_prep();
  ra8_err_t err = ra8_nsc_periph_init();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  internal_prep();
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
 * Runs all test functions in sequence. Order is load-bearing:
 * internal_test_periph_init_mstp_fault_first_call() must run first (it needs
 * s_initialized still false to reach the substrate sequence at all), and
 * internal_test_periph_init_first_call() must be the first SUCCESSFUL call so
 * the full init path is exercised.
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
int main(void)
{
  internal_test_periph_init_mstp_fault_first_call();
  internal_test_periph_init_dependency_faults();
  internal_test_periph_init_first_call();
  internal_test_periph_init_idempotent_second_call();
  internal_test_periph_init_always_ok_after_first();
  return 0;
}
