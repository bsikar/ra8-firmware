/**
 * @file test_ra8_fs_fat_mount_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_mount.c.
 *
 * @details
 * Dedicated companion test executable that drives the branches in
 * ra8_fs_fat_mount.c not yet exercised by sibling test files:
 *
 *   - priv_alloc_mount_slot full (lines 68-69): mount table exhaustion.
 *   - priv_alloc_file_slot full (lines 79-80): file table exhaustion.
 *   - priv_compute_geometry bad geometry (line 144): total < first_data_lba.
 *   - priv_gpt_entry_first_lba hi LBA (line 279): 64-bit first-LBA rejected.
 *   - priv_gpt_entry_is_basic_data mismatch (line 305): GUID byte differs.
 *   - priv_gpt_scan_entries read fail (line 378): backend error mid-scan.
 *   - priv_gpt_scan_entries any_lba fallback (lines 387-389).
 *   - priv_gpt_scan_entries not_found (line 391): no usable entry found.
 *   - priv_gpt_locate_volume LBA-1 read fail (line 419).
 *   - priv_gpt_locate_volume bad signature (line 423).
 *   - priv_gpt_locate_volume entry_lba hi word nonzero (line 427).
 *   - priv_gpt_locate_volume entry_lba zero (line 433).
 *   - priv_gpt_locate_volume entry size != 128 (line 436).
 *   - priv_gpt_locate_volume count clamped to 128 (lines 439-440).
 *   - ra8_fs_format() get_capacity failure (line 463).
 *   - priv_read_boot_sector first read fail (line 511).
 *   - priv_read_boot_sector GPT error propagated (line 524).
 *   - priv_read_boot_sector second read fail (line 530).
 *   - ra8_fs_mount no free slot (line 546).
 *   - ra8_fs_mount geometry failure (line 559).
 *   - ra8_fs_unmount slot not in use (line 595).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_mount_test_util.h"
#include "unity_minimal.h"

/**
 * @enum mount_cov_vec_t
 * @brief Per-test disk geometries and GPT vector values.
 *
 * @details Sector counts are deliberately tiny so out-of-range reads fire at
 *          exact positions; the GPT values pair a spec-conforming entry size
 *          (128) with the deviant values each test needs.
 *
 * @invariant k_gpt_count_overmax exceeds the mount path's 128-entry scan cap.
 * @see write_gpt_header()
 */
typedef enum : uint32_t {
  k_geo_probe_sectors  = 20U,   /**< Small disk; only LBA 0 is meaningful.    */
  k_geo_bad_total      = 10U,   /**< BPB total-sectors < first_data_lba (66). */
  k_geo_gpt_disk       = 10U,   /**< MBR + GPT hdr + entries + partition.     */
  k_mbr_far_part_lba   = 50U,   /**< Partition LBA beyond a 3-sector disk.    */
  k_gpt_entry_size     = 128U,  /**< Spec-conforming GPT entry size.          */
  k_gpt_entry_size_bad = 64U,   /**< Non-conforming GPT entry size.           */
  k_gpt_count_overmax  = 200U,  /**< Entry count above the 128 scan cap.      */
  k_gpt_ent_off_lba_hi = 0x24U, /**< first_lba high-word offset in an entry.  */
  k_gpt_ent_off_lba_lo = 0x20U, /**< first_lba low-word offset in an entry.   */
} mount_cov_vec_t;

/* ===========================================================================
 * Test: mount table full (lines 68-69, 546)
 * ===========================================================================
 */

/**
 * @test test_mount_table_full
 * @brief Filling both mount slots makes the third ra8_fs_mount return k_ra8_err_no_mem.
 *
 * @details
 * Mounts the same FAT16 image twice (filling s_mounts[0] and s_mounts[1]).
 * The third mount call enters priv_alloc_mount_slot which exhausts the table
 * (lines 68-69) and returns nullptr; ra8_fs_mount propagates k_ra8_err_no_mem
 * (line 546).  Both successful mounts are unmounted to restore global state.
 *
 * @par MC/DC:
 * Decision: `if (m == nullptr)` (1 condition, line 545).
 * V1: table has a free slot -> m != nullptr -> false -> mount proceeds (normal path).
 * V2: table full -> m == nullptr -> true -> k_ra8_err_no_mem (covered here).
 *
 * @pre s_disk is nullptr (start of test).
 * @post Both successfully-mounted handles are unmounted; s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mount_table_full(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: mount table full returns no_mem");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h1 = nullptr;
  ra8_fs_mount_t* h2 = nullptr;
  ra8_fs_mount_t* h3 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h2));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_mount(&s_backend, &h3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h2));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: mount table full returns no_mem");
}

/* ===========================================================================
 * Test: file table full (lines 79-80)
 * ===========================================================================
 */

