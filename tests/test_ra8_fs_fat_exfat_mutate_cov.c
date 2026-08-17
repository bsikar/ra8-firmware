/**
 * @file tests/test_ra8_fs_fat_exfat_mutate_cov.c
 * @brief Coverage-boost tests for `ra8_fs_fat_exfat_mutate.c`.
 *
 * @details
 * Reaches the hard-to-reach branches in `priv_exfat_unlink`,
 * `priv_exfat_rename`, `priv_exfat_free_clusters`, `priv_exfat_listdir`,
 * and `priv_exfat_gather_name` by using a RAM-backed 64 MiB exFAT volume
 * and patching raw directory entries / FAT entries directly in the sector
 * store after the normal filesystem API creates the baseline state.
 *
 * This suite owns the corruption-injection and error paths -- a mistyped
 * secondary, an over-long SecondaryCount, a broken FAT chain, a missing source.
 * The rename SUCCESS paths (short<->long resize, entry-set relocation, the data
 * surviving the move) and the `fsck.exfat` evidence live in
 * `test_ra8_fs_exfat_rename_long.c`, which is the functional companion to the
 * long-name rename work (#603).
 *
 * Every uncovered line is handled exactly once: either a positive test
 * exercises it, or a `GCOVR_EXCL_LINE` tag marks it as an I/O-failure /
 * scan-limit path that cannot be injected without a failing backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_exfat_mutate_test_util.h"
#include "unity_minimal.h"

/* ---- tests -------------------------------------------------------------- */

/**
 * @test test_exfat_unlink_not_found
 * @brief `ra8_fs_unlink` returns `k_ra8_err_not_found` on an empty exFAT volume.
 *
 * @details
 * After formatting with no files, the root directory holds only the three
 * system entries (bitmap, up-case, label) followed by EOD.  Scanning to EOD
 * without finding a matching File entry exercises line 205-206 in
 * `priv_exfat_find_set`: the `if (e[0] == k_exfat_entry_eod)` branch.
 *
 * Lines targeted: 206 (return k_ra8_err_not_found on EOD in find_set).
 *
 * @par MC/DC:
 * Decision: `if (e[0] == k_exfat_entry_eod)` -- 1 condition.
 * V1: entry is EOD (0x00) -> T -> not found returned (this test).
 * V2: entry is not EOD (existing tests for the success path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_err_not_found.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_unlink_not_found(void)
{
  TEST_BEGIN("exfat mutate cov: unlink nonexistent file -> not_found (line 206)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "NOFILE.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: unlink nonexistent file -> not_found (line 206)");
}

/**
 * @test test_exfat_rename_new_exists
 * @brief `ra8_fs_rename` returns `k_ra8_err_exists` when the target exists.
 *
 * @details
 * Creates both "A.TXT" and "B.TXT", then renames A to B.  The existence
 * probe in `priv_exfat_rename` (via `priv_exfat_find`) succeeds and the
 * function returns `k_ra8_err_exists` at line 429.
 *
 * Lines targeted: 429.
 *
 * @par MC/DC:
 * Decision: `if (priv_exfat_find(...) == k_ra8_ok)` -- 1 condition.
 * V1: new name found -> T -> exists returned (this test).
 * V2: new name not found -> F -> proceeds (rename success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_rename returns k_ra8_err_exists.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_rename_new_exists(void)
{
  TEST_BEGIN("exfat mutate cov: rename when target exists -> exists (line 429)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "B.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_rename(h, "A.TXT", "B.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: rename when target exists -> exists (line 429)");
}

/**
 * @test test_exfat_rename_not_found
 * @brief `ra8_fs_rename` returns `k_ra8_err_not_found` for a missing source.
 *
 * @details
 * Renames a file that does not exist.  `priv_exfat_find_set` scans to EOD
 * and returns `k_ra8_err_not_found`, which `priv_exfat_rename` propagates at
 * line 442.
 *
 * Lines targeted: 442 (propagate find_set error in rename).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after priv_exfat_find_set in
 * priv_exfat_rename (1 condition).
 * V1: source missing -> not_found -> TRUE -> propagated (this test).
 * V2: source found -> FALSE -> rename proceeds (rename success siblings).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_rename returns k_ra8_err_not_found.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_rename_not_found(void)
{
  TEST_BEGIN("exfat mutate cov: rename nonexistent file -> not_found (line 442)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rename(h, "GHOST.TXT", "OTHER.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: rename nonexistent file -> not_found (line 442)");
}

/**
 * @test test_exfat_stream_type_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when the first secondary is
 *        not a Stream-extension entry (lines 131-133).
 *
 * @details
 * Writes "A.TXT", then corrupts the on-disk Stream entry (root index 4) by
 * overwriting its type byte with `k_mut_type_bogus` (0x42, which is not
 * `k_exfat_entry_stream` 0xC0).  Calling `ra8_fs_unlink("A.TXT")` causes
 * `priv_exfat_take_set` to enter the `k==0` branch, detect the wrong type,
 * set `matched=0`, and continue.  The outer loop then sees EOD and returns
 * `k_ra8_err_not_found`.
 *
 * Lines targeted: 131-133 (stream type mismatch sets matched=0 + continue).
 *
 * @par MC/DC:
 * Decision: `if (se[0] != k_exfat_entry_stream)` -- 1 condition.
 * V1: type is 0x42 != 0xC0 -> T -> matched cleared (this test).
 * V2: type is 0xC0 -> F -> stream processing proceeds (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_err_not_found.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_stream_type_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: stream type mismatch -> matched=0 (lines 131-133)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Stream entry type byte (root index 4, byte 0). */
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  s_disk.bytes[strm_off]  = (uint8_t)k_mut_type_bogus;

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: stream type mismatch -> matched=0 (lines 131-133)");
}

