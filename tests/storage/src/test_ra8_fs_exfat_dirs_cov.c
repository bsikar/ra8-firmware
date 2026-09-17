/**
 * @file test_ra8_fs_exfat_dirs_cov.c
 * @brief exFAT directory error arms, MC/DC vectors and the scanner's control.
 *
 * @details
 * The sibling of `test_ra8_fs_exfat_dirs.c`, which pins what WORKS. This one
 * pins what must not: every refusal the new verbs owe a caller, the rollback
 * that keeps a failed `mkdir` from leaking a cluster, the two shapes an exFAT
 * directory's cluster run comes in, and -- first, because nothing else here
 * means anything without it -- the proof that the structural scanner both
 * filesystem suites end every scenario with actually fires when the volume is
 * damaged.
 *
 * Faults are injected with the shared fixture's backend countdowns
 * (`s_mut_rd_fail_in` / `s_mut_wr_fail_in`), which place a read or write
 * failure at a precise point in a call sequence, and a hand-built two-cluster
 * contiguous directory reaches the walk arms a single-cluster `mkdir` cannot.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_exfat_dir_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum dcov_const_t
 * @brief Sizes, seeds and fault positions for the directory coverage suite.
 *
 * @details ::k_dcov_set_entries is what a short-named entry set costs -- File +
 *          Stream + one Name -- and is what the directory-full test divides a
 *          cluster's slots by to predict the exact number of `mkdir` calls that
 *          fit.
 *
 * @invariant k_dcov_over_cap exceeds the 64-character exFAT name cap.
 * @see test_mkdir_fills_a_directory()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dcov_payload     = 256U,  /**< Bytes in a helper payload.                      */
  k_dcov_seed        = 0x5AU, /**< Helper payload seed.                            */
  k_dcov_set_entries = 3U,    /**< Entries one short-named set occupies.           */
  k_dcov_over_cap    = 65U,   /**< One character past the exFAT name cap.          */
  k_dcov_path_depth  = 32U,   /**< k_exfat_path_depth: components a path may have. */
  k_dcov_one         = 1U,    /**< One entry / one cluster.                        */
  k_dcov_none        = 0U,    /**< No entries.                                     */
  k_dcov_two         = 2U,    /**< Two clusters.                                   */
  k_dcov_remnant     = 0x05U, /**< Type byte of a retired (in-use clear) entry.    */
  k_dcov_wr_zero     = 0,     /**< Fail the very next backend write.               */
  k_dcov_wr_after_9  = 9,     /**< Fail the tenth backend write.                   */
  k_dcov_path_cap    = 160U,  /**< Path buffer for the generated deep paths.       */
  k_dcov_rd_sweep    = 8U,    /**< Read faults swept over an rmdir's refusal.      */
  k_dcov_wr_at_mark  = 8,     /**< Write index of mkdir's bitmap-mark write.       */
} dcov_const_t;

/**
 * @brief Mount the fixture volume, asserting success.
 *
 * @return The mounted volume.
 * @retval non-NULL The mount handle.
 *
 * @pre `build_exfat_volume()` has run.
 * @pre No other mount is outstanding.
 * @post The returned handle is an exFAT mount.
 * @post No on-disk state is modified.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_mount_fixture(void)
{
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  return h;
}

/**
 * @brief Write a small constant-filled file at @p path, asserting success.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path File path.
 *
 * @pre @p h and @p path are non-NULL.
 * @pre The parent directory of @p path exists.
 * @post @p path resolves to a file of ::k_dcov_payload bytes.
 * @post The write succeeded (asserted).
 *
 * @since 0.1.0 @details Implements the bounded write small fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_write_small(ra8_fs_mount_t* h, const char* path)
{
  uint8_t buf[k_dcov_payload] = {};
  memset(buf, (int)k_dcov_seed, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, path, buf, (uint32_t)k_dcov_payload));
}

/* ---- the scanner's own negative control --------------------------------- */