/**
 * @test test_file_table_full
 * @brief Filling all four file slots makes the next ra8_fs_open return k_ra8_err_no_mem.
 *
 * @details
 * Creates files A-D by opening each for write and immediately closing.  Then
 * opens A, B, C, D for read, leaving all four s_files slots in_use.  A fifth
 * open of "E.TXT" (which also exists) triggers priv_alloc_file_slot to complete
 * its loop without finding a free slot and return nullptr (lines 79-80); the
 * caller propagates k_ra8_err_no_mem.
 *
 * @par MC/DC:
 * Decision: `if (s_files[i].in_use == 0U)` inside priv_alloc_file_slot (1 cond).
 * All four iterations observe in_use == 1 -> condition always false -> loop
 * completes -> nullptr returned.
 *
 * @pre s_disk is nullptr (start of test).
 * @post All four open handles are closed; mount unmounted; s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_file_table_full(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: file table full returns no_mem");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create five files on disk so E.TXT also exists to open. */
  ra8_fs_file_t* tmp = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));

  /* Occupy all four file slots. */
  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  ra8_fs_file_t* fc = nullptr;
  ra8_fs_file_t* fd = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_read, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_read, &fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_read, &fd));

  /* Fifth open: priv_alloc_file_slot returns nullptr -> k_ra8_err_no_mem. */
  ra8_fs_file_t* fe = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_read, &fe));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: file table full returns no_mem");
}

/* ===========================================================================
 * Test: geometry validation fail (lines 144, 559)
 * ===========================================================================
 */

/**
 * @test test_geometry_validation_fail
 * @brief BPB with total_sectors smaller than first_data_lba triggers line 144.
 *
 * @details
 * Constructs a BPB where rsvd=1, num_fats=2, fat_sz=32, root_ents=16.
 * Derived geometry: first_fat=1, first_root=65, first_data=66.
 * total_sectors is set to 10, which is less than 66.  priv_compute_geometry
 * detects the underflow and returns k_ra8_err_validation_failed (line 144);
 * ra8_fs_mount propagates it (line 559).
 *
 * @par MC/DC:
 * Decision: `if (m->total_sectors < m->first_data_lba)` (1 condition, line 143).
 * V1: total_sectors >= first_data_lba -> false (valid geometry, normal path).
 * V2: total_sectors < first_data_lba -> true -> k_ra8_err_validation_failed (here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed; no mount left active.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_geometry_validation_fail(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: total_sectors < first_data_lba returns validation_failed");
  /* Allocate 20 sectors to avoid OOB reads; only LBA 0 is meaningful here. */
  internal_alloc_disk((uint32_t)k_geo_probe_sectors);
  uint8_t* bpb = s_disk.bytes;
  /* BPB: bytes_per_sec=512, spc=1, rsvd=1, num_fats=2, root_ents=16,
   * fat_sz=32, tot_sec=10.  first_data_lba = 1+64+1 = 66 > 10. */
  internal_put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_mc_blk);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  internal_put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  internal_put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 16U);
  internal_put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_geo_bad_total);
  internal_put16(bpb, (uint32_t)k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
  ra8_fs_mount_t* h   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: total_sectors < first_data_lba returns validation_failed");
}

/* ===========================================================================
 * Test: unmount not in use (line 595)
 * ===========================================================================
 */

/**
 * @test test_unmount_not_in_use
 * @brief Unmounting a slot that is already free returns k_ra8_err_invalid_state.
 *
 * @details
 * Mounts and immediately unmounts a FAT16 volume (in_use transitions 0->1->0).
 * A second unmount call on the same handle finds in_use == 0 and returns
 * k_ra8_err_invalid_state (line 595).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (1 condition, line 594).
 * V1: in_use == 1 -> false -> unmount succeeds (first call, normal path).
 * V2: in_use == 0 -> true -> k_ra8_err_invalid_state returned (second call here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unmount_not_in_use(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: unmount not-in-use slot returns invalid_state");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_unmount(h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: unmount not-in-use slot returns invalid_state");
}

/* ===========================================================================
 * Test: format get_capacity failure (line 463)
 * ===========================================================================
 */

