/**
 * @file test_ra8_dual_core_cov.c
 * @brief Coverage-lift tests for libs/ra8_hal/src/ra8_dual_core.c
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Exercises the one branch in ra8_cpu1_is_running() not reached by the sibling
 * test_app_cpu1_pingpong.c: the early-return path when CPU1 has never been
 * activated (ACT=0, line "return false" inside "if (!active)").
 *
 * Root cause of the gap: test_app_cpu1_pingpong calls ra8_cpu1_is_running()
 * only after at least one successful ra8_cpu1_release(), which sets ACT=1 in
 * s_sim.actcsr (file-static to the TU).  The halt used in reset_world() sets
 * CPUWAIT=1 but does not clear ACT, so subsequent is_running() calls always
 * take the "active but stalled" branch (line "return !stalled"), never the
 * "not active" branch.  Because s_sim is a file-static struct initialized to
 * zero by the C runtime, the "not active" path is reachable only in a fresh
 * process before any successful release.  This executable provides that fresh
 * context.
 *
 * MC/DC coverage summary for ra8_cpu1_is_running() (2 sequential conditions):
 *
 *   Condition 1 -- active = (ACT bit set):
 *     Vector A (this file): active=false -> returns false via early return.
 *     Vector B (sibling):   active=true,  stalled=false -> returns true.
 *     A vs B: active varies (false vs true), stalled irrelevant in A;
 *     output flips (false vs true) -> active independently affects outcome.
 *
 *   Condition 2 -- stalled = (CPUWAIT bit set):
 *     Vector B (sibling): active=true, stalled=false -> returns true.
 *     Vector C (sibling): active=true, stalled=true  -> returns false.
 *     B vs C: stalled varies (false vs true), active fixed at true;
 *     output flips (true vs false) -> stalled independently affects outcome.
 *
 *   Three vectors {A, B, C} provide N+1 = 3 for N=2 conditions: minimal MC/DC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/** @brief Dummy CPU1 entry address: 128-byte aligned, above MRAM region. */
typedef enum : uint32_t {
  k_dc_cov_dummy_entry = 0x02080000U, /**< 128-byte aligned; same as pingpong test. */
  k_dc_cov_dummy_sp    = 0x22180000U, /**< 8-byte aligned; top of SRAM range.       */
} dc_cov_addr_t;

/**
 * @brief Shared per-test teardown: scrub MMIO backing store.
 *
 * @details
 * Calls ra8_sim_mmap_reset() to zero the peripheral-window RAM.  The
 * module-internal s_sim struct is file-static to ra8_dual_core.c and is
 * NOT reset by this call; tests in this file are ordered so that the
 * "not yet activated" (ACT=0) test always runs first, before any
 * ra8_cpu1_release() can set the ACT bit.
 *
 * @pre None.
 * @post Peripheral-window RAM zeroed.
 * @post s_sim.actcsr is unchanged (not accessible from here).
 * @note Not thread-safe; test suite is single-threaded.
 * @since 0.1.0
 */
static void reset_mmio(void)
{
  ra8_sim_mmap_reset();
}

/**
 * @test test_dc_cov_is_running_before_release
 *
 * @brief ra8_cpu1_is_running() returns false when CPU1 has never been activated.
 *
 * @details
 * At process start s_sim.actcsr == 0 (zero-initialized static).  The ACT
 * bit (bit 7) is therefore 0, so internal_actcsr_read() returns 0, making
 * "active" false.  The "if (!active)" branch fires and the function returns
 * false via the early-return path.  This is the only test that must run
 * before any ra8_cpu1_release() call; subsequent tests in this file may alter
 * s_sim.actcsr.
 *
 * @par MC/DC:
 * Condition: active = (internal_actcsr_read() & ACT_MASK) != 0U
 * - Vector A (this test): ACT=0 -> active=false -> returns false [early return].
 * This vector, paired with Vector B from the sibling test (active=true,
 * stalled=false -> true), proves active independently affects the outcome.
 *
 * @pre s_sim.actcsr == 0 (fresh process, no prior ra8_cpu1_release call).
 * @pre ra8_sim_mmap_reset() has not altered s_sim (it does not -- s_sim is
 *      private to ra8_dual_core.c).
 * @post ra8_cpu1_is_running() returned false.
 * @post s_sim is unchanged.
 * @note Must execute before any ra8_cpu1_release() in this process.
 * @since 0.1.0
 */