/**
 * @test test_scanner_catches_orphaned_entry_set
 * @brief The structural scanner reports leaked space, and stays quiet without it.
 *
 * @details Every other scenario in both suites ends by asserting the volume is
 *          structurally sound, which proves nothing unless the scanner can
 *          fail. A file's entry set is retired BY HAND -- the in-use bit is
 *          cleared on its File entry, exactly as `unlink` would, but its
 *          clusters are deliberately left marked in the allocation bitmap. That
 *          is the leak `ra8_fs_write_file()` used to produce on a repeated
 *          create (#603) and the one a `rmdir` that forgot to free would produce
 *          now, and the scanner must call it an orphan. The clean reading taken
 *          first is the must-stay-quiet half.
 *
 * @par MC/DC:
 * No compound decision in the driver lies on this path -- the subject is the
 * TEST FIXTURE's scanner, not the filesystem. It drives the scanner's own
 * orphan verdict both ways: quiet on an intact volume, firing on one whose
 * entry set has been retired by hand with its bitmap bits left set.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The scan is clean before the damage and dirty after it.
 * @post The reported diagnostic names the orphan.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_scanner_catches_orphaned_entry_set(void)
{
  TEST_BEGIN("exfat dirs cov: the scanner catches a hand-orphaned entry set");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  internal_write_small(h, "/D/F.BIN");

  char msg[k_chk_msg_cap] = {};
  TEST_ASSERT_EQ(k_dcov_one, internal_exfat_scan(h, msg, (uint32_t)k_chk_msg_cap));

  /* Retire the "/D" entry set's File entry without freeing anything: its
   * cluster, and the cluster of the file inside it, stay marked used with
   * nothing referencing them. */
  const uint32_t file_off = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off] &= (uint8_t)~(uint8_t)k_chk_inuse_bit;

  TEST_ASSERT_EQ(k_dcov_none, internal_exfat_scan(h, msg, (uint32_t)k_chk_msg_cap));
  TEST_ASSERT_EQ(0, strncmp(msg, "a cluster is marked used", strlen("a cluster is marked used")));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: the scanner catches a hand-orphaned entry set");
}

/* ---- refusals ------------------------------------------------------------ */