/**
 * @test test_format_get_capacity_fails
 * @brief ra8_fs_format propagates a get_capacity backend error (line 463).
 *
 * @details
 * Provides a backend whose get_capacity always returns k_ra8_err_hw_error.
 * ra8_fs_format passes the null/invalid-arg guards and calls get_capacity;
 * the error is returned immediately at line 463.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after get_capacity (1 condition, line 462).
 * V1: get_capacity returns k_ra8_ok -> false -> format continues (normal path).
 * V2: get_capacity returns non-ok -> true -> error propagated (covered here).
 *
 * @pre No disk allocation needed.
 * @post No mount or disk state changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_format_get_capacity_fails(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: format propagates get_capacity error");
  const ra8_fs_backend_t fail_backend = {
    .read_block   = nullptr,
    .write_block  = internal_dummy_write,
    .get_capacity = internal_fail_capacity,
    .ctx          = nullptr,
  };
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_format(&fail_backend, &opts));
  TEST_END("ra8_fs_fat_mount cov: format propagates get_capacity error");
}

/**
 * @test test_format_zero_block_count
 * @brief ra8_fs_format rejects a well-formed sector size with zero sectors
 *        (the second operand of the capacity guard).
 *
 * @details
 * Provides a backend whose get_capacity reports a valid 512-byte sector size
 * but a zero block count. ra8_fs_format passes the guards above, reads the
 * capacity, then rejects it at the block_size/block_count guard.
 *
 * @par MC/DC:
 * Decision: `if (block_size != k_ra8_fs_bytes_per_sector || block_count == 0U)`
 * (2 conditions, line 465).
 * - V1: block_size == 512, block_count > 0 -> false (normal format path,
 *       exercised by the format round-trip tests).
 * - V2: block_size != 512                  -> true (first operand; short-circuit).
 * - V3: block_size == 512, block_count == 0 -> C1 false, C2 true (covered here).
 * Pair (V1,V3) proves the block_count operand independently moves the outcome.
 *
 * @pre No disk allocation needed.
 * @post No mount or disk state changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_format_zero_block_count(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: format rejects zero block count");
  const ra8_fs_backend_t zero_backend = {
    .read_block   = nullptr,
    .write_block  = internal_dummy_write,
    .get_capacity = internal_zero_count_capacity,
    .ctx          = nullptr,
  };
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_format(&zero_backend, &opts));
  TEST_END("ra8_fs_fat_mount cov: format rejects zero block count");
}

/* ===========================================================================
 * Test: first read fails (line 511)
 * ===========================================================================
 */

/**
 * @test test_first_read_fails
 * @brief A backend that rejects every read makes priv_read_boot_sector fail at
 *        line 511.
 *
 * @details
 * priv_read_boot_sector reads LBA 0 first (line 509).  When that read returns
 * k_ra8_err_hw_error the function returns immediately at line 511 and
 * ra8_fs_mount propagates the error.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after first read (1 cond, line 510).
 * V1: read succeeds -> false -> parsing continues (normal path).
 * V2: read fails -> true -> error propagated immediately (covered here).
 *
 * @pre No disk allocation needed.
 * @post No mount or disk state changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_first_read_fails(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: first read fail from priv_read_boot_sector");
  const ra8_fs_backend_t fail_rd_backend = {
    .read_block   = internal_always_fail_read,
    .write_block  = internal_dummy_write,
    .get_capacity = internal_dummy_capacity_ok,
    .ctx          = nullptr,
  };
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mount(&fail_rd_backend, &h));
  TEST_END("ra8_fs_fat_mount cov: first read fail from priv_read_boot_sector");
}

/* ===========================================================================
 * Test: MBR second read fails (line 530)
 * ===========================================================================
 */

