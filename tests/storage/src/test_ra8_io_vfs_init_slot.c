/**
 * @file test_ra8_io_vfs_init_slot.c
 * @brief MC/DC vectors for the ra8_io VFS mount-slot reset decision, split
 *        out of test_ra8_io_vfs.c to keep both files under the repository's
 *        per-file line cap.
 *
 * @details
 * Drives internal_vfs_init_slot()'s `slot->in_use && slot->owned` decision
 * directly through its RA8_TEST_HELPER wrapper, against a hand-built
 * ::vfs_slot_t and a minimal fixture format, rather than needing three
 * different mount call paths to reach every in_use/owned combination.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"
#include "ra8_io_vfs_internal.h"
#include "unity_minimal.h"

/** @brief Count of fixture-format unmount dispatches for the init_slot MC/DC test. */
static uint32_t s_init_slot_unmounts;

/** @brief Fixture unmount that only counts dispatches. @details Performs one bounded, deterministic operation for this host test. @param[in,out] mount_ctx Unused fixture context. @return Always k_ra8_ok. @retval k_ra8_ok The fixture never injects a failure. @pre None. @post s_init_slot_unmounts advances by exactly one. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_init_slot_unmount(void* mount_ctx)
{
  (void)mount_ctx;
  s_init_slot_unmounts++;
  return k_ra8_ok;
}

/** @brief Minimal ops table exposing only unmount, for the init_slot fixture. */
static const ra8_io_fsfmt_ops_t s_init_slot_ops = {.unmount = internal_init_slot_unmount};

/** @brief Minimal registered format for the init_slot fixture. */
static const ra8_io_fsfmt_t s_init_slot_fmt = {.name = "init_slot_fixture",
                                               .ops  = &s_init_slot_ops};

/**
 * @brief Drive every MC/DC vector of the mount-slot reset decision directly.
 * @details Builds a ::vfs_slot_t by hand for each vector so `in_use` and
 * `owned` vary independently without needing three different mount paths to
 * reach every combination.
 * @par MC/DC:
 * Decision: `if (slot->in_use && slot->owned)`
 * (2 conditions, libs/ra8_io/src/ra8_io_vfs.c@internal_vfs_init_slot)
 * - Vector 1: in_use=true,  owned=true  -> true  -> unmount dispatched once.
 * - Vector 2: in_use=false, owned=true  -> false -> unmount not dispatched.
 * - Vector 3: in_use=true,  owned=false -> false -> unmount not dispatched.
 * Vectors 1+2 flip the outcome varying in_use only; vectors 1+3 flip it
 * varying owned only. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @pre None; each vector builds its own slot.
 * @post Every vector's slot is zero-initialized on return.
 * @note Not thread-safe; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vfs_init_slot_mcdc(void)
{
  TEST_BEGIN("vfs init_slot MC/DC");

  vfs_slot_t slot      = {.format = &s_init_slot_fmt, .in_use = true, .owned = true};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(1U, s_init_slot_unmounts);
  TEST_ASSERT(!slot.in_use);

  slot                 = (vfs_slot_t){.format = &s_init_slot_fmt, .in_use = false, .owned = true};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(0U, s_init_slot_unmounts);

  slot                 = (vfs_slot_t){.format = &s_init_slot_fmt, .in_use = true, .owned = false};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(0U, s_init_slot_unmounts);

  TEST_END("vfs init_slot MC/DC");
}

int main(void)
{
  internal_test_vfs_init_slot_mcdc();
  return 0;
}