/**
 * @test test_mkdir_argument_refusals
 * @brief `mkdir` refuses the root, an over-long leaf and a missing parent.
 *
 * @details Three distinct verdicts from one entry point, each with its own
 *          cause: a path that resolves to no leaf at all names the volume root,
 *          which already exists and is not creatable; a leaf past the
 *          64-character exFAT cap cannot be stored; and a parent that does not
 *          exist is a lookup failure, not an argument fault. The census
 *          brackets all three, because a refusal that had already allocated
 *          would show as a rising count.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `nlen == 0U` -> true (the volume root) and `nlen > k_exfat_name_cap` -> true
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_mkdir_check), plus the
 * not-found arm of the component lookup in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_enter.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Each call reports its own error code.
 * @post The census is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_argument_refusals(void)
{
  TEST_BEGIN("exfat dirs cov: mkdir argument refusals");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h      = internal_mount_fixture();
  const uint32_t  before = internal_alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, ""));

  char over[(uint32_t)k_dcov_over_cap + 2U] = {};
  over[0]                                   = '/';
  for (uint32_t i = 0U; i < (uint32_t)k_dcov_over_cap; i++) {
    over[1U + i] = 'X';
  }
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, over));

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mkdir(h, "/NOPE/CHILD"));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "mkdir_argument_refusals");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: mkdir argument refusals");
}

/**
 * @test test_resolution_refusals
 * @brief A file is never an intermediate component, and depth is bounded.
 *
 * @details `priv_exfat_enter` requires the Directory attribute, so a path that
 *          descends THROUGH a file is an argument fault rather than a
 *          not-found: the name exists, it is simply not a directory. The depth
 *          bound is the NASA Rule 2 guard on the resolver's loop -- it is
 *          iterative precisely so recursion cannot make the bound a stack
 *          overflow instead of an error code. Reaching it means building a tree
 *          32 levels deep, because a component that does not exist stops the
 *          walk long before the counter does.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `(attr & k_exfat_attr_directory) == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_enter) -> true, the
 * component-is-a-file arm, and the exhaustion of the bounded component loop in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_resolve_parent, which is
 * the NASA Power of 10 Rule 2 bound reporting itself rather than overrunning.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Descending through a file reports ::k_ra8_err_invalid_arg on every verb.
 * @post A 33-component path reports ::k_ra8_err_invalid_arg.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_resolution_refusals(void)
{
  TEST_BEGIN("exfat dirs cov: a file is not a directory, and depth is bounded");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  internal_write_small(h, "/PLAIN.BIN");

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/PLAIN.BIN/CHILD"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, "/PLAIN.BIN/CHILD"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_listdir(h, "/PLAIN.BIN", internal_count_cb, &(mut_list_ctx_t){}));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_stat(h, "/PLAIN.BIN/CHILD", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_unlink(h, "/PLAIN.BIN/CHILD"));

  /* The depth bound only bites when every parent RESOLVES -- a path of
   * components that do not exist stops at the first missing one and reports
   * not_found. So the tree is really built 32 levels deep first, and only the
   * 33rd component is refused. */
  char     deep[k_dcov_path_cap] = {};
  uint32_t at                    = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_dcov_path_depth; i++) {
    deep[at] = '/';
    at++;
    deep[at] = 'a';
    at++;
    deep[at] = '\0';
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, deep));
  }
  deep[at] = '/';
  at++;
  deep[at] = 'b';
  at++;
  deep[at] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, deep));

  internal_exfat_verify(h, "resolution_refusals");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a file is not a directory, and depth is bounded");
}

/**
 * @test test_rmdir_refusals
 * @brief `rmdir` refuses the root, a missing name and a cluster-less entry.
 *
 * @details The last of the three is the corrupt-volume arm: an entry set that
 *          claims the directory attribute but names no data cluster cannot be
 *          walked at all, and is reported as a protocol error rather than
 *          treated as an empty directory whose (non-existent) run would then be
 *          "freed". The entry is damaged by hand because no writer here
 *          produces one.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `priv_strlen(leaf) == 0U` -> true (the volume root) and
 * `first < k_cluster_first_data` -> true (a directory entry owning no cluster),
 * both in libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_rmdir_locate.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Each call reports its own error code.
 * @post No cluster is freed by any of them.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_refusals(void)
{
  TEST_BEGIN("exfat dirs cov: rmdir refusals");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, "/"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/GONE"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BROKEN"));
  const uint32_t before = internal_alloc_bitmap_used(h);
  /* Zero the Stream entry's FirstCluster: a directory that owns nothing. */
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  internal_disk_set_u32le(strm_off + (uint32_t)k_mut_strm_off_clus, 0U);

  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_fs_rmdir(h, "/BROKEN"));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_fs_mkdir(h, "/BROKEN/CHILD"));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: rmdir refusals");
}

/**
 * @test test_rename_refusals
 * @brief `rename` refuses the volume root on either side.
 *
 * @details A rename rewrites an entry set, and the volume root has none: it
 *          lives in the boot sector. Both directions are checked because the
 *          two leaves are validated separately, and a guard on only one of them
 *          would let the other through to write a zero-length name.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `priv_strlen(ol) == 0U` and `priv_strlen(nl) == 0U`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@internal_exfat_rename_prepare), each
 * driven to true independently -- a guard on only one side would let the other
 * through to write a zero-length name.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both calls report ::k_ra8_err_invalid_arg.
 * @post The file is still reachable under its original name.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rename_refusals(void)
{
  TEST_BEGIN("exfat dirs cov: rename refuses the volume root");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  internal_write_small(h, "/A.BIN");

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rename(h, "/A.BIN", "/"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rename(h, "/", "/B.BIN"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_unlink(h, "/"));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/A.BIN", &st));

  internal_exfat_verify(h, "rename_refusals");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: rename refuses the volume root");
}

/* ---- rollback and exhaustion -------------------------------------------- */

