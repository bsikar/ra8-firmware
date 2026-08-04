/**
 * @file test_ra8_fs_alloc_state.c
 * @brief The allocator state's "no bound slot" fallbacks (#607).
 *
 * @details
 * `ra8_fs_fat_alloc_internal.h` promises that every accessor is TOTAL: a mount
 * with no state slot behaves exactly like the code before #607 -- the hint is
 * cluster 2, the free count is unknown, and nothing is written back -- so a
 * missing binding degrades performance and never correctness.
 *
 * Nothing in the public API can produce that state: `ra8_fs_mount()` binds a
 * slot before anything else runs, and a static assertion pins one state slot
 * per mount slot so the bind cannot fail. The promise is therefore checked by
 * calling the accessors directly with a stack-allocated mount that was never
 * handed to `ra8_fs_mount()` -- the internal-symbol test access `docs/MCDC.md`
 * blesses for exactly this: a validation path no public call can reach.
 *
 * It needs its own executable because reaching those symbols means including
 * `ra8_fs_fat_internal.h`, whose on-disk-layout enums share their names with
 * the ones in `tests/support/fs_fat_dir_test_util.h`. No disk is needed here
 * at all, so the two never have to meet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum as_const_t
 * @brief The values pushed at an unbound mount, and what it must answer.
 *
 * @invariant `k_as_expect_hint` is ::k_cluster_first_data, the pre-#607 scan start.
 * @see test_unbound_mount_degrades_gracefully()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_as_expect_hint = 2U,    /**< The answer an unbound mount must always give. */
  k_as_push_hint   = 4096U, /**< A hint someone tries to set on it.            */
  k_as_push_low    = 7U,    /**< A cluster someone tries to release to it.     */
  k_as_clusters    = 8192U, /**< Plausible geometry for the orphan mount.      */
} as_const_t;

/**
 * @test test_unbound_mount_degrades_gracefully
 * @brief Every allocator accessor is total: a mount with no state slot gets
 *        the pre-#607 behaviour instead of a crash or a stale answer.
 *
 * @details The header promises "a missing binding degrades performance and
 *          never correctness". Nothing in the public API can produce an
 *          unbound mount, so the promise is checked by calling the accessors
 *          directly with a stack-allocated mount that was never handed to
 *          `ra8_fs_mount()`.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- each accessor's `st == nullptr` guard
 * is a single condition, driven false everywhere else in the suite and true
 * here)
 *
 * @since 0.1.0
 */
static void test_unbound_mount_degrades_gracefully(void)
{
  TEST_BEGIN("fs alloc: an unbound mount gets the pre-#607 behaviour");
  ra8_fs_mount_t orphan    = {};
  orphan.count_of_clusters = (uint32_t)k_as_clusters;

  /* The hint answer is the first data cluster, whatever anyone tries to set. */
  TEST_ASSERT_EQ(k_as_expect_hint, priv_alloc_hint_get(&orphan));
  priv_alloc_hint_set(&orphan, (uint32_t)k_as_push_hint);
  TEST_ASSERT_EQ(k_as_expect_hint, priv_alloc_hint_get(&orphan));
  priv_alloc_hint_lower(&orphan, (uint32_t)k_as_push_low);
  TEST_ASSERT_EQ(k_as_expect_hint, priv_alloc_hint_get(&orphan));

  /* Accounting and writeback are no-ops rather than faults. */
  priv_free_count_took(&orphan, 1U);
  priv_free_count_gave(&orphan, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, priv_fsinfo_seed(&orphan));
  TEST_ASSERT_EQ(k_ra8_ok, priv_fsinfo_flush(&orphan));

  /* Releasing a mount that owns nothing is equally harmless. */
  priv_alloc_state_release(&orphan);
  TEST_ASSERT_EQ(k_as_expect_hint, priv_alloc_hint_get(&orphan));
  TEST_END("fs alloc: an unbound mount gets the pre-#607 behaviour");
}

/**
 * @brief Run every case in this file.
 *
 * @return Process exit status.
 * @retval 0 Every case passed.
 *
 * @pre No other test binary shares this process.
 * @pre No volume is mounted (this file never mounts one).
 * @post No allocator state slot has been bound.
 * @post No backend call was made.
 *
 * @note Single-threaded by construction.
 * @since 0.1.0
 */
int main(void)
{
  test_unbound_mount_degrades_gracefully();
  printf("[OK  ] test_ra8_fs_alloc_state.c\n");
  return 0;
}