/**
 * @test test_mbr_second_read_fails
 * @brief An MBR pointing to a partition beyond the disk boundary makes
 *        priv_read_boot_sector fail at line 530.
 *
 * @details
 * LBA 0 carries a non-GPT MBR (partition type 0x01, not 0xEE) with 0x55/0xAA
 * and partition LBA 50.  The BPB parse of LBA 0 fails (bytes_per_sector is 0,
 * not 512), priv_mbr_part0_lba returns 50, partition_base_lba is set to 50,
 * and the second read (LBA 50 + 0 = absolute LBA 50) on a 3-sector disk
 * returns k_ra8_err_out_of_range (line 530).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after second read (1 cond, line 529).
 * V1: second read succeeds -> false -> parse continues (normal path).
 * V2: second read fails -> true -> error propagated (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mbr_second_read_fails(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: MBR second read fail at line 530");
  /* 3 sectors only; partition LBA 50 is beyond the disk boundary. */
  internal_alloc_disk(3U);
  uint8_t* lba0 = s_disk.bytes;
  /* Non-GPT MBR: type 0x01, partition LBA 50. bytes_per_sec left at 0 so
   * the first BPB parse fails and the MBR path is taken. */
  lba0[k_bpb_off_sig0]      = (uint8_t)k_bpb_sig0_val;
  lba0[k_bpb_off_sig1]      = (uint8_t)k_bpb_sig1_val;
  lba0[k_mbr_off_part_type] = 0x01U;
  internal_put32(lba0, (uint32_t)k_mbr_off_part_lba, (uint32_t)k_mbr_far_part_lba);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: MBR second read fail at line 530");
}

/* ===========================================================================
 * Test: GPT header read fails (lines 419, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_header_read_fails
 * @brief A 1-sector disk with a protective MBR fails to read the GPT header at
 *        LBA 1, hitting lines 419 and 524.
 *
 * @details
 * LBA 0 is a protective MBR (type 0xEE, partition LBA 1, sig 0x55/0xAA).
 * The disk has only 1 sector so reading LBA 1 (k_gpt_header_lba) returns
 * k_ra8_err_out_of_range.  priv_gpt_locate_volume returns that error at
 * line 419; priv_read_boot_sector propagates it at line 524.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after GPT header read (1 cond, line 418).
 * V1: read succeeds -> false -> signature check proceeds (normal path).
 * V2: read fails -> true -> error returned at line 419 (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_header_read_fails(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT header read fails at line 419");
  internal_alloc_disk(1U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT header read fails at line 419");
}

/* ===========================================================================
 * Test: GPT bad signature (lines 423, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_bad_signature
 * @brief A protective MBR with an all-zero LBA 1 (no "EFI PART") hits line 423.
 *
 * @details
 * LBA 0 is a protective MBR; LBA 1 is all zeros (calloc default).  The
 * signature loop in priv_gpt_locate_volume finds priv_scratch[0] == 0 != 0x45
 * ('E') and returns k_ra8_err_validation_failed at line 423.
 * priv_read_boot_sector propagates it at line 524.
 *
 * @par MC/DC:
 * Decision: `if (priv_scratch[i] != k_gpt_signature[i])` (1 cond per iteration).
 * V1: all signature bytes match -> false on every iteration (normal path).
 * V2: first byte differs -> true -> k_ra8_err_validation_failed (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_bad_signature(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT bad signature returns validation_failed");
  internal_alloc_disk(2U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  /* LBA 1 is left all-zero by calloc; "EFI PART" is absent. */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT bad signature returns validation_failed");
}

/* ===========================================================================
 * Test: GPT entry_lba beyond 32 bits is FOLLOWED (#683)
 * ===========================================================================
 */

/**
 * @test test_gpt_entry_lba_hi_nonzero
 * @brief A GPT entry array past 2 TiB is addressed, not refused (#683).
 *
 * @details
 * The partition entry array starts beyond 2 TiB (the 64-bit field's high word
 * is non-zero). The old parser refused such geometry with not_supported; the
 * 64-bit backend interface addresses it, so the locate now READS at that LBA.
 * On this three-sector fake the address is far past the medium, so what comes
 * back is the backend's own out_of_range -- proof the full 64-bit address
 * reached the backend instead of being masked or rejected early.
 *
 * @par MC/DC:
 * Decision: none removed-guard coverage remains; the 64-bit read is a single
 * data path (no compound decision). The sibling GPT tests keep the in-range
 * control vector.
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_entry_lba_hi_nonzero(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT entry_lba past 2 TiB reaches the backend");
  internal_alloc_disk(3U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1, 2U, 1U, 4U, (uint32_t)k_gpt_entry_size); /* entry_lba_hi = 1 */
  ra8_fs_mount_t* h = nullptr;
  /* The 64-bit LBA (2^32 + 2) is handed to the backend, whose fake medium is
   * three sectors long -- its own bounds check answers. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT entry_lba past 2 TiB reaches the backend");
}

/* ===========================================================================
 * Test: GPT entry_lba zero (lines 433, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_entry_lba_zero
 * @brief A GPT header with entry_lba == 0 triggers line 433.
 *
 * @details
 * UEFI requires the partition entry array to start at LBA >= 2.  When
 * entry_lba is zero the GPT header is structurally invalid; priv_gpt_locate_volume
 * returns k_ra8_err_validation_failed at line 433.
 *
 * @par MC/DC:
 * Decision: `if (entry_lba == 0U)` (1 cond, line 432).
 * V1: entry_lba != 0 -> false -> scan proceeds (normal path).
 * V2: entry_lba == 0 -> true -> k_ra8_err_validation_failed (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_entry_lba_zero(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT entry_lba zero returns validation_failed");
  internal_alloc_disk(3U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1, 0U, 0U, 4U, (uint32_t)k_gpt_entry_size); /* entry_lba = 0 */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT entry_lba zero returns validation_failed");
}

