/**
 * @file tests/test_ra8_fs_fat_exfat_write_dir_cov.c
 * @brief Coverage-boost tests for the exFAT create + directory write paths.
 *
 * @details
 * Split sibling of test_ra8_fs_fat_exfat_write_bmp_cov.c targeting the
 * create / directory half of `ra8_fs_fat_exfat_write.c`: the
 * priv_exfat_create argument guards (empty path, over-long name, zero
 * length), the priv_exfat_write_data and read-entry failure arms, the
 * priv_exfat_find_dir_space full-root / FAT-walk / chained-cluster arms, and
 * the priv_exfat_write_dir_set read and write failure arms. Fault positions
 * are injected with the countdown backend from
 * tests/support/fs_fat_exfat_write_test_util.h (see that header for the full
 * R/W call-sequence map).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "support/fs_fat_exfat_write_test_util.h"
#include "unity_minimal.h"

/* ---- tests ----------------------------------------------------------------- */

/**
 * @test test_create_empty_path
 * @brief `priv_exfat_create` rejects an empty path (line 548).
 *
 * @details Calling `ra8_fs_write_file` with "/" as the path causes
 *          `priv_exfat_create` to strip the leading slash, leaving an
 *          empty name of length 0, and return `k_ra8_err_invalid_arg`.
 *
 * Lines targeted: 548.
 *
 * @par MC/DC:
 * Decision: `if (nlen == 0U)` -- 1 condition.
 * V1: path "/" -> stripped to "" -> nlen=0 -> T -> invalid_arg (this test).
 * V2: path "X.TXT" -> nlen=5 -> F -> proceeds (all success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_invalid_arg.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_create_empty_path(void)
{
  TEST_BEGIN("exfat write cov: empty path -> invalid_arg (line 548)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, "/", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: empty path -> invalid_arg (line 548)");
}

/**
 * @test test_create_path_too_long
 * @brief `priv_exfat_create` rejects a name longer than 64 characters (line 551).
 *
 * @details A 65-character name exceeds `k_exfat_name_cap` (64), so
 *          `priv_exfat_create` returns `k_ra8_err_invalid_arg` at line 551.
 *
 * Lines targeted: 551.
 *
 * @par MC/DC:
 * Decision: `if (nlen > k_exfat_name_cap)` -- 1 condition.
 * V1: nlen=65 > 64 -> T -> invalid_arg (this test).
 * V2: nlen=5 <= 64 -> F -> proceeds (success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_invalid_arg.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_create_path_too_long(void)
{
  TEST_BEGIN("exfat write cov: 65-char name -> invalid_arg (line 551)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Exactly 65 'A' characters: one over the 64-char limit. */
  static const char k_long_name[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, k_long_name, &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: 65-char name -> invalid_arg (line 551)");
}

