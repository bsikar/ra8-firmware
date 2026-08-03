/**
 * @file tests/test_ra8_fs_fat_exfat_write_bmp_cov.c
 * @brief Coverage-boost tests for the exFAT allocation-bitmap write paths.
 *
 * @details
 * Split sibling of test_ra8_fs_fat_exfat_write_dir_cov.c targeting the
 * bitmap half of `ra8_fs_fat_exfat_write.c`: the priv_exfat_find_bitmap EoD
 * and read-failure arms, the priv_exfat_bitmap_scan read-failure and
 * full-volume arms, and the priv_exfat_bmp_switch write-failure,
 * sector-change, and read-failure arms. Fault positions are injected with
 * the countdown backend from tests/support/fs_fat_exfat_write_test_util.h
 * (see that header for the full R/W call-sequence map).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "support/fs_fat_exfat_write_test_util.h"
#include "unity_minimal.h"

/* ---- tests ----------------------------------------------------------------- */

/**
 * @test test_find_bitmap_eod
 * @brief `priv_exfat_find_bitmap` returns `k_ra8_err_not_found` at the
 *        end-of-directory marker (lines 82, 485, 561).
 *
 * @details Overwrites the first root-cluster entry's type byte with 0x00
 *          (EOD) after format, hiding the bitmap entry.  `priv_exfat_find_bitmap`
 *          sees EOD at entry 0 and returns `k_ra8_err_not_found` at line 82.
 *          This propagates through `priv_exfat_alloc_write` (line 485) and
 *          `priv_exfat_create` (line 561).
 *
 * Lines targeted: 82, 485, 561.
 *
 * @par MC/DC:
 * Decision: `if (e[0] == k_exfat_entry_eod)` in find_bitmap -- 1 condition.
 * V1: e[0]=0x00 (EOD) -> T -> not_found returned (this test).
 * V2: e[0]=0x81 (bitmap) -> F -> bitmap found (normal write_file path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_find_bitmap_eod(void)
{
  TEST_BEGIN("exfat write cov: EOD hides bitmap -> not_found (lines 82,485,561)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Overwrite the bitmap entry's type byte with EOD. */
  s_disk.bytes[root_entry_off(h, 0U)] = (uint8_t)k_wc_entry_eod;

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: EOD hides bitmap -> not_found (lines 82,485,561)");
}

/**
 * @test test_find_bitmap_read_fail
 * @brief Read failure on R1 propagates through `priv_exfat_find_bitmap`
 *        (lines 79, 485, 561).
 *
 * @details With `s_rd_remaining=0` the very first `priv_read_sector` call --
 *          inside `priv_exfat_next_entry` called by `priv_exfat_find_bitmap` --
 *          fails.  The error propagates at line 79 (find_bitmap), line 485
 *          (alloc_write), and line 561 (create).
 *
 * Lines targeted: 79, 485, 561.
 *
 * @par MC/DC:
 * Decision: `if (r != k_ra8_ok)` in find_bitmap -- 1 condition.
 * V1: r=k_ra8_err_out_of_range -> T -> error returned (this test).
 * V2: r=k_ra8_ok -> F -> entry examined (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_find_bitmap_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R1 read fail -> error at line 79");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r1;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R1 read fail -> error at line 79");
}

/**
 * @test test_bitmap_scan_read_fail
 * @brief Read failure on R2 propagates through `priv_exfat_bitmap_scan`
 *        (lines 124, 490, 561).
 *
 * @details After find_bitmap reads the root cluster sector (R1, succeeds),
 *          the second read is inside `priv_exfat_bitmap_scan`, which reads the
 *          bitmap sector.  Failing it exercises line 124 in bitmap_scan, line
 *          490 in alloc_write, and line 561 in create.
 *
 * Lines targeted: 124, 490, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in bitmap_scan -- 1 condition.
 * V1: e=error -> T -> early return (this test).
 * V2: e=k_ra8_ok -> F -> process bitmap byte (normal scan path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bitmap_scan_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R2 read fail -> error at line 124");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r2;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R2 read fail -> error at line 124");
}

/**
 * @test test_bitmap_scan_full_volume
 * @brief `priv_exfat_bitmap_scan` returns `k_ra8_err_no_mem` when every cluster
 *        bit is set (lines 143, 490, 561).
 *
 * @details Fills the entire bitmap cluster (8 sectors) with 0xFF after format,
 *          marking every cluster as allocated.  `priv_exfat_bitmap_scan`
 *          iterates all `count_of_clusters` entries without finding a free bit
 *          and returns `k_ra8_err_no_mem` at line 143.
 *
 * Lines targeted: 143, 490, 561.
 *
 * @par MC/DC:
 * Decision: loop exhausted without match in bitmap_scan -- 1 outcome.
 * V1: all bits 1 -> loop exhausted -> no_mem (this test).
 * V2: free bit found -> k_ra8_ok (normal write_file path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_no_mem.
 *
 * @since 0.1.0
 */