static void test_dc_cov_is_running_before_release(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: is_running returns false before any release (ACT=0)");
  /* s_sim.actcsr starts at 0: ACT bit is clear -> active=false -> early return false. */
  TEST_ASSERT_EQ(0, ra8_cpu1_is_running());
  TEST_END("ra8_dual_core_cov: is_running returns false before any release (ACT=0)");
}

/**
 * @test test_dc_cov_release_sets_cpu1_active
 *
 * @brief ra8_cpu1_release() with valid args succeeds and CPU1 becomes running.
 *
 * @details
 * Verifies the happy path: a 128-byte-aligned entry and 8-byte-aligned sp
 * pass all validation checks, the sim shim sets ACT synchronously on the
 * ACTREQ write, and the poll returns k_ra8_ok on its first iteration.
 * After release, ra8_cpu1_is_running() must return true (ACT=1, CPUWAIT=0).
 *
 * @par MC/DC:
 * ra8_cpu1_release() contains no compound boolean decisions (all conditions
 * are on separate ifs).  Each branch is covered by a single input vector.
 *
 * @pre ra8_cpu1_is_running() was already exercised with ACT=0 (test above).
 * @pre ra8_sim_mmap_reset() called in reset_mmio().
 * @post s_sim.actcsr has the ACT bit set.
 * @post ra8_cpu1_is_running() returns true.
 * @note Must follow test_dc_cov_is_running_before_release in this process.
 * @since 0.1.0
 */
static void test_dc_cov_release_sets_cpu1_active(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: release with valid args -> is_running true");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_cpu1_release((void*)(uintptr_t)k_dc_cov_dummy_entry, (void*)(uintptr_t)k_dc_cov_dummy_sp));
  TEST_ASSERT_EQ(1, ra8_cpu1_is_running());
  TEST_END("ra8_dual_core_cov: release with valid args -> is_running true");
}