/**
 * @test test_create_zero_len
 * @brief A zero-byte `ra8_fs_write_file` creates an empty file on exFAT too.
 *
 * @details This case used to assert `k_ra8_err_invalid_arg`, because the exFAT
 *          whole-file creator refused `len == 0` while the FAT path accepted
 *          it -- the same call meaning two different things depending on a
 *          volume format the caller was supposed to be abstracted from. With
 *          exFAT streaming (#602) there is one path for both, so a zero-length
 *          create now does on exFAT exactly what it always did on FAT.
 *
 * @par MC/DC:
 * Decision: `if (len == 0U)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@internal_write_locked` -- 1 condition.
 * V1: len=0 -> T -> the write is a no-op and the created file stays empty
 *     (this test).
 * V2: len=1 -> F -> bytes are streamed (all the success-path cases).
 *
 * @pre Volume is formatted and accessible.
 * @post `X.TXT` exists with a size of 0.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_create_zero_len(void)
{
  TEST_BEGIN("exfat write cov: len=0 creates an empty file");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "X.TXT", &dummy, 0U));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "X.TXT", &st));
  TEST_ASSERT_EQ(0U, st.size_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: len=0 creates an empty file");
}

/**
 * @test test_write_data_fail
 * @brief Write failure on W1 propagates through `priv_exfat_write_data`
 *        (lines 240, 494, 561).
 *
 * @details R1 (find_bitmap) and R2 (bitmap_scan) succeed; W1 is the
 *          `priv_write_sector` inside `priv_exfat_write_data`.  Failing it
 *          exercises line 240 in write_data, line 494 in alloc_write, and
 *          line 561 in create.
 *
 * Lines targeted: 240, 494, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_data -- 1 condition.
 * V1: e=error -> T -> early return (this test).
 * V2: e=k_ra8_ok -> F -> continue sector loop (normal write path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_write_data_fail(void)
{
  TEST_BEGIN("exfat write cov: W1 write fail in write_data (lines 240,494,561)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_wr_remaining = (int32_t)k_wc_wr_at_w1;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: W1 write fail in write_data (lines 240,494,561)");
}

/**
 * @test test_read_entry_fail
 * @brief Read failure on R4 (first `priv_exfat_read_entry` in
 *        `priv_exfat_find_dir_space`) propagates (lines 273, 341, 531).
 *
 * @details R1-R3 succeed (find_bitmap, bitmap_scan, bmp_switch); W1-W2
 *          succeed (write_data, bitmap_mark); R4 is the first
 *          `priv_read_sector` inside `priv_exfat_read_entry` for entry 0 of
 *          the root cluster.  Failing it exercises line 273 in read_entry,
 *          line 341 in find_dir_space, and line 531 in link.
 *
 * Lines targeted: 273, 341, 531.
 *
 * @par MC/DC:
 * Decision: `if (r != k_ra8_ok)` in find_dir_space -- 1 condition.
 * V1: r=error -> T -> return r (this test).
 * V2: r=k_ra8_ok -> F -> examine slot (normal scan path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_read_entry_fail(void)
{
  TEST_BEGIN("exfat write cov: R4 read fail -> lines 273,341,531");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r4;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: R4 read fail -> lines 273,341,531");
}

/**
 * @test test_dir_space_full_root_eoc
 * @brief A full root reaches the FAT-chain end and GROWS rather than refusing.
 *
 * @details Patches root entries 3-127 to type 0x85 (in-use). Entries 0-2 are
 *          system entries (also in-use); entries 16-127 are already 0xA5 (bit 7
 *          set) from the pre-fill. With all 128 entries in-use,
 *          `priv_exfat_scan_dir_space` scans the whole cluster, follows the root
 *          through `priv_fat_get`, and finds it end-of-chain -- the arm that
 *          drives ::priv_exfat_find_dir_space to GROW the root (#677). Before
 *          #677 that arm returned `k_ra8_err_no_mem`; now the root's FAT chain is
 *          extended by a fresh cluster and the write succeeds. The root here is
 *          deliberately corrupted with bare in-use bytes, so the file is not read
 *          back; the success of the write is the whole assertion.
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_step_cluster) -- 1 condition.
 * V1: next=EOC -> T -> the run has no next cluster, so the directory grows (this
 * test). V2: next=valid_cluster -> F -> follow chain (test_dir_space_chain).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_ok, having grown the root.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_dir_space_full_root_eoc(void)
{
  TEST_BEGIN("exfat write cov: a full root grows on the FAT-EOC arm");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  internal_patch_root_full(h, (uint32_t)k_wc_patch_start);

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: a full root grows on the FAT-EOC arm");
}

/**
 * @test test_dir_space_fat_get_fail
 * @brief Read failure on R132 causes `priv_fat_get` to fail inside
 *        `priv_exfat_find_dir_space` (line 359).
 *
 * @details With entries 3-127 patched to 0x85 (in-use), the inner for loop
 *          runs all 128 iterations (R4-R131).  R132 is the `priv_read_sector`
 *          inside `priv_fat_get` (lines 356-357).  Failing it makes `fe !=
 *          k_ra8_ok` true and exercises line 359.
 *
 * Read sequence (3 alloc_write + 128 find_dir_space inner loop = 131
 * successes; R132 = fat_get):
 *   R1  find_bitmap, R2  bitmap_scan, R3  bmp_switch,
 *   R4-R131 read_entry(cluster, 0..127),
 *   R132 priv_fat_get -> FAIL.
 *
 * Lines targeted: 359.
 *
 * @par MC/DC:
 * Decision: `if (fe != k_ra8_ok)` in find_dir_space FAT chain -- 1 condition.
 * V1: fe=error -> T -> return fe (this test).
 * V2: fe=k_ra8_ok -> F -> is_eoc check (test_dir_space_full_root_eoc).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_dir_space_fat_get_fail(void)
{
  TEST_BEGIN("exfat write cov: R132 fat_get fail -> line 359");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  internal_patch_root_full(h, (uint32_t)k_wc_patch_start);
  s_rd_remaining = (int32_t)k_wc_rd_fat_get;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: R132 fat_get fail -> line 359");
}

/**
 * @test test_dir_space_chain
 * @brief `priv_exfat_find_dir_space` follows a FAT chain to a second cluster
 *        that has free entries (lines 364, 365).
 *
 * @details Patches:
 *   - Root entries 3-127 to 0x85 (all in-use).
 *   - FAT[root_cluster] = cluster_g (root_cluster + 100), so the FAT chain extends.
 *   - FAT[cluster_g] = EOC (0xFFFFFFFF) to terminate the chain at cluster_g.
 *   - All sectors of cluster cluster_g zeroed so entries appear as EOD (free).
 *
 * `priv_exfat_find_dir_space` scans the root cluster (no free run), calls
 * `priv_fat_get` (succeeds, returns cluster_g, not EOC), sets `cluster=cluster_g` at line 364
 * and increments `guard` at line 365.  It then scans cluster_g and finds the first
 * 3 entries free, returning k_ra8_ok.  The file is created successfully.
 *
 * Lines targeted: 364, 365.
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 condition.
 * V1: next=EOC -> T -> no_mem (test_dir_space_full_root_eoc).
 * V2: next=cluster_g (not EOC) -> F -> cluster=cluster_g, guard++ (this test).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_dir_space_chain(void)
{
  TEST_BEGIN("exfat write cov: FAT chain to cluster cluster_g -> lines 364,365");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  internal_patch_root_full(h, (uint32_t)k_wc_patch_start);

  const uint32_t cluster_g = h->root_cluster + (uint32_t)k_wc_chain_offset;
  /* first_data_lba is partition-relative; s_disk.bytes is indexed absolutely. */
  const uint32_t cluster_g_lba = h->partition_base_lba + h->first_data_lba +
                                 ((uint64_t)(cluster_g - 2U) * h->sectors_per_cluster);

  /* Zero cluster cluster_g so all entries appear as EOD (free). */
  for (uint32_t s = 0U; s < h->sectors_per_cluster; s++) {
    memset(&s_disk.bytes[(size_t)(cluster_g_lba + s) * (uint32_t)k_wc_block_size],
           0x00,
           (uint32_t)k_wc_block_size);
  }

  /* Build FAT chain: root_cluster -> cluster_g -> EOC. */
  internal_disk_set_u32le(internal_fat_entry_off(h, h->root_cluster), cluster_g);
  internal_disk_set_u32le(internal_fat_entry_off(h, cluster_g), (uint32_t)k_wc_fat_eoc);

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: FAT chain to cluster cluster_g -> lines 364,365");
}