/**
 * @test test_mkdir_rolls_back_its_cluster
 * @brief A `mkdir` that fails after allocating gives the cluster back.
 *
 * @details Two failure points, on either side of the bitmap mark. Failing the
 *          FIRST write reaches the cluster while it is still being zeroed, so
 *          nothing has been marked and there is nothing to reclaim. Failing a
 *          later write reaches the entry-set write, AFTER the bitmap bit is
 *          set -- and that is the one the rollback exists for: without it the
 *          cluster would stay marked used with no entry pointing at it, which
 *          is precisely the orphan the structural scanner reports. The census
 *          is identical before and after in both cases.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `e != k_ra8_ok` after ::priv_exfat_zero_cluster -> true (nothing marked yet)
 * and after ::priv_exfat_write_dir_set -> true (the arm that clears the bitmap
 * bit again), both in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_mkdir.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both attempts report a backend error.
 * @post The census is unchanged and the scan is clean.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_rolls_back_its_cluster(void)
{
  TEST_BEGIN("exfat dirs cov: a failed mkdir leaks no cluster");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h      = internal_mount_fixture();
  const uint32_t  before = internal_alloc_bitmap_used(h);

  s_mut_wr_fail_in = (int32_t)k_dcov_wr_zero;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mkdir(h, "/EARLY"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/EARLY", &(ra8_fs_stat_t){}));
  internal_exfat_verify(h, "mkdir_rollback_early");

  /* Past the cluster zeroing (one write per sector) and the bitmap mark, into
   * the entry-set write, where the cluster is already marked used. */
  s_mut_wr_fail_in = (int32_t)k_dcov_wr_after_9;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mkdir(h, "/LATE"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/LATE", &(ra8_fs_stat_t){}));

  internal_exfat_verify(h, "mkdir_rollback_late");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a failed mkdir leaks no cluster");
}

/**
 * @test test_mkdir_read_failure_propagates
 * @brief A backend read failure during resolution is reported, not swallowed.
 *
 * @details The resolver's very first act is to scan the parent for the leaf, so
 *          failing the next read reaches it before anything is allocated. A
 *          driver that treated a read failure as "not found" would then create
 *          a second entry for a name that already exists.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `fe != k_ra8_err_not_found`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_mkdir_check) -> true: the
 * arm that separates "the backend failed" from "the name is absent", and
 * without which a read failure would read as a green light to create.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The call reports the backend's own error code.
 * @post The census is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_read_failure_propagates(void)
{
  TEST_BEGIN("exfat dirs cov: a read failure during mkdir propagates");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h      = internal_mount_fixture();
  const uint32_t  before = internal_alloc_bitmap_used(h);

  s_mut_rd_fail_in = (int32_t)k_dcov_wr_zero;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mkdir(h, "/X"));
  s_mut_rd_fail_in = (int32_t)k_mut_fault_never;

  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  internal_exfat_verify(h, "mkdir_read_failure");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a read failure during mkdir propagates");
}

/**
 * @test test_mkdir_fills_a_directory
 * @brief A directory filled past one cluster GROWS through `mkdir` (#677).
 *
 * @details A directory is born owning one cluster, whose capacity is the entry
 *          slots it holds divided by the three a short-named set occupies. This
 *          creates that many subdirectories -- exactly filling the first cluster
 *          -- and then several MORE, each of which used to report
 *          ::k_ra8_err_no_mem and now succeeds by extending the parent. The
 *          listing is asserted to hold every child, so a grown cluster whose
 *          entries a walk could not reach would show up as a short count.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `run >= need` (libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@internal_exfat_space_in_cluster)
 * -> false for every slot of a full cluster, and `fe == k_ra8_err_not_found`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@internal_exfat_scan_dir_space) -> true,
 * the end of a contiguous run that now drives a grow-and-retry rather than a
 * refusal.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Every child, past the single-cluster ceiling, exists and is listed.
 * @post The volume passes the structural scan.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_fills_a_directory(void)
{
  TEST_BEGIN("exfat dirs cov: a directory grows past one cluster via mkdir");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/FULL"));
  const uint32_t fits  = internal_entries_per_cluster(h) / (uint32_t)k_dcov_set_entries;
  const uint32_t total = fits + (uint32_t)k_dcov_two;
  for (uint32_t i = 0U; i < total; i++) {
    char path[k_dcov_path_cap] = {};
    (void)snprintf(path, sizeof(path), "/FULL/D%u", i);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, path));
  }

  mut_list_ctx_t counted = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/FULL", internal_count_cb, &counted));
  TEST_ASSERT_EQ(total, counted.count);

  internal_exfat_verify(h, "mkdir_grows_a_directory");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a directory grows past one cluster via mkdir");
}

/* ---- the two shapes of a directory run ---------------------------------- */

