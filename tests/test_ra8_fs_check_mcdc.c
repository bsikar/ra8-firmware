/**
 * @file test_ra8_fs_check_mcdc.c
 * @brief MC/DC vector suite for `ra8_fs_check()`'s compound decisions (#610).
 *
 * @details
 * One `test_mcdc_*` per enclosing function that carries a compound boolean
 * decision in `ra8_fs_fat_check.c` / `ra8_fs_fat_exfat_check.c`. Each drives the
 * N+1 independent-influence vectors for its decision(s) and cites the decision by
 * `path@function` in its `@par MC/DC:` block, so `check_new_compound_has_mcdc.py`
 * matches every new decision to a test. The detection behaviour these vectors
 * exercise is asserted in the sibling `test_ra8_fs_check.c`; here the assertions
 * only confirm the check ran, because the point is the decision coverage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "test_ra8_fs_check_util.h"

/**
 * @test test_mcdc_fat_entry
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_check.c@priv_fat_entry` -- two decisions.
 * Decision A: `(is_dir == 0) && ((attr & volume_id) != 0)` (skip a label entry).
 * - dir entry        -> C1 false                 (a subdirectory).
 * - ordinary file    -> C1 true,  C2 false       (is_dir 0, volume_id 0).
 * - volume-label     -> C1 true,  C2 true        (is_dir 0, volume_id 1).
 * Decision B: `(first != 0) && !in_range(first)` (an out-of-range entry).
 * - empty file       -> C1 false                 (first cluster 0).
 * - ordinary file    -> C1 true,  C2 false       (first in range).
 * - corrupted entry  -> C1 true,  C2 true        (first out of range).
 */
static void test_mcdc_fat_entry(void)
{
  TEST_BEGIN("MC/DC priv_fat_entry: label-skip and out-of-range guards");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));    /* dir: A C1=false          */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "VOL")); /* label: A C1=true C2=true */
  ra8_fs_file_t* ef = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.BIN", k_ra8_fs_mode_write, &ef)); /* B C1=false */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* file: A C1=true C2=false, B C1=true C2=false */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h                 = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  const uint32_t eo = fat_root_entry(h, "A       BIN");
  disk_wr16(eo + (uint32_t)k_chk_off_fst_lo, (uint16_t)k_chk_huge_lo); /* B C1=true C2=true */
  disk_wr16(eo + (uint32_t)k_chk_off_fst_hi, (uint16_t)k_chk_huge_hi);
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  chk_teardown(h);
  TEST_END("MC/DC priv_fat_entry: label-skip and out-of-range guards");
}

/**
 * @test test_mcdc_fat_tree
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_check.c@priv_fat_tree` --
 * decision `(loc.is_root != 0) && !fat32` (walk a fixed root vs a cluster root).
 * - FAT16 root       -> C1 true,  C2 true        (fixed-root region).
 * - FAT32 root       -> C1 true,  C2 false       (cluster-chained root).
 * - subdirectory     -> C1 false                 (never the fixed-root path).
 */
static void test_mcdc_fat_tree(void)
{
  TEST_BEGIN("MC/DC priv_fat_tree: root/type dispatch");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB")); /* C1 false when SUB is popped */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* FAT16 root: C1 true C2 true */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h = chk_setup((uint32_t)k_fmt_blocks_fat32,
                k_ra8_fs_type_fat32,
                "B.BIN",
                (uint32_t)k_chk_payload_mc);
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* FAT32 root: C1 true C2 false */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("MC/DC priv_fat_tree: root/type dispatch");
}

/**
 * @test test_mcdc_fat_diff
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_check.c@priv_fat_diff` --
 * decision `(v == free) || (v == marker)` (a cluster that is not lost).
 * - free cluster     -> C1 true                  (skipped, not lost).
 * - defective marker -> C1 false, C2 true        (skipped, not lost).
 * - used cluster     -> C1 false, C2 false       (a live cluster).
 */
static void test_mcdc_fat_diff(void)
{
  TEST_BEGIN("MC/DC priv_fat_diff: free/defective skip");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  disk_wr16(fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_defective); /* C1 false C2 true */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* free: C1 true; used: C1 false C2 false */
  TEST_ASSERT(r.clusters_bad >= 1U);
  chk_teardown(h);
  TEST_END("MC/DC priv_fat_diff: free/defective skip");
}