/**
 * @test test_exfat_name_entry_type_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when a secondary is not a
 *        Name entry (lines 145-147).
 *
 * @details
 * Writes "A.TXT", then overwrites the Name entry's type byte (root index 5,
 * byte 0) with 0x00.  When `priv_exfat_take_set` reads this entry for
 * `k == 1`, the type is 0x00 != 0xC1, so `matched` is cleared and the loop
 * continues.  The outer scan hits EOD and returns `k_ra8_err_not_found`.
 *
 * Lines targeted: 145-147 (name type mismatch).
 *
 * @par MC/DC:
 * Decision: `if (se[0] != k_exfat_entry_name)` -- 1 condition.
 * V1: type is 0x00 != 0xC1 -> T -> matched cleared (this test).
 * V2: type is 0xC1 -> F -> name processing proceeds (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_err_not_found.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_name_entry_type_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: name entry type mismatch -> matched=0 (lines 145-147)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Name entry type byte (root index 5, byte 0). */
  const uint32_t name_off = internal_root_byte(h, (uint32_t)k_mut_root_name0_idx);
  s_disk.bytes[name_off]  = 0x00U;

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: name entry type mismatch -> matched=0 (lines 145-147)");
}

/**
 * @test test_exfat_name_chunk_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when the name chunk does not
 *        match the target (lines 150-152).
 *
 * @details
 * Writes "A.TXT" then "B.TXT" (same length -- 5 chars each).  When the
 * outer scan in `priv_exfat_find_set` finds the File entry for "A.TXT" and
 * calls `priv_exfat_take_set` looking for "B.TXT", the Name chunk comparison
 * (`priv_exfat_name_chunk_eq`) returns 0 because the stored name is "A.TXT",
 * not "B.TXT".  This clears `matched` at lines 150-152.
 * The scan then continues to the "B.TXT" set and succeeds, causing unlink to
 * return `k_ra8_ok`.
 *
 * Lines targeted: 150-152 (name chunk mismatch clears matched).
 *
 * @par MC/DC:
 * Decision: `if (priv_exfat_name_chunk_eq(...) == 0U)` -- 1 condition.
 * V1: names differ -> T -> matched cleared (this test).
 * V2: names match  -> F -> matched unchanged at 1 (normal success path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink("B.TXT") returns k_ra8_ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_name_chunk_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: name chunk mismatch (lines 150-152)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Both names are 5 chars so the NameLength check passes on both; the name
   * bytes differ, triggering the mismatch on the A.TXT set. */
  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "B.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "B.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: name chunk mismatch (lines 150-152)");
}