/* ===========================================================================
 * Test: GPT entry size bad (lines 436, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_entry_size_bad
 * @brief A GPT header with entry_size != 128 triggers line 436.
 *
 * @details
 * This driver only handles the standard 128-byte entry size.  When
 * entry_size is 64 priv_gpt_locate_volume returns k_ra8_err_not_supported at
 * line 436.
 *
 * @par MC/DC:
 * Decision: `if (entry_size != 128U)` (1 cond, line 435).
 * V1: entry_size == 128 -> false -> scan proceeds (normal path).
 * V2: entry_size != 128 -> true -> k_ra8_err_not_supported (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_entry_size_bad(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT entry_size != 128 returns not_supported");
  internal_alloc_disk(3U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1,
                            2U,
                            0U,
                            4U,
                            (uint32_t)k_gpt_entry_size_bad); /* entry_size != 128 */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT entry_size != 128 returns not_supported");
}

/* ===========================================================================
 * Test: GPT count clamped then scan read fails (lines 439-440, 378, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_count_clamped_scan_fails
 * @brief count > 128 gets clamped (lines 439-440); the second entry sector
 *        read then fails (line 378); the error propagates to line 524.
 *
 * @details
 * The GPT header advertises 200 entries (> k_gpt_entry_scan_max = 128).
 * priv_gpt_locate_volume clamps count to 128 (lines 439-440).  Entries 0-3
 * map to LBA 2 (which exists); their GUIDs are all-zero so no candidate is
 * recorded.  Entries 4-7 map to LBA 3 (beyond the 3-sector disk), so
 * priv_gpt_scan_entries returns k_ra8_err_out_of_range at line 378.
 * priv_gpt_locate_volume returns that error; priv_read_boot_sector propagates
 * at line 524.
 *
 * @par MC/DC:
 * Decision: `if (count > k_gpt_entry_scan_max)` (1 cond, line 438).
 * V1: count <= 128 -> false -> count used as-is (normal path).
 * V2: count > 128 -> true -> count clamped (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_count_clamped_scan_fails(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT count clamped, scan read fails at line 378");
  internal_alloc_disk(3U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1,
                            2U,
                            0U,
                            (uint32_t)k_gpt_count_overmax,
                            (uint32_t)k_gpt_entry_size); /* count > 128 -> clamped */
  /* LBA 2 stays all-zero (null GUIDs, no candidates). LBA 3 does not exist. */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT count clamped, scan read fails at line 378");
}

/* ===========================================================================
 * Test: GPT entry first_lba hi nonzero (lines 279, 391, 524)
 * ===========================================================================
 */

/**
 * @test test_gpt_entry_hi_first_lba
 * @brief A GPT entry whose first_lba exceeds 32 bits is FOLLOWED (#683).
 *
 * @details
 * Entry 0 has a non-zero type GUID (so it appears allocated) and a first LBA
 * of exactly 2^32 (high word 1, low word 0). The old parser treated the
 * entry as unusable and reported not_found; the 64-bit backend interface
 * addresses it, so the mount retargets to that LBA and READS there. On this
 * four-sector fake the address is far past the medium, and the backend's own
 * out_of_range comes back -- proof the untruncated 64-bit LBA reached it.
 *
 * @par MC/DC:
 * (no compound decision -- the old one-condition high-word guard is deleted;
 * this is the behavioral pin that the full 64-bit LBA is used)
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_entry_hi_first_lba(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: GPT entry first_lba past 2 TiB reaches the backend");
  internal_alloc_disk(4U);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1, 2U, 0U, 4U, (uint32_t)k_gpt_entry_size);
  /* Entry 0 in LBA 2: non-zero GUID (bytes 0-15 = 1) + hi first_lba = 1. */
  uint8_t* lba2   = &s_disk.bytes[(size_t)2U * (uint32_t)k_mc_blk];
  uint8_t* entry0 = lba2;
  /* Type GUID: all bytes 1 (a live, non-Basic-Data entry). */
  for (uint32_t i = 0U; i < 16U; i++) {
    entry0[i] = 1U;
  }
  /* first_lba = 2^32 exactly: low word 0, high word 1. */
  entry0[k_gpt_ent_off_lba_hi] = 1U;
  /* Entries 1-3 remain all-zero (null GUIDs). */
  ra8_fs_mount_t* h = nullptr;
  /* The 64-bit LBA is handed to the backend, whose four-sector fake medium
   * answers with its own bounds error rather than the parser refusing. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: GPT entry first_lba past 2 TiB reaches the backend");
}

/* ===========================================================================
 * Test: GPT non-basic-data GUID fallback (lines 305, 387-389)
 * ===========================================================================
 */