/**
 * @test test_mcdc_fat_fsinfo
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_check.c@priv_fat_fsinfo` -- two decisions.
 * Decision A: `(lba == 0) || (lba >= reserved_sectors)` (an untrustworthy FSInfo).
 * - BPB_FSInfo 0     -> C1 true                  (absent).
 * - valid FSInfo     -> C1 false, C2 false       (compared).
 * - FSInfo past area -> C1 false, C2 true        (out of the reserved region).
 * Decision B: `(stored != unknown) && (stored != free)` (a disagreeing count).
 * - free flipped     -> C1 true,  C2 true        (a real mismatch -> fault).
 * - matching count   -> C1 true,  C2 false       (agrees, no fault).
 * - unknown sentinel -> C1 false                 (0xFFFFFFFF, not compared).
 */
static void test_mcdc_fat_fsinfo(void)
{
  TEST_BEGIN("MC/DC priv_fat_fsinfo: FSInfo trust and free-count compare");
  const uint32_t        v = (uint32_t)k_fmt_blocks_fat32;
  ra8_fs_mount_t*       h = chk_setup(v, k_ra8_fs_type_fat32, "B.BIN", (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* A C1=false C2=false; B C1=true C2=false */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h                   = chk_setup(v, k_ra8_fs_type_fat32, "B.BIN", (uint32_t)k_chk_payload_mc);
  const uint32_t boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  const uint32_t fsi  = (uint32_t)disk_rd16(boot + (uint32_t)k_chk_fsi_off_num);
  const uint32_t foff = (h->partition_base_lba + fsi) * (uint32_t)k_fmt_block_size;
  disk_wr32(foff + (uint32_t)k_chk_fsi_off_free,
            disk_rd32(foff + (uint32_t)k_chk_fsi_off_free) + 1U); /* B C1=true C2=true */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_free_count_bad, r.first_fault.kind);
  chk_teardown(h);
  h = chk_setup(v, k_ra8_fs_type_fat32, "B.BIN", (uint32_t)k_chk_payload_mc);
  disk_wr16(h->partition_base_lba * (uint32_t)k_fmt_block_size + (uint32_t)k_chk_fsi_off_num,
            0U); /* A C1=true */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h = chk_setup(v, k_ra8_fs_type_fat32, "B.BIN", (uint32_t)k_chk_payload_mc);
  disk_wr16(h->partition_base_lba * (uint32_t)k_fmt_block_size + (uint32_t)k_chk_fsi_off_num,
            (uint16_t)k_chk_fsi_far); /* A C1=false C2=true */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h                    = chk_setup(v, k_ra8_fs_type_fat32, "B.BIN", (uint32_t)k_chk_payload_mc);
  const uint32_t boot2 = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  const uint32_t fsi2  = (uint32_t)disk_rd16(boot2 + (uint32_t)k_chk_fsi_off_num);
  const uint32_t foff2 = (h->partition_base_lba + fsi2) * (uint32_t)k_fmt_block_size;
  disk_wr32(foff2 + (uint32_t)k_chk_fsi_off_free, (uint32_t)k_chk_fsi_unknown); /* B C1=false */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("MC/DC priv_fat_fsinfo: FSInfo trust and free-count compare");
}

/**
 * @test internal_test_mcdc_check_locked
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_check.c@internal_check_locked` --
 * decision `(handle == nullptr) || (report == nullptr)` (2 conditions).
 * - V1 handle valid, report valid -> false (control: the scan runs).
 * - V2 handle NULL,  report valid -> C1 true  (varies handle only).
 * - V3 handle valid, report NULL  -> C1 false, C2 true (varies report only).
 * V1+V2 prove handle independently flips the outcome; V1+V3 prove report does.
 * Also covers the not-in-use guard (an unmounted handle -> invalid_state).
 */
RA8_INTERNAL
static void internal_test_mcdc_check_locked(void)
{
  TEST_BEGIN("MC/DC internal_check_locked: (handle||report) NULL pair + invalid_state");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr, 0U);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_check(nullptr, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), nullptr)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r)); /* unmounted */
  free_volume();
  TEST_END("MC/DC internal_check_locked: (handle||report) NULL pair + invalid_state");
}