/**
 * @test test_exfat_too_many_secondary
 * @brief `priv_exfat_find_set` returns `k_ra8_err_no_mem` when `total > max_pos`
 *        (line 213-214).
 *
 * @details
 * Writes "A.TXT" (SecondaryCount = 2), then patches the File entry's
 * SecondaryCount byte (byte 1 of the File entry at root index 3) to 19.
 * This makes `total = 1 + 19 = 20 > k_exfat_set_max_entries = 19`, so
 * `priv_exfat_find_set` returns `k_ra8_err_no_mem` at line 214.
 * `priv_exfat_unlink` propagates that error at line 332.
 *
 * Lines targeted: 214 (no_mem in find_set), 332 (propagate in unlink).
 *
 * @par MC/DC:
 * Decision: `if (total > k_exfat_set_max_entries)` in priv_exfat_find_set
 * (1 condition).
 * V1: patched SecondaryCount 19 -> total 20 > 19 -> TRUE -> no_mem.
 * V2: normal SecondaryCount 2 -> total 3 -> FALSE (every other test).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_err_no_mem.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_too_many_secondary(void)
{
  TEST_BEGIN("exfat mutate cov: sec-count 19 -> no_mem (lines 214,332)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Patch SecondaryCount to 19: total = 20 > k_exfat_set_max_entries. */
  const uint32_t file_off = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_mut_file_secnt_off] = (uint8_t)k_mut_max_secondary;

  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: sec-count 19 -> no_mem (lines 214,332)");
}

/**
 * @test test_exfat_free_clusters_no_first
 * @brief `priv_exfat_free_clusters` returns early when `first < 2` (line 283-284).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry's FirstCluster field
 * (bytes 20-23 of the stream entry) to 0.  A cluster number below
 * `k_cluster_first_data` (2) is invalid; `priv_exfat_free_clusters`
 * returns `k_ra8_ok` immediately at line 284 without touching the bitmap.
 *
 * Lines targeted: 283-284 (first < k_cluster_first_data guard).
 *
 * @par MC/DC:
 * Decision: `if (first < k_cluster_first_data)` -- 1 condition.
 * V1: first == 0 < 2 -> T -> early return (this test).
 * V2: first == 5 >= 2 -> F -> proceeds to free (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_ok (file entry deleted; bitmap unchanged).
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_free_clusters_no_first(void)
{
  TEST_BEGIN("exfat mutate cov: first_cluster=0 -> early return (lines 283-284)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Zero the FirstCluster field in the Stream entry. */
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  internal_disk_set_u32le(strm_off + (uint32_t)k_mut_strm_off_clus, 0U);

  /* Unlink must not crash and must return ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: first_cluster=0 -> early return (lines 283-284)");
}

/**
 * @test test_exfat_free_clusters_zero_size
 * @brief `priv_exfat_free_clusters` returns early when `size == 0` (line 286-287).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry's DataLength field
 * (bytes 24-27) to 0.  A zero data length means no clusters were used;
 * `priv_exfat_free_clusters` returns `k_ra8_ok` immediately at line 287
 * without finding or clearing the bitmap.
 *
 * Lines targeted: 286-287 (size == 0 guard).
 *
 * @par MC/DC:
 * Decision: `if (size == 0U)` -- 1 condition.
 * V1: size == 0 -> T -> early return (this test).
 * V2: size != 0 -> F -> proceeds (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_ok.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_free_clusters_zero_size(void)
{
  TEST_BEGIN("exfat mutate cov: size=0 -> early return (lines 286-287)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Zero the DataLength field in the Stream entry. */
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  internal_disk_set_u32le(strm_off + (uint32_t)k_mut_strm_off_dlen, 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: size=0 -> early return (lines 286-287)");
}