/**
 * @brief Build a two-cluster contiguous "directory" out of a written file.
 *
 * @details `mkdir` makes one-cluster directories, so a multi-cluster NoFatChain
 *          run has to be presented another way: a file of exactly two clusters
 *          is written -- which the driver allocates contiguously and flags
 *          NoFatChain -- its bytes are patched to entry types, and the
 *          Directory attribute is set on its File entry. Nothing in the lookup
 *          path verifies the SetChecksum, so the patched set resolves exactly
 *          like a real one, which is the same technique #604's guards were
 *          proved with.
 *
 *          @p tail_type is the type byte written at the start of the SECOND
 *          cluster: an end-of-directory marker stops the walk there (so the
 *          step into the second cluster is what makes the walk terminate), and a
 *          retired-entry byte makes the walk run the whole run out instead.
 *
 * @param[in] h         Mounted exFAT volume.
 * @param[in] tail_type Type byte for the second cluster's first entry.
 *
 * @pre @p h is non-NULL and mounted; the root holds no user entry yet.
 * @pre The volume has two contiguous free clusters.
 * @post The root's first user entry set is a two-cluster directory.
 * @post Its checksum is stale, which no lookup path consults.
 *
 * @since 0.1.0 @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_build_two_cluster_dir(ra8_fs_mount_t* h, uint8_t tail_type)
{
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t len    = cbytes * (uint32_t)k_dcov_two;
  uint8_t*       buf    = (uint8_t*)malloc((size_t)len);
  if (buf == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for the two-cluster payload");
    return;
  }
  memset(buf, 0, (size_t)len);
  /* Every 32-byte slot of the first cluster is a retired entry: not
   * end-of-directory, not in use, so the walk must keep going. */
  for (uint32_t off = 0U; off < cbytes; off += (uint32_t)k_mut_entry_bytes) {
    buf[off] = (uint8_t)k_dcov_remnant;
  }
  for (uint32_t off = cbytes; off < len; off += (uint32_t)k_mut_entry_bytes) {
    buf[off] = tail_type;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BIG", buf, len));
  free(buf);
  internal_mark_first_file_as_directory(h);
}

