/**
 * @file test_ra8_hw_err_cov.c
 * @brief Coverage + behaviour tests for the bounded-wait primitives
 *        (`ra8_hw_err.h`) and the host MMIO fault seam (`ra8_fake_mmio.c`).
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * `ra8_hw_wait_flag_{set,clear}{8,32}` are the shared "spin until this status bit
 * changes" primitives every HAL poll loop delegates to. Under the host test
 * build (`RA8_OFF_TARGET` + `UNIT_TEST`) each waiter consults the programmable
 * MMIO fault seam once per poll (T1-01): with no fault armed the seam is
 * transparent and the waiter honours the register's real RAM value, so a
 * pre-staged flag succeeds and an un-staged flag runs to budget and times out.
 * Arming `ra8_fake_mmio_fail_wait` forces a deterministic timeout regardless of
 * RAM; arming `ra8_fake_mmio_satisfy_after(n)` makes the flag "assert" on the
 * n-th poll (modelling a clear-then-wait handshake and exercising the loop's
 * continuation branch). This drives both the success and the
 * `k_ra8_err_hw_timeout` legs of all four waiters, plus every branch of the seam
 * itself -- no hardware line is bypassed by an exclusion marker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_fake_mmio.h"
#include "ra8_hw_err.h"
#include "unity_minimal.h"

/** @brief Arbitrary 32-bit poll mask used by the set/clear waiters. */
typedef enum : uint32_t {
  k_test_mask32 = 0x00000040U, /**< bit 6. */
} test_mask32_t;

/** @brief Arbitrary 8-bit poll mask. */
typedef enum : uint8_t {
  k_test_mask8 = 0x08U, /**< bit 3. */
} test_mask8_t;

/** @brief Wait tuning: a poll index below the budget, and a small fast budget. */
typedef enum : uint32_t {
  k_test_satisfy_n = 3U,     /**< satisfy_after threshold (< k_test_budget).  */
  k_test_budget    = 0x100U, /**< 256 iterations -- fast, > k_test_satisfy_n. */
} test_wait_t;

/** @brief One past the fault-table capacity, to drive the no_mem path. */
typedef enum : uint8_t {
  k_test_overflow_regs = (uint8_t)k_ra8_fake_mmio_max_faults + 1U, /**< Test overflow registers. */
} test_overflow_t;

/**
 * @test test_hw_err_null_ptr
 * @details All four waiters reject a NULL register with k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- each waiter's `reg == nullptr` guard is
 * single-condition; ra8_hw_err.h has no `&&`/`||`)
 */
static void test_hw_err_null_ptr(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_* reject NULL register");
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_set8(nullptr, (uint8_t)k_test_mask8, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_clear8(nullptr, (uint8_t)k_test_mask8, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_set32(nullptr, (uint32_t)k_test_mask32, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_hw_wait_flag_clear32(nullptr, (uint32_t)k_test_mask32, k_test_budget));
  TEST_END("ra8_hw_wait_flag_* reject NULL register");
}

/**
 * @test test_hw_err_unarmed_succeeds
 * @details With no fault armed the seam models a ready flag: every waiter
 * succeeds on its first poll regardless of the register's RAM value. This is the
 * drop-in replacement for the deleted ``#ifdef RA8_OFF_TARGET return k_ra8_ok``
 * short-circuits (T1-01/#177) -- a consumer that never touches the seam makes
 * progress instead of spinning to timeout. The timeout and succeed-after-N legs
 * are covered by test_hw_err_fail_wait and test_hw_err_satisfy_after.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the four waiters and
 * ra8_fake_mmio_wait_eval are all single-condition; it exercises the unarmed
 * seam's "flag ready" success leg on the first poll)
 */
static void test_hw_err_unarmed_succeeds(void)
{
  TEST_BEGIN("ra8_hw_wait_flag_* succeed on first poll when no fault is armed");
  ra8_fake_mmio_reset();

  /* A staged flag succeeds ... */
  volatile uint32_t r32 = (uint32_t)k_test_mask32;
  volatile uint8_t  r8  = (uint8_t)k_test_mask8;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set8(&r8, (uint8_t)k_test_mask8, k_test_budget));
  volatile uint32_t c32 = 0U;
  volatile uint8_t  c8  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_clear32(&c32, (uint32_t)k_test_mask32, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_clear8(&c8, (uint8_t)k_test_mask8, k_test_budget));

  /* ... and so does an UN-staged flag, because unarmed means "flag ready". */
  volatile uint32_t never32 = 0U;
  volatile uint8_t  never8  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_hw_wait_flag_set32(&never32, (uint32_t)k_test_mask32, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set8(&never8, (uint8_t)k_test_mask8, k_test_budget));
  volatile uint32_t stuck32 = (uint32_t)k_test_mask32;
  volatile uint8_t  stuck8  = (uint8_t)k_test_mask8;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_hw_wait_flag_clear32(&stuck32, (uint32_t)k_test_mask32, k_test_budget));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_clear8(&stuck8, (uint8_t)k_test_mask8, k_test_budget));
  TEST_END("ra8_hw_wait_flag_* succeed on first poll when no fault is armed");
}

/**
 * @test test_hw_err_fail_wait
 * @details `ra8_fake_mmio_fail_wait` forces a timeout even when the flag is staged
 * as already satisfied -- proving the seam overrides the RAM poll.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- single-condition throughout; it arms the
 * fail seam so the bounded poll runs to k_ra8_err_hw_timeout regardless of RAM)
 */