/**
 * @test test_write_dir_set_read_fail
 * @brief Read failure on R10 (first read inside `priv_exfat_write_dir_set`)
 *        propagates (line 441).
 *
 * @details R1-R9 succeed (3 in alloc_write + 6 in find_dir_space for entries
 *          0-5 of the root cluster); W1-W2 succeed (write_data, bitmap_mark).
 *          R10 is the `priv_read_sector` inside `priv_exfat_write_dir_set`
 *          for the first directory entry (index 3).  Failing it exercises
 *          line 441.
 *
 * Lines targeted: 441.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_dir_set read -- 1 condition.
 * V1: e=error -> T -> return e (this test).
 * V2: e=k_ra8_ok -> F -> patch and write (normal write_dir_set path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_write_dir_set_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R10 write_dir_set read fail -> line 441");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r10;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: R10 write_dir_set read fail -> line 441");
}

/**
 * @test test_write_dir_set_write_fail
 * @brief Write failure on W3 (first write inside `priv_exfat_write_dir_set`)
 *        propagates (line 448).
 *
 * @details W1 (write_data) and W2 (bitmap_mark) succeed; W3 is the
 *          `priv_write_sector` inside `priv_exfat_write_dir_set` for the
 *          patched sector of directory entry 3.  Failing it exercises line 448.
 *
 * Lines targeted: 448.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_dir_set write -- 1 condition.
 * V1: e=error -> T -> return e (this test).
 * V2: e=k_ra8_ok -> F -> next entry (normal write_dir_set path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_write_dir_set_write_fail(void)
{
  TEST_BEGIN("exfat write cov: W3 write_dir_set write fail -> line 448");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_wr_remaining = (int32_t)k_wc_wr_at_w3;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write cov: W3 write_dir_set write fail -> line 448");
}

/* ---- entry point ----------------------------------------------------------- */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the create + directory-write coverage tests in sequence.
 *          Each test is self-contained: it builds and formats the volume,
 *          mounts, injects its fault or patches the disk, unmounts, and
 *          frees the disk.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides malloc/free and stderr.
 * @post The targeted create/dir branches in ra8_fs_fat_exfat_write.c are
 *       exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_create_empty_path();
  internal_test_create_path_too_long();
  internal_test_create_zero_len();
  internal_test_write_data_fail();
  internal_test_read_entry_fail();
  internal_test_dir_space_full_root_eoc();
  internal_test_dir_space_fat_get_fail();
  internal_test_dir_space_chain();
  internal_test_write_dir_set_read_fail();
  internal_test_write_dir_set_write_fail();
  return 0;
}
