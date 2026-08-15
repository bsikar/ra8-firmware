/**
 * @file test_ra8_ipc_sem.c
 * @brief Unit tests for the ra8_ipc hardware-semaphore family
 *
 * @details Split from test_ra8_ipc.c along the test-group seam: covers
 * the semaphore accessor, try-take/release, the bounded take-timeout
 * spin (with a poll-hook releasing the semaphore mid-spin), and
 * is-locked queries. Shared fixture state lives in
 * support/ipc_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"
#include "ra8_isr.h"
#include "support/ipc_test_util.h"
#include "unity_minimal.h"

/**
 * @enum ipc_sem_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_ipc_sem_test_idx_a = 5U, /**< Ipc sem test index a. */
  k_ipc_sem_test_idx_b = 7U, /**< Ipc sem test index b. */
} ipc_sem_test_lit_t;

/* ---------- Semaphore tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_semaphore_accessor(void)
{
  TEST_BEGIN("ipc semaphore accessor");
  prep();
  volatile uint32_t* sem0  = ra8_ipc_sem(0U);
  volatile uint32_t* sem15 = ra8_ipc_sem((uint8_t)(k_ra8_ipc_sem_count - 1U));
  TEST_ASSERT_NOT_NULL((void*)sem0);
  TEST_ASSERT_NOT_NULL((void*)sem15);
  TEST_ASSERT((uintptr_t)sem15 - (uintptr_t)sem0 ==
              ((uintptr_t)(k_ra8_ipc_sem_count - 1U) * sizeof(uint32_t)));
  TEST_ASSERT_NULL((void*)ra8_ipc_sem((uint8_t)k_ra8_ipc_sem_count));
  TEST_END("ipc semaphore accessor");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sem_try_take_and_release(void)
{
  TEST_BEGIN("ipc sem try_take / release");
  prep();
  /* Zero -> first take returns ok. */
  volatile uint32_t* sem = ra8_ipc_sem(3U);
  TEST_ASSERT_NOT_NULL((void*)sem);
  *sem = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_try_take(3U));
  /* The successful take's read latched LOCK = 1 (HUM Ch 3.2.3 set
   * condition, applied to the RAM register file by the ra8_fake_mmio
   * read-to-set model). */
  TEST_ASSERT((*sem & (uint32_t)k_ra8_ipc_sem_mask_lock) != 0U);
  /* Mutual exclusion: a second take must observe the latch and report
   * busy -- the driver runs its real read-and-decode, no in-driver
   * host model involved. */
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ipc_sem_try_take(3U));
  /* Release: HUM Ch 3.2.3 says writing 1 clears LOCK; the ra8_fake_mmio
   * write-1-to-clear model applies the clear the way silicon does, so
   * post-release LOCK reads back 0 (free). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_release(3U));
  TEST_ASSERT((*sem & (uint32_t)k_ra8_ipc_sem_mask_lock) == 0U);
  /* Released -> a re-take succeeds again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_try_take(3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_release(3U));
  /* Force "already locked" by leaving LOCK = 1 in the backing word. */
  *sem = (uint32_t)k_ra8_ipc_sem_mask_lock;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ipc_sem_try_take(3U));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_sem_try_take((uint8_t)k_ra8_ipc_test_sem_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_sem_release((uint8_t)k_ra8_ipc_test_sem_bad));
  TEST_END("ipc sem try_take / release");
}

/**
 * @var s_sem_hook_polls
 * @brief Read-take polls observed by ``hook_release_sem_at_nth_poll``.
 *
 * @details Incremented once per seam-routed IPCSEM read; compared
 * against ::s_sem_hook_release_at to decide when the modelled peer
 * core releases the semaphore.
 * @note Reset by the test before installing the hook.
 * @warning Only meaningful while the hook is installed.
 * @since 0.1.0
 */
static uint32_t s_sem_hook_polls;

/**
 * @var s_sem_hook_target
 * @brief IPCSEMn register the modelled peer core releases.
 *
 * @details Set by the test to the semaphore under contention.
 * @note nullptr disables the hook body.
 * @warning Only meaningful while the hook is installed.
 * @since 0.1.0
 */
static volatile uint32_t* s_sem_hook_target;

/**
 * @var s_sem_hook_release_at
 * @brief 1-based poll index at which the modelled peer releases.
 *
 * @details The hook clears LOCK right before the driver's Nth read.
 * @note Reset by the test before installing the hook.
 * @warning Only meaningful while the hook is installed.
 * @since 0.1.0
 */
static uint32_t s_sem_hook_release_at;