/**
 * @test test_gpt_non_basic_data_fallback
 * @brief A GPT entry with a non-Basic-Data GUID stores the LBA in any_lba
 *        (lines 387-389) after priv_gpt_entry_is_basic_data returns 0 (line 305).
 *
 * @details
 * Entry 0 has type GUID byte 0 == 0x02 (differs from Basic Data's 0xA2).
 * priv_gpt_entry_is_basic_data returns 0 at line 305; priv_gpt_note_entry
 * leaves basic_lba at 0 but sets any_lba to 4 (the entry's first_lba).
 * After scanning four entries basic_lba remains 0 so lines 387-389 are taken
 * (any_lba fallback, k_ra8_ok).  partition_base_lba is set to 4.  LBA 4
 * contains all zeros (no valid BPB), so the final volume parse fails with
 * k_ra8_err_validation_failed.
 *
 * @par MC/DC:
 * Decision: `if (entry[i] != k_gpt_guid_basic_data[i])` inside
 * priv_gpt_entry_is_basic_data (1 cond per iteration, line 304).
 * V1: byte matches -> condition false -> loop continues (later iterations).
 * V2: byte differs -> condition true -> return 0 at line 305 (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_gpt_non_basic_data_fallback(void)
{
  TEST_BEGIN("ra8_fs_fat_mount cov: non-Basic-Data GUID uses any_lba fallback");
  /* 10 sectors: MBR(0)+GPT_hdr(1)+entries(2)+spare(3)+partition_start(4-9). */
  internal_alloc_disk((uint32_t)k_geo_gpt_disk);
  internal_write_protective_mbr(s_disk.bytes, 1U);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  internal_write_gpt_header(lba1, 2U, 0U, 4U, (uint32_t)k_gpt_entry_size);
  /* Entry 0: GUID[0]=0x02 (not 0xA2 = Basic Data[0]) -> line 305 triggered.
   * first_lba low=4 (within 10 sectors), high=0. */
  uint8_t* lba2   = &s_disk.bytes[(size_t)2U * (uint32_t)k_mc_blk];
  uint8_t* entry0 = lba2;
  entry0[0]       = 0x02U;                                    /* non-basic GUID */
  internal_put32(entry0, (uint32_t)k_gpt_ent_off_lba_lo, 4U); /* first LBA = 4  */
  /* Entries 1-3 remain all-zero. LBA 4-9 all-zero (no valid BPB). */
  ra8_fs_mount_t* h = nullptr;
  /* The FAT parse at LBA 4 finds no 0x55/0xAA -> k_ra8_err_validation_failed. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_disk();
  TEST_END("ra8_fs_fat_mount cov: non-Basic-Data GUID uses any_lba fallback");
}

/* ===========================================================================
 * main
 * ===========================================================================
 */

int main(void)
{
  internal_test_mount_table_full();
  internal_test_file_table_full();
  internal_test_geometry_validation_fail();
  internal_test_unmount_not_in_use();
  internal_test_format_get_capacity_fails();
  internal_test_format_zero_block_count();
  internal_test_first_read_fails();
  internal_test_mbr_second_read_fails();
  internal_test_gpt_header_read_fails();
  internal_test_gpt_bad_signature();
  internal_test_gpt_entry_lba_hi_nonzero();
  internal_test_gpt_entry_lba_zero();
  internal_test_gpt_entry_size_bad();
  internal_test_gpt_count_clamped_scan_fails();
  internal_test_gpt_entry_hi_first_lba();
  internal_test_gpt_non_basic_data_fallback();
  return 0;
}