/**
 * @test test_dc_cov_halt_stops_cpu1
 *
 * @brief ra8_cpu1_halt() stalls CPU1 and ra8_cpu1_is_running() returns false.
 *
 * @details
 * After a successful release (previous test), CPUWAIT=0.  Halt writes
 * CPUWAIT=1.  ra8_cpu1_is_running() then sees ACT=1 (still set) and
 * CPUWAIT=1 (stalled) -> returns !stalled = false.
 *
 * @par MC/DC:
 * ra8_cpu1_halt() has no compound boolean decisions.  The single call
 * exercises the non-error path.
 *
 * @pre s_sim.actcsr has ACT=1 from the preceding release test.
 * @post s_sim.waitcr has CPUWAIT=1.
 * @post ra8_cpu1_is_running() returns false.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dc_cov_halt_stops_cpu1(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: halt -> is_running false (active but stalled)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cpu1_halt());
  TEST_ASSERT_EQ(0, ra8_cpu1_is_running());
  TEST_END("ra8_dual_core_cov: halt -> is_running false (active but stalled)");
}

/**
 * @test test_dc_cov_release_null_entry
 *
 * @brief ra8_cpu1_release() rejects a NULL entry pointer.
 *
 * @details
 * Validates the first null-pointer guard: entry==nullptr, sp valid ->
 * returns k_ra8_err_null_ptr without touching any CPU_CTRL registers.
 *
 * @par MC/DC:
 * Decision: entry == nullptr || sp == nullptr (2 conditions).
 * - Vector N (this test): entry=NULL, sp=valid -> true (entry varies).
 * Vectors for sp=NULL and both=valid are in test_app_cpu1_pingpong.c.
 *
 * @pre None.
 * @post s_sim is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dc_cov_release_null_entry(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: release rejects NULL entry");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cpu1_release(nullptr, (void*)(uintptr_t)k_dc_cov_dummy_sp));
  TEST_END("ra8_dual_core_cov: release rejects NULL entry");
}

/**
 * @test test_dc_cov_release_misaligned_entry
 *
 * @brief ra8_cpu1_release() rejects an entry address that is not 128-byte aligned.
 *
 * @details
 * Adds 1 to the dummy entry (making it odd-byte aligned), which fails the
 * 128-byte alignment check (bits [6:0] != 0).
 *
 * @par MC/DC:
 * Single condition: (entry & (128-1)) != 0.  This test provides the true arm.
 *
 * @pre None.
 * @post s_sim is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dc_cov_release_misaligned_entry(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: release rejects mis-aligned entry");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cpu1_release((void*)((uintptr_t)k_dc_cov_dummy_entry + 1U),
                                  (void*)(uintptr_t)k_dc_cov_dummy_sp));
  TEST_END("ra8_dual_core_cov: release rejects mis-aligned entry");
}

/**
 * @test test_dc_cov_release_misaligned_sp
 *
 * @brief ra8_cpu1_release() rejects an sp address that is not 8-byte aligned.
 *
 * @details
 * Adds 1 to the dummy sp (making it non-8-byte aligned), which fails the
 * 8-byte alignment check (bits [2:0] != 0).
 *
 * @par MC/DC:
 * Single condition: (sp & (8-1)) != 0.  This test provides the true arm.
 *
 * @pre None.
 * @post s_sim is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dc_cov_release_misaligned_sp(void)
{
  reset_mmio();
  TEST_BEGIN("ra8_dual_core_cov: release rejects mis-aligned sp");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cpu1_release((void*)(uintptr_t)k_dc_cov_dummy_entry,
                                  (void*)((uintptr_t)k_dc_cov_dummy_sp + 1U)));
  TEST_END("ra8_dual_core_cov: release rejects mis-aligned sp");
}

/**
 * @test test_dc_cov_release_act_timeout
 *
 * @brief ra8_cpu1_release() reports k_ra8_err_timeout when ACT never asserts.
 *
 * @details
 * Arms the ra8_sim_mmio fault seam on the CPU1ACTCSR key so the ACT poll
 * never observes the activation bit: the bounded loop exhausts its budget
 * and release returns the real hardware-timeout error instead of the fake
 * success the deleted RA8_SIMULATOR_MODE short-circuit used to return. Runs
 * last so the armed fault and the ACTREQ write cannot perturb the earlier
 * ordering-sensitive ACT-state tests.
 *
 * @par MC/DC:
 * (no compound decisions -- the ACT poll's loop-exit is a single-condition
 * seam consult, not a compound boolean)
 *
 * @pre The ra8_sim_mmio seam is reset then armed to fail on the ACTCSR key.
 * @pre Caller runs on the single-threaded host test harness.
 * @post ra8_cpu1_release() returns k_ra8_err_timeout.
 * @post The seam is reset so no armed fault leaks past this test.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dc_cov_release_act_timeout(void)
{
  reset_mmio();
  ra8_sim_mmio_reset();
  TEST_BEGIN("ra8_dual_core_cov: release ACT-poll timeout");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait(ra8_dual_core_test_actcsr_key()));
  TEST_ASSERT_EQ(
    k_ra8_err_timeout,
    ra8_cpu1_release((void*)(uintptr_t)k_dc_cov_dummy_entry, (void*)(uintptr_t)k_dc_cov_dummy_sp));
  ra8_sim_mmio_reset();
  TEST_END("ra8_dual_core_cov: release ACT-poll timeout");
}

/**
 * @brief Entry point: run all coverage-lift tests in a deterministic order.
 *
 * @details
 * ORDERING IS CRITICAL: test_dc_cov_is_running_before_release MUST be the
 * first test so that s_sim.actcsr is still at its zero-initialized value.
 * Subsequent tests may call ra8_cpu1_release() which sets the ACT bit.
 *
 * @return int Zero on success; exit(1) fires inside a failing TEST_ASSERT.
 * @since 0.1.0
 */
int main(void)
{
  /* This test MUST run before any ra8_cpu1_release() call (ACT=0 initial state). */
  test_dc_cov_is_running_before_release();

  /* Validation-path tests: no release succeeds here, s_sim.actcsr stays 0. */
  test_dc_cov_release_null_entry();
  test_dc_cov_release_misaligned_entry();
  test_dc_cov_release_misaligned_sp();

  /* Lifecycle tests: release succeeds, ACT=1 after this point. */
  test_dc_cov_release_sets_cpu1_active();
  test_dc_cov_halt_stops_cpu1();

  /* Timeout leg last: arms the seam and writes ACTREQ, so it must not precede
   * the ordering-sensitive ACT-state tests above. */
  test_dc_cov_release_act_timeout();

  (void)fprintf(stderr, "[OK  ] test_ra8_dual_core_cov.c\n");
  return 0;
}