/**
 * @test test_contiguous_run_walks_into_its_second_cluster
 * @brief A NoFatChain directory's walk steps to the adjacent cluster.
 *
 * @details The MC/DC pair for the cluster-advance decision. A one-cluster
 *          directory only ever exercises "the run ends here"; this is the other
 *          arm -- the first cluster is full of retired entries, so the walk has
 *          to step into the second to find the end-of-directory marker that
 *          terminates it. Following the FAT instead would read a free entry (0),
 *          not recognise it as end-of-chain, and walk into cluster 0.
 *
 * @par MC/DC:
 * Decision: `contig_end != 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_step_cluster, 1 condition).
 * - V1: a NoFatChain directory -> true  -> step to cluster + 1 (this test).
 * - V2: the FAT-chained root   -> false -> follow the FAT (every other test).
 * Decision: `adjacent >= contig_end` (1 condition), on V1's arm only.
 * - V3: two-cluster run, first cluster exhausted -> false -> cluster + 1 (here).
 * - V4: one-cluster directory, cluster exhausted -> true  -> the run ends
 *       (test_rmdir_walks_a_run_to_its_end, and every full-directory scan).
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The listing terminates and reports no entries.
 * @post `rmdir` removes the directory and frees both clusters.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_contiguous_run_walks_into_its_second_cluster(void)
{
  TEST_BEGIN("exfat dirs cov: a contiguous run walks into its second cluster");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);

  internal_build_two_cluster_dir(h, (uint8_t)k_chk_type_eod);
  TEST_ASSERT_EQ(baseline + (uint32_t)k_dcov_two, internal_alloc_bitmap_used(h));

  mut_list_ctx_t counted = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/BIG", internal_count_cb, &counted));
  TEST_ASSERT_EQ(k_dcov_none, counted.count);

  /* Retired entries are not contents, so the directory is removable, and both
   * clusters of the run come back. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/BIG"));
  TEST_ASSERT_EQ(baseline, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "contiguous_run_second_cluster");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a contiguous run walks into its second cluster");
}

/**
 * @test test_rmdir_walks_a_run_to_its_end
 * @brief A directory whose run ends without a marker is still empty.
 *
 * @details The other termination: every slot of both clusters is a retired
 *          entry, so the walk never meets an end-of-directory marker and instead
 *          runs the contiguous run out. A driver that treated "the run ended" as
 *          an error would make such a directory permanently un-removable; a
 *          driver that treated it as "not empty" would too.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `r == k_ra8_err_not_found`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_dir_is_empty) -> true:
 * the run ended without an end-of-directory marker, which is empty and not an
 * error. Every other scenario takes its false arm.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The listing completes and reports no entries.
 * @post `rmdir` removes the directory and frees both clusters.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_walks_a_run_to_its_end(void)
{
  TEST_BEGIN("exfat dirs cov: a run that ends without a marker is empty");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);

  internal_build_two_cluster_dir(h, (uint8_t)k_dcov_remnant);

  mut_list_ctx_t counted = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/BIG", internal_count_cb, &counted));
  TEST_ASSERT_EQ(k_dcov_none, counted.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/BIG"));
  TEST_ASSERT_EQ(baseline, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "rmdir_run_to_its_end");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a run that ends without a marker is empty");
}

/**
 * @test test_trailing_slash_names_the_directory
 * @brief A trailing slash resolves to the directory, not to an empty leaf.
 *
 * @details `"/D/"` and `"/D"` name the same thing to a listing, and neither
 *          names something creatable: `mkdir("/D/")` has no leaf to create and
 *          is refused. Runs of slashes are skipped for the same reason, so
 *          `"//D//X"` resolves like `"/D/X"` -- a caller assembling paths by
 *          concatenation gets the obvious answer instead of a lookup failure.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `len == 0U`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_resolve_dir) -> true, the
 * trailing-slash arm that names the parent, against the false arm every other
 * listing takes.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post `"/D/"` lists the same entries as `"/D"`.
 * @post A doubled separator resolves identically.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_trailing_slash_names_the_directory(void)
{
  TEST_BEGIN("exfat dirs cov: trailing and doubled slashes resolve");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  internal_write_small(h, "/D/F.BIN");

  name_ctx_t plain = {};
  name_ctx_t slash = {};
  TEST_ASSERT_EQ(k_dcov_one, internal_list_names(h, "/D", &plain));
  TEST_ASSERT_EQ(k_dcov_one, internal_list_names(h, "/D/", &slash));
  TEST_ASSERT_EQ(0, strcmp(plain.names[0], slash.names[0]));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/D/"));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "//D//F.BIN", &st));
  TEST_ASSERT_EQ(false, st.is_directory);

  internal_exfat_verify(h, "trailing_slash_names_the_directory");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: trailing and doubled slashes resolve");
}

/**
 * @test test_lookup_and_resolve_edges
 * @brief The three edges of resolution that no round-trip reaches.
 *
 * @details `open("/")` asks for the volume root as a FILE: the root has no
 *          entry set, so the lookup refuses rather than inventing one. A listing
 *          of a path whose PARENT is missing has to report the parent's failure
 *          rather than its own. And a component longer than the 64-character
 *          exFAT name cap cannot be stored, so it cannot be matched either --
 *          checked before the scan rather than truncated into a name that would
 *          match the wrong entry.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `priv_strlen(leaf) == 0U`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_lookup) -> true, and
 * `len > k_exfat_name_cap`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_enter) -> true, which the
 * leaf-length guard can never reach because it never sees an intermediate
 * component.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Each call reports its own error code.
 * @post No volume state is modified.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_lookup_and_resolve_edges(void)
{
  TEST_BEGIN("exfat dirs cov: resolution edges");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_fs_listdir(h, "/NOPE/SUB", internal_count_cb, &(mut_list_ctx_t){}));

  /* An over-long INTERMEDIATE component, which the leaf-length guard never
   * sees: the resolver refuses it while descending. */
  char over[k_dcov_path_cap] = {};
  over[0]                    = '/';
  uint32_t at                = 1U;
  for (uint32_t i = 0U; i < (uint32_t)k_dcov_over_cap; i++) {
    over[at] = 'X';
    at++;
  }
  over[at] = '/';
  at++;
  over[at] = 'C';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, over));

  internal_exfat_verify(h, "lookup_and_resolve_edges");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: resolution edges");
}