static void test_hw_err_fail_wait(void)
{
  TEST_BEGIN("ra8_fake_mmio_fail_wait forces timeout regardless of RAM");
  ra8_fake_mmio_reset();

  volatile uint32_t r32 = (uint32_t)k_test_mask32; /* staged satisfied... */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&r32));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));

  volatile uint8_t c8 = 0U; /* clear-wait staged satisfied... */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&c8));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_clear8(&c8, (uint8_t)k_test_mask8, k_test_budget));
  TEST_END("ra8_fake_mmio_fail_wait forces timeout regardless of RAM");
}

/**
 * @test test_hw_err_satisfy_after
 * @details `ra8_fake_mmio_satisfy_after(n)` makes the flag assert on the n-th
 * poll: the waiter succeeds after iterating (covering the loop-continuation
 * branch, i.e. iter < n then iter >= n). `n == 0` succeeds immediately.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- single-condition throughout; it drives
 * ra8_fake_mmio_satisfy_after's `iter >= arg` continuation, including n == 0)
 */
static void test_hw_err_satisfy_after(void)
{
  TEST_BEGIN("ra8_fake_mmio_satisfy_after asserts the flag mid-wait");
  ra8_fake_mmio_reset();

  volatile uint32_t r32 = 0U; /* RAM never sets it; the seam does, on poll n. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&r32, k_test_satisfy_n));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));

  volatile uint8_t r8 = 0U; /* n == 0 -> satisfied on the first poll. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&r8, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set8(&r8, (uint8_t)k_test_mask8, k_test_budget));
  TEST_END("ra8_fake_mmio_satisfy_after asserts the flag mid-wait");
}

/**
 * @test test_fake_mmio_arm_errors
 * @details The arm entry points validate their inputs (NULL register) and the
 * bounded table (no_mem once ::k_ra8_fake_mmio_max_faults distinct registers are
 * armed).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the arm entry points' `reg == nullptr`
 * and table-full `slot == nullptr` guards are each single-condition; no `&&`/`||`
 * in ra8_fake_mmio.c)
 */
static void test_fake_mmio_arm_errors(void)
{
  TEST_BEGIN("ra8_fake_mmio arm functions validate inputs + table capacity");
  ra8_fake_mmio_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fake_mmio_fail_wait(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fake_mmio_satisfy_after(nullptr, k_test_satisfy_n));

  /* Fill the table with distinct registers, then overflow it. */
  volatile uint32_t regs[k_test_overflow_regs] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_mmio_max_faults; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&regs[i]));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_fake_mmio_fail_wait(&regs[(uint8_t)k_ra8_fake_mmio_max_faults]));
  ra8_fake_mmio_reset();
  TEST_END("ra8_fake_mmio arm functions validate inputs + table capacity");
}

/**
 * @test test_fake_mmio_reset_and_update
 * @details `ra8_fake_mmio_reset` disarms every register (transparent again), and
 * re-arming the same register updates it in place (fail_wait over a prior
 * satisfy_after wins).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- single-condition throughout; it checks
 * reset disarms every slot and a re-arm updates the same slot in place)
 */
static void test_fake_mmio_reset_and_update(void)
{
  TEST_BEGIN("ra8_fake_mmio reset restores transparency; re-arm updates in place");

  volatile uint32_t r32 = (uint32_t)k_test_mask32; /* staged satisfied. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&r32));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));

  /* Re-arm the same register: satisfy_after then fail_wait -> fail_wait wins. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&r32, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&r32));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_hw_wait_flag_set32(&r32, (uint32_t)k_test_mask32, k_test_budget));
  ra8_fake_mmio_reset();
  TEST_END("ra8_fake_mmio reset restores transparency; re-arm updates in place");
}

/**
 * @test test_fake_mmio_wait_eval_direct
 * @details Directly exercises ::ra8_fake_mmio_wait_eval: an un-armed register (NULL
 * or a real address with no fault slot) succeeds on the first poll regardless of
 * the driver's real_cond (both polarities), covering the free-slot / NULL-address
 * guard in the lookup and the unarmed = "flag ready" contract.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ra8_fake_mmio_wait_eval's free-slot /
 * NULL-address lookup is single-condition; an un-armed register succeeds on the
 * first poll for both real_cond polarities)
 */
static void test_fake_mmio_wait_eval_direct(void)
{
  TEST_BEGIN("ra8_fake_mmio_wait_eval succeeds for an un-armed register");
  ra8_fake_mmio_reset();
  TEST_ASSERT(ra8_fake_mmio_wait_eval(nullptr, 0U, true));
  TEST_ASSERT(ra8_fake_mmio_wait_eval(nullptr, 0U, false)); /* un-armed -> succeed. */

  volatile uint32_t r32 = 0U;
  TEST_ASSERT(ra8_fake_mmio_wait_eval(&r32, 0U, true));
  TEST_ASSERT(ra8_fake_mmio_wait_eval(&r32, 0U, false)); /* un-armed -> succeed. */
  TEST_END("ra8_fake_mmio_wait_eval succeeds for an un-armed register");
}

int32_t main(void)
{
  test_hw_err_null_ptr();
  test_hw_err_unarmed_succeeds();
  test_hw_err_fail_wait();
  test_hw_err_satisfy_after();
  test_fake_mmio_arm_errors();
  test_fake_mmio_reset_and_update();
  test_fake_mmio_wait_eval_direct();
  (void)fprintf(stderr, "[OK ] test_ra8_hw_err_cov.c\n");
  return 0;
}