/**
 * @test test_exfat_fat_chain_free
 * @brief The FAT-chain walk in `priv_exfat_free_clusters` frees a two-cluster
 *        chain (lines 303-306, 309-311, 314-315, 317-318).
 *
 * @details
 * Writes a 5000-byte file that spans two 4 KiB clusters (clusters 5 and 6
 * on a fresh 64 MiB volume).  By default the formatter uses the NoFatChain
 * mode (GeneralSecondaryFlags bit 1 set).  This test:
 *   1. Clears the NoFatChain bit in the Stream entry to force FAT-chain mode.
 *   2. Writes FAT[first_cluster] = first_cluster + 1 (link to second cluster).
 *   3. Writes FAT[first_cluster + 1] = EOC (0xFFFFFFFF).
 *   4. Calls ra8_fs_unlink, which enters the FAT-chain walk loop.
 *
 * The walk iterates twice:
 *   - Iteration 1: bitmap_clear(cluster5), fat_get->cluster6, is_eoc=false,
 *     clus=cluster6 (lines 317-318).
 *   - Iteration 2: bitmap_clear(cluster6), fat_get->EOC, is_eoc=true,
 *     return ok (line 315).
 *
 * Lines targeted:
 *   303 (clus=first), 304 (for loop header), 305 (bitmap_clear call),
 *   306 (if e!=ok), 309 (next=0), 310 (fat_get call), 311 (if e!=ok),
 *   314 (is_eoc check), 315 (return ok on EOC), 317 (clus=next),
 *   318 (loop body close).
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 condition.
 * V1: next is EOC -> T -> return ok (iteration 2).
 * V2: next is a valid cluster -> F -> clus=next, continue (iteration 1).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_unlink returns k_ra8_ok; bitmap bits for both clusters cleared.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_fat_chain_free(void)
{
  TEST_BEGIN("exfat mutate cov: FAT-chain 2-cluster free (lines 303-318)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Write a 5000-byte file -> two 4 KiB clusters allocated. */
  static uint8_t s_payload[k_mut_payload_two_cls];
  for (uint32_t i = 0U; i < (uint32_t)k_mut_payload_two_cls; i++) {
    s_payload[i] = (uint8_t)(i & (uint32_t)k_mut_mask_byte);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "A.TXT", s_payload, (uint32_t)k_mut_payload_two_cls));

  /* Read first_cluster from the Stream entry FirstCluster field. */
  const uint32_t strm_off    = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  const uint32_t first_clus  = internal_disk_get_u32le(strm_off + (uint32_t)k_mut_strm_off_clus);
  const uint32_t second_clus = first_clus + 1U;

  /* Clear the NoFatChain bit so free_clusters uses the FAT walk. */
  s_disk.bytes[strm_off + (uint32_t)k_mut_strm_off_flags] &= (uint8_t)~(uint8_t)k_mut_no_fat_bit;

  /* Build a two-entry FAT chain: first -> second -> EOC. */
  internal_disk_set_u32le(internal_fat_byte(h, first_clus), second_clus);
  internal_disk_set_u32le(internal_fat_byte(h, second_clus), (uint32_t)k_mut_fat_eoc);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: FAT-chain 2-cluster free (lines 303-318)");
}

/**
 * @test test_exfat_listdir_skip_non_stream
 * @brief `priv_exfat_listdir` skips a file set whose first secondary is not a
 *        Stream-extension entry (line 529-530).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry type byte from 0xC0 to
 * `k_mut_type_bogus` (0x42).  When `priv_exfat_listdir` reads this second
 * entry after the File entry it finds type != 0xC0 and executes `continue`
 * at line 530, never invoking the callback.  Listdir then reads the Name
 * entry (not a File entry, skipped) and EOD, and returns `k_ra8_ok`.
 * The callback count remains 0.
 *
 * Lines targeted: 529-530.
 *
 * @par MC/DC:
 * Decision: `if (strm[0] != k_exfat_entry_stream)` -- 1 condition.
 * V1: type is 0x42 != 0xC0 -> T -> continue (this test).
 * V2: type is 0xC0 -> F -> proceeds to gather_name (normal listdir path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_listdir returns k_ra8_ok with count == 0.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_listdir_skip_non_stream(void)
{
  TEST_BEGIN("exfat mutate cov: bad stream type -> listdir skip (lines 529-530)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Stream entry type byte. */
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  s_disk.bytes[strm_off]  = (uint8_t)k_mut_type_bogus;

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &ctx));
  TEST_ASSERT_EQ(0U, ctx.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: bad stream type -> listdir skip (lines 529-530)");
}