static void test_bitmap_scan_full_volume(void)
{
  TEST_BEGIN("exfat write cov: all clusters used -> no_mem at line 143");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Bitmap cluster is always at cluster 2 = first_data_lba for a fresh volume.
   * first_data_lba is partition-relative (the driver adds partition_base_lba
   * on every access), so indexing s_disk.bytes needs the base added back. */
  const uint32_t bmp_lba = h->partition_base_lba + h->first_data_lba;
  for (uint32_t s = 0U; s < h->sectors_per_cluster; s++) {
    memset(&s_disk.bytes[(size_t)(bmp_lba + s) * (uint32_t)k_wc_block_size],
           (int)k_wc_mask_byte,
           (uint32_t)k_wc_block_size);
  }

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: all clusters used -> no_mem at line 143");
}

/**
 * @test test_bmp_switch_write_fails
 * @brief `priv_exfat_bmp_switch` returns the write error when flushing the
 *        old sector fails (lines 153, 154, 155).
 *
 * @details Calls `priv_exfat_bmp_switch` directly with `*loaded` set to a
 *          valid LBA (not UINT32_MAX) and a different target LBA, so the
 *          function enters the flush block.  With `s_wr_remaining=0`, the
 *          `priv_write_sector` call fails, and the function returns the write
 *          error at line 155.
 *
 * Lines targeted: 153, 154, 155.
 *
 * @par MC/DC:
 * Decision: `if (we != k_ra8_ok)` in bmp_switch -- 1 condition.
 * V1: we=error -> T -> return write error (this test).
 * V2: we=k_ra8_ok -> F -> proceed to read (test_bmp_switch_sector_change).
 *
 * @pre Volume is formatted and accessible.
 * @post priv_exfat_bmp_switch returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_write_fails(void)
{
  TEST_BEGIN("exfat write cov: bmp_switch old-sector write fails (lines 153-155)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Arm: fail the very next write. */
  s_wr_remaining = (int32_t)k_wc_wr_at_w1;

  uint32_t        loaded               = h->first_fat_lba;
  uint8_t         sec[k_wc_block_size] = {};
  const ra8_err_t r = priv_exfat_bmp_switch(h, h->first_fat_lba + 1U, &loaded, sec);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: bmp_switch old-sector write fails (lines 153-155)");
}

/**
 * @test test_bmp_switch_sector_change
 * @brief `priv_exfat_bmp_switch` flushes the old sector and loads the new one
 *        when both I/O ops succeed (lines 153, 154, 157).
 *
 * @details Calls `priv_exfat_bmp_switch` with `*loaded` set to a valid LBA
 *          (not UINT32_MAX) and a different target LBA.  With no faults armed,
 *          the write of the old sector succeeds (line 153, we=k_ra8_ok) and the
 *          read of the new sector succeeds (line 157), returning k_ra8_ok.
 *
 * Lines targeted: 153, 154, 157.
 *
 * @par MC/DC:
 * Decision: `if (*loaded != UINT32_MAX)` in bmp_switch -- 1 condition.
 * V1: loaded=valid_lba -> T -> enters flush block (this test and write-fails
 *     test).
 * V2: loaded=UINT32_MAX -> F -> skips flush (normal bitmap_mark first call).
 *
 * @pre Volume is formatted and accessible.
 * @post priv_exfat_bmp_switch returns k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_sector_change(void)
{
  TEST_BEGIN("exfat write cov: bmp_switch flush+load -> k_ra8_ok (lines 153,154,157)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* No faults -- both write (flush old) and read (load new) must succeed. */
  uint32_t loaded               = h->first_fat_lba;
  uint8_t  sec[k_wc_block_size] = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_exfat_bmp_switch(h, h->first_fat_lba + 1U, &loaded, sec));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: bmp_switch flush+load -> k_ra8_ok (lines 153,154,157)");
}

/**
 * @test test_bmp_switch_read_fail
 * @brief Read failure on R3 propagates through `priv_exfat_bmp_switch`
 *        (lines 160, 197, 561).
 *
 * @details R1 (find_bitmap) and R2 (bitmap_scan) succeed; W1 (write_data)
 *          succeeds; R3 is the `priv_read_sector` call inside
 *          `priv_exfat_bmp_switch` (loaded=UINT32_MAX path, lines 158-160).
 *          The error propagates through `priv_exfat_bitmap_mark` (line 197)
 *          and `priv_exfat_create` (line 561).
 *
 * Lines targeted: 160, 197, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in bmp_switch read path -- 1 condition.
 * V1: e=error -> T -> return error (this test).
 * V2: e=k_ra8_ok -> F -> loaded updated, k_ra8_ok returned (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R3 read fail in bmp_switch (lines 160,197,561)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r3;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R3 read fail in bmp_switch (lines 160,197,561)");
}

/* ---- entry point ----------------------------------------------------------- */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the bitmap-path coverage tests in sequence. Each test is
 *          self-contained: it builds and formats the volume, mounts,
 *          injects its fault, unmounts, and frees the disk.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides malloc/free and stderr.
 * @post The targeted bitmap branches in ra8_fs_fat_exfat_write.c are
 *       exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_find_bitmap_eod();
  test_find_bitmap_read_fail();
  test_bitmap_scan_read_fail();
  test_bitmap_scan_full_volume();
  test_bmp_switch_write_fails();
  test_bmp_switch_sector_change();
  test_bmp_switch_read_fail();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_exfat_write_bmp_cov.c\n");
  return 0;
}