/**
 * @test test_mcdc_exchk_set
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_check.c@priv_exchk_set` --
 * decision `(count < 2) || (count > k_exfat_set_max_entries)` (a malformed set).
 * - SecondaryCount 0   -> C1 true              (count 1 < 2).
 * - SecondaryCount 200 -> C1 false, C2 true    (count 201 over the maximum).
 * - a valid set        -> C1 false, C2 false   (a normal File entry set).
 */
static void test_mcdc_exchk_set(void)
{
  TEST_BEGIN("MC/DC priv_exchk_set: SecondaryCount bounds");
  const uint32_t        v = (uint32_t)k_fmt_blocks_exfat;
  ra8_fs_mount_t*       h = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* valid set: C1 false C2 false */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h           = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  uint32_t fe = exfat_first_file(h);
  s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt] = 0U; /* C1 true */
  r                                                 = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  chk_teardown(h);
  h  = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  fe = exfat_first_file(h);
  s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt] =
    (uint8_t)k_chk_secnt_over; /* C1 false C2 true */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  chk_teardown(h);
  TEST_END("MC/DC priv_exchk_set: SecondaryCount bounds");
}

/**
 * @test internal_test_mcdc_exchk_scan_dir
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_check.c@internal_exchk_scan_dir` --
 * decision `(e[0] == bitmap) || (e[0] == upcase)` (a system run entry).
 * A clean exFAT root carries all three arms: the allocation-bitmap entry (0x81)
 * -> C1 true, the up-case entry (0x82) -> C1 false C2 true, and File entries
 * (0x85) -> C1 false C2 false. One clean walk exercises every vector.
 */
RA8_INTERNAL
static void internal_test_mcdc_exchk_scan_dir(void)
{
  TEST_BEGIN("MC/DC internal_exchk_scan_dir: bitmap/up-case system entries");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                      k_ra8_fs_type_exfat,
                                      "T.BIN",
                                      (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("MC/DC internal_exchk_scan_dir: bitmap/up-case system entries");
}

/**
 * @test test_mcdc_exchk_diff_byte
 *
 * @par MC/DC:
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_check.c@priv_exchk_diff_byte` -- two decisions.
 * Decision A: `(alloc != 0) && (refd == 0)` (a lost cluster).
 * - spare bit set     -> C1 true,  C2 true       (allocated, unreferenced).
 * - live cluster      -> C1 true,  C2 false       (allocated and referenced).
 * - free cluster      -> C1 false                 (not allocated).
 * Decision B: `(alloc == 0) && (refd != 0)` (a bitmap mismatch).
 * - ref bit cleared   -> C1 true,  C2 true       (referenced, not allocated).
 * - free cluster      -> C1 true,  C2 false       (free and unreferenced).
 * - live cluster      -> C1 false                 (allocated).
 */
static void test_mcdc_exchk_diff_byte(void)
{
  TEST_BEGIN("MC/DC priv_exchk_diff_byte: bitmap-vs-referenced both ways");
  const uint32_t        v = (uint32_t)k_fmt_blocks_exfat;
  ra8_fs_mount_t*       h = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r)); /* A T,F and F; B T,F and F */
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  h = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  exfat_set_spare_bit(h); /* A C1=true C2=true: allocated, referenced by nothing */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.clusters_lost >= 1U);
  chk_teardown(h);
  h                    = chk_setup(v, k_ra8_fs_type_exfat, "T.BIN", (uint32_t)k_chk_payload_ex);
  const uint32_t fe    = exfat_first_file(h);
  const uint32_t first = disk_rd32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus);
  exfat_clear_ref_bit(h, first); /* B C1=true C2=true: referenced, bitmap bit clear */
  r = (ra8_fs_check_report_t){};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.bitmap_mismatches >= 1U);
  chk_teardown(h);
  TEST_END("MC/DC priv_exchk_diff_byte: bitmap-vs-referenced both ways");
}

int32_t main(void)
{
  test_mcdc_fat_entry();
  test_mcdc_fat_tree();
  test_mcdc_fat_diff();
  test_mcdc_fat_fsinfo();
  internal_test_mcdc_check_locked();
  test_mcdc_exchk_set();
  internal_test_mcdc_exchk_scan_dir();
  test_mcdc_exchk_diff_byte();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_check_mcdc.c\n");
  return 0;
}