/**
 * @test test_exfat_gather_name_skip_non_name
 * @brief `priv_exfat_gather_name` skips secondaries whose type is not
 *        `k_exfat_entry_name` (line 488-489).
 *
 * @details
 * Writes "A.TXT", then patches the Name entry type byte (root index 5,
 * byte 0) from 0xC1 to `k_mut_type_bogus` (0x42).  The Stream entry is
 * left intact (type 0xC0) so `priv_exfat_listdir` reaches `gather_name`.
 * Inside `gather_name` the patched entry fails the `ne[0] == 0xC1` check
 * and `continue` fires, so no units are gathered.
 *
 * The entry is then NOT reported.  This case used to assert the opposite --
 * one callback with an empty name -- which was the same class of defect #606
 * is about: a listing entry the caller cannot do anything with, because `""`
 * re-opens nothing.  A set whose name cannot be assembled has no name this
 * library will vouch for, and the walk still consumes every secondary, so the
 * cursor stays aligned and the entries after it are listed normally.
 *
 * Lines targeted: the non-name-secondary skip in gather_name, and the
 * empty-name skip in listdir.
 *
 * @par MC/DC:
 * Decision: `if (ne[0] != k_exfat_entry_name)` -- 1 condition.
 * V1: type != 0xC1 -> T -> continue (this test).
 * V2: type == 0xC1 -> F -> copy name units (normal gather_name path).
 *
 * Decision: `if (name[0] == '\0')` in `priv_exfat_listdir` -- 1 condition.
 * V3: no units gathered -> T -> the entry is skipped (this test).
 * V4: a real name       -> F -> the callback fires (every sibling test).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_listdir returns k_ra8_ok and reported nothing.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_gather_name_skip_non_name(void)
{
  TEST_BEGIN("exfat mutate cov: non-name secondary skipped in gather_name (lines 488-489)");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Patch the Name entry type byte; leave Stream entry intact. */
  const uint32_t name_off = internal_root_byte(h, (uint32_t)k_mut_root_name0_idx);
  s_disk.bytes[name_off]  = (uint8_t)k_mut_type_bogus;

  /* listdir must walk cleanly and report nothing: there is no name here. */
  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &ctx));
  TEST_ASSERT_EQ(0U, ctx.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: non-name secondary skipped in gather_name (lines 488-489)");
}

/**
 * @test test_exfat_listdir_read_fault
 * @brief `priv_exfat_listdir` propagates a backend read failure.
 *
 * @details
 * Arms the fixture's one-shot read fault immediately before `ra8_fs_listdir`,
 * so the first sector read inside `priv_exfat_dir_next` fails. The walk
 * returns that error instead of an entry, which is the only way the listdir
 * loop's status operand can be true.
 *
 * @par MC/DC:
 * Decision: `if ((err != k_ra8_ok) || !present)` in `priv_exfat_listdir`
 * -- 2 conditions.
 * V1: err = k_ra8_ok, present = true  -> F,F -> false (every listing test).
 * V2: err = k_ra8_ok, present = false -> F,T -> true  (end of directory).
 * V3: err = k_ra8_err_out_of_range    -> T,- -> true  (this test).
 * V1+V3 prove the status condition independently affects the outcome; V1+V2
 * do the same for the entry-present condition. N+1 = 3 vectors for N = 2.
 *
 * @pre Volume is formatted, mounted, and holds one file.
 * @post ra8_fs_listdir returns the backend error and reported nothing.
 * @post The one-shot fault has disarmed itself.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_listdir_read_fault(void)
{
  TEST_BEGIN("exfat mutate cov: listdir propagates a backend read fault");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &dummy, 1U));

  mut_list_ctx_t ctx = {};
  s_mut_rd_fail_in   = 0;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_listdir(h, "/", internal_count_cb, &ctx));
  s_mut_rd_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(0U, ctx.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat mutate cov: listdir propagates a backend read fault");
}

/* ---- entry point -------------------------------------------------------- */

int main(void)
{
  internal_test_exfat_unlink_not_found();
  internal_test_exfat_rename_new_exists();
  internal_test_exfat_rename_not_found();
  internal_test_exfat_stream_type_mismatch();
  internal_test_exfat_name_entry_type_mismatch();
  internal_test_exfat_name_chunk_mismatch();
  internal_test_exfat_free_clusters_no_first();
  internal_test_exfat_free_clusters_zero_size();
  internal_test_exfat_fat_chain_free();
  internal_test_exfat_too_many_secondary();
  internal_test_exfat_listdir_skip_non_stream();
  internal_test_exfat_gather_name_skip_non_name();
  internal_test_exfat_listdir_read_fault();
  return 0;
}