/**
 * @brief Poll hook modelling a peer core releasing an IPCSEM mid-spin.
 *
 * @details
 * Installed via ``ra8_fake_mmio_set_poll_hook``; the seam invokes it on
 * the driver's own thread right before each seam-routed IPCSEM read.
 * From the configured poll onward it clears LOCK in the backing word,
 * exactly what the driver would observe on silicon when the peer core
 * writes the IPCSEMn release command mid-spin.
 *
 * @pre ::s_sem_hook_target / ::s_sem_hook_release_at configured.
 * @pre Installed through ``ra8_fake_mmio_set_poll_hook``.
 * @post ::s_sem_hook_polls counts every invocation.
 * @post LOCK is cleared from the configured poll onward.
 * @note Single-threaded (runs inline with the driver's poll).
 * @since 0.1.0
 */
static void hook_release_sem_at_nth_poll(void)
{
  ++s_sem_hook_polls;
  if (s_sem_hook_target == nullptr) {
    return;
  }
  if (s_sem_hook_polls >= s_sem_hook_release_at) {
    *s_sem_hook_target = 0U;
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the take_timeout spin
 * loop through both of its single-condition branch outcomes: the
 * ``prev == 0`` acquire test is false for polls 1..2 and true on poll
 * 3, and the bounded ``i < max_spins`` loop test is true (continue)
 * then exits via the acquire return, never the budget)
 */
static void test_sem_take_timeout_spins_then_acquires(void)
{
  TEST_BEGIN("ipc sem take_timeout spins then acquires");
  prep();
  volatile uint32_t* sem = ra8_ipc_sem(k_ipc_sem_test_idx_a);
  TEST_ASSERT_NOT_NULL((void*)sem);
  /* Peer core currently owns the semaphore. */
  *sem = (uint32_t)k_ra8_ipc_sem_mask_lock;
  /* Model the peer writing the release command right before our 3rd
   * poll -- the hook runs on the driver's own spin thread via the
   * ra8_fake_mmio read-to-set model, so the loop's continuation branch
   * executes for real on the first two polls. */
  s_sem_hook_polls      = 0U;
  s_sem_hook_target     = sem;
  s_sem_hook_release_at = 3U;
  ra8_fake_mmio_set_poll_hook(hook_release_sem_at_nth_poll);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_take_timeout(5U, 8U));
  ra8_fake_mmio_set_poll_hook(nullptr);
  s_sem_hook_target = nullptr;
  /* Acquired on exactly the 3rd poll -- the first two spun. */
  TEST_ASSERT_EQ(3U, s_sem_hook_polls);
  /* The successful take latched LOCK again (HUM set condition). */
  TEST_ASSERT((*sem & (uint32_t)k_ra8_ipc_sem_mask_lock) != 0U);
  TEST_END("ipc sem take_timeout spins then acquires");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sem_take_timeout_succeeds(void)
{
  TEST_BEGIN("ipc sem take_timeout succeeds");
  prep();
  volatile uint32_t* sem = ra8_ipc_sem(k_ipc_sem_test_idx_b);
  *sem                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_take_timeout(7U, 16U));
  TEST_END("ipc sem take_timeout succeeds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sem_take_timeout_fails(void)
{
  TEST_BEGIN("ipc sem take_timeout fails");
  prep();
  /* Stage LOCK = 1 (a peer core owns it) and never release: every
   * seam-routed read observes 1 and re-latches it, so the bounded spin
   * runs its full budget and reports the timeout -- the same sequence
   * silicon produces under an unyielding owner. */
  volatile uint32_t* sem = ra8_ipc_sem(8U);
  *sem                   = (uint32_t)k_ra8_ipc_sem_mask_lock;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ipc_sem_take_timeout(8U, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_sem_take_timeout(8U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_sem_take_timeout((uint8_t)k_ra8_ipc_test_sem_bad, 4U));
  TEST_END("ipc sem take_timeout fails");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sem_is_locked(void)
{
  TEST_BEGIN("ipc sem is_locked");
  prep();
  volatile uint32_t* sem = ra8_ipc_sem(1U);
  *sem                   = 0U;
  bool locked            = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_is_locked(1U, &locked));
  TEST_ASSERT(locked == false);
  /* Side-effect: the read inside is_locked took the lock; the
   * function should have re-released it for unlocked, leaving LOCK=0
   * in our fake memory. */
  TEST_ASSERT(*sem == 0U);

  *sem = (uint32_t)k_ra8_ipc_sem_mask_lock;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_sem_is_locked(1U, &locked));
  TEST_ASSERT(locked == true);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_sem_is_locked(1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_sem_is_locked((uint8_t)k_ra8_ipc_test_sem_bad, &locked));
  TEST_END("ipc sem is_locked");
}

int32_t main(void)
{
  test_semaphore_accessor();
  test_sem_try_take_and_release();
  test_sem_take_timeout_succeeds();
  test_sem_take_timeout_spins_then_acquires();
  test_sem_take_timeout_fails();
  test_sem_is_locked();
  return 0;
}