/**
 * @test test_rmdir_read_faults_cost_nothing
 * @brief No read failure during a `rmdir` can lose the directory.
 *
 * @details A property, swept rather than pinned to one position: for every one
 *          of the first ::k_dcov_rd_sweep backend reads, failing THAT read must
 *          leave the directory exactly where it was. Those reads are the
 *          entry-set search and the emptiness walk, both of which run before
 *          anything is written -- so a driver that acted on a partial answer,
 *          or treated an unreadable directory as an empty one, would free a
 *          cluster here and the census would say so.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `r != k_ra8_ok`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_dir_is_empty) -> true,
 * swept across every read the refusal path makes, so an unreadable directory
 * can never be mistaken for an empty one.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Every swept position reports the backend's error.
 * @post The directory still resolves and the census is unchanged, every time.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_read_faults_cost_nothing(void)
{
  TEST_BEGIN("exfat dirs cov: a read fault during rmdir costs nothing");
  for (uint32_t n = 0U; n < (uint32_t)k_dcov_rd_sweep; n++) {
    internal_build_exfat_volume();
    ra8_fs_mount_t* h = internal_mount_fixture();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
    const uint32_t before = internal_alloc_bitmap_used(h);

    s_mut_rd_fail_in = (int32_t)n;
    TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_rmdir(h, "/D"));
    s_mut_rd_fail_in = (int32_t)k_mut_fault_never;

    ra8_fs_stat_t st = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/D", &st));
    TEST_ASSERT_EQ(true, st.is_directory);
    TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
    internal_exfat_check(h, "rmdir_read_fault_sweep");

    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
    internal_free_volume();
  }
  TEST_END("exfat dirs cov: a read fault during rmdir costs nothing");
}

/**
 * @test test_rmdir_write_fault_keeps_the_directory
 * @brief A `rmdir` that cannot retire the set frees nothing.
 *
 * @details The order matters and this is what pins it. Entries are retired
 *          BEFORE clusters are freed, so a failure at the very first write
 *          leaves a live entry set pointing at clusters that are still marked
 *          used -- a volume that has lost nothing. The reverse order would
 *          leave a live entry set pointing at free space, which no host `fsck`
 *          can repair without guessing.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `e != k_ra8_ok` after ::priv_exfat_drop_set
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_rmdir) -> true: the set
 * could not be retired, so the clusters behind it are not freed either.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The call reports the backend's error.
 * @post The directory still resolves and the census is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_write_fault_keeps_the_directory(void)
{
  TEST_BEGIN("exfat dirs cov: a write fault during rmdir frees nothing");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = internal_mount_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  const uint32_t before = internal_alloc_bitmap_used(h);

  s_mut_wr_fail_in = (int32_t)k_dcov_wr_zero;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_rmdir(h, "/D"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/D", &st));
  TEST_ASSERT_EQ(true, st.is_directory);
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "rmdir_write_fault");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: a write fault during rmdir frees nothing");
}

/**
 * @test test_mkdir_allocation_failures
 * @brief `mkdir` reports an unusable bitmap, and a bitmap it cannot mark.
 *
 * @details Two failures inside the allocation step, on either side of the write
 *          that makes the cluster real. With the allocation-bitmap entry hidden
 *          -- its type byte changed, as a corrupt volume would present it --
 *          there is nowhere to allocate FROM, and the scan's failure has to
 *          reach the caller rather than being read as "no space". Failing the
 *          bitmap-mark write instead leaves a zeroed cluster nobody has claimed,
 *          which costs the volume nothing.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `e != k_ra8_ok` after ::priv_exfat_alloc_scan -> true (no allocation bitmap
 * to scan) and after ::priv_exfat_bitmap_mark -> true (the bit could not be
 * set), both in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@internal_exfat_dir_alloc.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both attempts report an error and create nothing.
 * @post The census is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_allocation_failures(void)
{
  TEST_BEGIN("exfat dirs cov: mkdir allocation failures");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h      = internal_mount_fixture();
  const uint32_t  before = internal_alloc_bitmap_used(h);

  s_mut_wr_fail_in = (int32_t)k_dcov_wr_at_mark;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mkdir(h, "/MARK"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/MARK", &(ra8_fs_stat_t){}));
  internal_exfat_verify(h, "mkdir_bitmap_mark_failure");

  /* Hide the allocation-bitmap entry: the scan has nowhere to start. */
  const uint32_t bmp_entry = internal_root_byte(h, (uint32_t)k_mut_root_bitmap_idx);
  s_disk.bytes[bmp_entry]  = (uint8_t)k_mut_type_bogus;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mkdir(h, "/NOBMP"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat dirs cov: mkdir allocation failures");
}

/**
 * @brief Run every exFAT directory coverage test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts before returning).
 *
 * @pre The host provides malloc for the 64 MiB volume.
 * @pre No other test binary shares this process.
 * @post Every fixture volume is freed.
 * @post The pass banner has been printed.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_scanner_catches_orphaned_entry_set();
  internal_test_mkdir_argument_refusals();
  internal_test_resolution_refusals();
  internal_test_rmdir_refusals();
  internal_test_rename_refusals();
  internal_test_mkdir_rolls_back_its_cluster();
  internal_test_mkdir_read_failure_propagates();
  internal_test_mkdir_fills_a_directory();
  internal_test_contiguous_run_walks_into_its_second_cluster();
  internal_test_rmdir_walks_a_run_to_its_end();
  internal_test_trailing_slash_names_the_directory();
  internal_test_lookup_and_resolve_edges();
  internal_test_rmdir_read_faults_cost_nothing();
  internal_test_rmdir_write_fault_keeps_the_directory();
  internal_test_mkdir_allocation_failures();
  return 0;
}
