/**
 * @file test_ra8_fs_check.c
 * @brief Read-only volume consistency check (`ra8_fs_check()`, #610) -- detection.
 *
 * @details
 * Formats a RAM card as FAT12/16/32 and exFAT and asserts two things about
 * ::ra8_fs_check on each: a freshly formatted volume reports zero faults (the
 * negative control -- a checker that always fired would be worthless), and a
 * volume corrupted by a hand-written byte flip at a computed on-disk location
 * reports exactly the fault that flip introduced. Every corruption is injected by
 * an INDEPENDENT reader/writer of the RAM image (see `test_ra8_fs_check_util.h`),
 * not by the driver under test, so a bug in the driver's own walk cannot mask a
 * bug in the checker. The MC/DC vector suite is its sibling
 * `test_ra8_fs_check_mcdc.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fs_fat_check_internal.h"
#include "test_ra8_fs_check_util.h"

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 clean volume -> no faults");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                      k_ra8_fs_type_fat16,
                                      "A.BIN",
                                      (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, r.type);
  TEST_ASSERT(r.referenced_scan);
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_none, r.first_fault.kind);
  TEST_ASSERT(r.files_visited >= 1U);
  TEST_ASSERT_EQ(r.clusters_total, r.clusters_free + r.clusters_used + r.clusters_bad);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat_only_mode(void)
{
  TEST_BEGIN("ra8_fs_check without a bitmap reports counts, clusters_lost unknown");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr, 0U);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check(h, nullptr, 0U, &r));
  TEST_ASSERT(!r.referenced_scan);
  TEST_ASSERT_EQ(k_ra8_fs_check_unknown, r.clusters_lost);
  TEST_ASSERT(r.clusters_free > 0U);
  TEST_ASSERT_EQ(0U, r.dirs_visited); /* the tree is not walked without a bitmap */
  chk_teardown(h);
  TEST_END("ra8_fs_check without a bitmap reports counts, clusters_lost unknown");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_lost(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a lost cluster");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  /* Mark a spare cluster allocated (end-of-chain) that no entry references. */
  disk_wr16(fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_eoc_fat16);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.clusters_lost >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_lost_cluster, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a lost cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_crosslink(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a cross-linked chain");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                 k_ra8_fs_type_fat16,
                                 "A.BIN",
                                 (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t c0 =
    (uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  const uint16_t c1 = disk_rd16(fat16_off(h, c0));
  TEST_ASSERT((uint32_t)c1 >= (uint32_t)k_chk_first_clus); /* A.BIN spans >= 2 clusters      */
  disk_wr16(fat16_off(h, (uint32_t)c1), (uint16_t)c0);     /* loop the tail back to the head */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.chains_crosslinked >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a cross-linked chain");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_bad_dir_entry(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects an out-of-range directory entry");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                 k_ra8_fs_type_fat16,
                                 "A.BIN",
                                 (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  disk_wr16(eo + (uint32_t)k_chk_off_fst_lo, (uint16_t)k_chk_huge_lo);
  disk_wr16(eo + (uint32_t)k_chk_off_fst_hi, (uint16_t)k_chk_huge_hi);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects an out-of-range directory entry");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_bad_fat_value(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a reserved/illegal FAT value");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  disk_wr16(fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_bad_fat_val);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_fat_value, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a reserved/illegal FAT value");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat32_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 clean volume -> no faults");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_fat32,
                                      k_ra8_fs_type_fat32,
                                      "B.BIN",
                                      (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 clean volume -> no faults");
}

/* --------------------------------------------------------------------------
 * exFAT tests.
 * -------------------------------------------------------------------------- */

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_clean(void)
{
  TEST_BEGIN("ra8_fs_check exFAT clean volume -> no faults");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                      k_ra8_fs_type_exfat,
                                      "T.BIN",
                                      (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.clusters_used >= 1U);
  TEST_ASSERT(r.clusters_free >= 1U);
  TEST_ASSERT(r.files_visited >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bad_checksum(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bad SetChecksum");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                 k_ra8_fs_type_exfat,
                                 "T.BIN",
                                 (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = exfat_first_file(h);
  TEST_ASSERT(fe != 0U);
  disk_wr16(fe + (uint32_t)k_chk_off_set_csum,
            (uint16_t)(disk_rd16(fe + (uint32_t)k_chk_off_set_csum) ^ (uint16_t)k_chk_csum_flip));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_set_checksum, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bad SetChecksum");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bad_name_hash(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bad NameHash (valid checksum)");
  ra8_fs_mount_t* h     = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                    k_ra8_fs_type_exfat,
                                    "T.BIN",
                                    (uint32_t)k_chk_payload_ex);
  const uint32_t  fe    = exfat_first_file(h);
  const uint32_t  count = 1U + (uint32_t)s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt];
  uint8_t         set[k_chk_set_max_bytes] = {};
  const uint32_t  bytes                    = count * (uint32_t)k_chk_dir_ent;
  memcpy(set, &s_disk.bytes[fe], (size_t)bytes);
  /* Corrupt the NameHash, then re-seal the SetChecksum so only the hash is wrong. */
  const uint32_t hoff     = (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_hash;
  const uint16_t bad      = (uint16_t)(disk_rd16(fe + hoff) ^ (uint16_t)k_chk_hash_flip);
  set[hoff]               = (uint8_t)(bad & (uint16_t)k_chk_byte_full);
  set[hoff + 1U]          = (uint8_t)((bad >> (uint16_t)k_chk_shl_b1) & (uint16_t)k_chk_byte_full);
  const uint16_t cs       = chk_set_checksum(set, bytes);
  set[k_chk_off_set_csum] = (uint8_t)(cs & (uint16_t)k_chk_byte_full);
  set[k_chk_off_set_csum + 1U] =
    (uint8_t)((cs >> (uint16_t)k_chk_shl_b1) & (uint16_t)k_chk_byte_full);
  memcpy(&s_disk.bytes[fe], set, (size_t)bytes);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_name_hash, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bad NameHash (valid checksum)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bitmap_lost(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bitmap bit set with nothing referencing it");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                k_ra8_fs_type_exfat,
                                "T.BIN",
                                (uint32_t)k_chk_payload_ex);
  exfat_set_spare_bit(h); /* a bit set with nothing referencing it */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.clusters_lost >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_lost_cluster, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bitmap bit set with nothing referencing it");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bitmap_mismatch(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a referenced cluster with its bitmap bit clear");
  ra8_fs_mount_t* h     = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                    k_ra8_fs_type_exfat,
                                    "T.BIN",
                                    (uint32_t)k_chk_payload_ex);
  const uint32_t  fe    = exfat_first_file(h);
  const uint32_t  first = disk_rd32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus);
  exfat_clear_ref_bit(h, first); /* referenced, but its bitmap bit is now clear */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.bitmap_mismatches >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bitmap_ref_unset, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a referenced cluster with its bitmap bit clear");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat12_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT12 clean volume -> no faults");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_fat12,
                                      k_ra8_fs_type_fat12,
                                      "A.BIN",
                                      (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat12, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT12 clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_subdir(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 walks a subdirectory (clean)");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.dirs_visited >= 2U); /* root + SUB */
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 walks a subdirectory (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_bad_chain(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a chain that runs into free space");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                 k_ra8_fs_type_fat16,
                                 "A.BIN",
                                 (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t c0 =
    (uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  disk_wr16(fat16_off(h, c0), 0U); /* the head now points at a free cluster */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_chain, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a chain that runs into free space");
}

/**
 * @test A FAT chain that consumes every declared cluster still validates its tail.
 *
 * @par MC/DC:
 * The exact-bound end-of-chain control returns clean. Changing only the final
 * FAT value to the in-range first cluster makes the post-bound visit report a
 * cross-link; the old ceiling exit returned a false-clean report.
 */
RA8_INTERNAL
static void internal_test_check_fat16_exact_bound_tail(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 validates the successor after the exact hop bound");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                 k_ra8_fs_type_fat16,
                                 "A.BIN",
                                 (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t first =
    (uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  TEST_ASSERT_EQ((uint32_t)k_chk_first_clus, first);
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    disk_wr16(fat16_off(h, first + i), (uint16_t)(first + i + 1U));
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = fat16_off(h, tail);
  disk_wr16(tail_off, (uint16_t)k_chk_eoc_fat16);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->count_of_clusters    = (uint32_t)k_chk_bound_clusters;
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  disk_wr16(tail_off, (uint16_t)first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->count_of_clusters = (uint32_t)k_chk_bound_clusters;
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_crosslink, r.first_fault.kind);
  TEST_ASSERT_EQ(first, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.chains_crosslinked);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 validates the successor after the exact hop bound");
}

/**
 * @test A FAT directory chain consuming every declared cluster validates its tail.
 *
 * @par MC/DC:
 * Both vectors scan the same four deleted-entry clusters. EOC after the fourth
 * cluster is clean; changing only that successor to the first cluster records a
 * cross-link at the directory walk's former false-clean ceiling.
 */
RA8_INTERNAL
static void internal_test_check_fat16_exact_bound_dir_tail(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 directory validates its exact-bound successor");
  ra8_fs_mount_t* h     = chk_setup((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr, 0U);
  const uint32_t  first = (uint32_t)k_chk_first_clus;
  const uint32_t  cbytes = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t i = 0U; i < (uint32_t)k_chk_bound_clusters; i++) {
    const uint32_t cluster = first + i;
    const uint32_t base =
      (h->partition_base_lba + h->first_data_lba + (i * h->sectors_per_cluster)) *
      (uint32_t)k_fmt_block_size;
    memset(&s_disk.bytes[base], (int)k_chk_fat_deleted, cbytes);
    const uint16_t next = (i + 1U < (uint32_t)k_chk_bound_clusters) ? (uint16_t)(cluster + 1U)
                                                                    : (uint16_t)k_chk_eoc_fat16;
    disk_wr16(fat16_off(h, cluster), next);
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = fat16_off(h, tail);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t               visited[1] = {};
  ra8_fs_check_report_t r          = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx        = {.m           = h,
                                      .rep         = &r,
                                      .bitmap      = visited,
                                      .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_fat_scan_cluster_dir(&ctx, first));
  TEST_ASSERT_EQ(0U, r.faults_total);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  disk_wr16(tail_off, (uint16_t)first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_fat_scan_cluster_dir(&ctx, first));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_crosslink, r.first_fault.kind);
  TEST_ASSERT_EQ(first, r.first_fault.cluster);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 directory validates its exact-bound successor");
}

/**
 * @test The exFAT FAT-chain walker fails closed at every exact-bound tail shape.
 *
 * @par MC/DC:
 * Three vectors share four distinct in-range clusters: an EOC tail is clean, an
 * in-range tail back to the head is a cross-link, and an out-of-range tail is a
 * bad chain. The cycle varies only terminality at the former false-clean exit.
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_tails(void)
{
  TEST_BEGIN("ra8_fs_check exFAT fragmented chain validates exact-bound tails");
  ra8_fs_mount_t* h     = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t  first = (uint32_t)k_chk_first_clus;
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    disk_wr32(internal_exfat_fat_off(h, (uint32_t)k_chk_first_clus + i),
              (uint32_t)k_chk_first_clus + i + 1U);
  }
  const uint32_t tail     = (uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_exfat_fat_off(h, tail);
  disk_wr32(tail_off, (uint32_t)k_chk_exfat_fat_eoc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t               visited[1] = {};
  ra8_fs_check_report_t r          = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx        = {.m           = h,
                                      .rep         = &r,
                                      .bitmap      = visited,
                                      .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_exfat_mark_fatchain(&ctx, (uint32_t)k_chk_first_clus));
  TEST_ASSERT_EQ(0U, r.faults_total);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  disk_wr32(tail_off, (uint32_t)k_chk_first_clus);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_exfat_mark_fatchain(&ctx, (uint32_t)k_chk_first_clus));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_crosslink, r.first_fault.kind);
  TEST_ASSERT_EQ((uint32_t)k_chk_first_clus, r.first_fault.cluster);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  disk_wr32(tail_off, (uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_exfat_mark_fatchain(&ctx, (uint32_t)k_chk_first_clus));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_chain, r.first_fault.kind);
  TEST_ASSERT_EQ(tail, r.first_fault.cluster);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT fragmented chain validates exact-bound tails");
}

/**
 * @test An exFAT contiguous run faults instead of clamping a one-cluster overrun.
 *
 * @par MC/DC:
 * Both vectors begin at cluster two with a four-cluster volume. A declared
 * length of four is the valid exact-bound control; increasing only the length
 * to five records a bad-directory finding at the first unavailable cluster.
 * A maximum-width declaration proves the cluster count cannot wrap clean.
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_run(void)
{
  TEST_BEGIN("ra8_fs_check exFAT contiguous run rejects an exact-bound overrun");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  uint8_t         visited[1] = {};
  ra8_fs_check_report_t r    = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx  = {.m           = h,
                                .rep         = &r,
                                .bitmap      = visited,
                                .bitmap_bits = (uint32_t)k_chk_bound_clusters};

  ra8_fs_check_test_exfat_mark_run(&ctx,
                                   (uint32_t)k_chk_first_clus,
                                   (uint32_t)k_chk_bound_clusters);
  TEST_ASSERT_EQ(0U, r.faults_total);

  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_test_exfat_mark_run(&ctx,
                                   (uint32_t)k_chk_first_clus,
                                   (uint32_t)k_chk_bound_clusters + 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  TEST_ASSERT_EQ((uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters,
                 r.first_fault.cluster);

  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_test_exfat_mark_run(&ctx, (uint32_t)k_chk_first_clus, UINT64_MAX);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  TEST_ASSERT_EQ((uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters,
                 r.first_fault.cluster);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT contiguous run rejects an exact-bound overrun");
}

/**
 * @test An exFAT directory-allocation chain validates its exact-bound successor.
 *
 * @par MC/DC:
 * Both vectors traverse the same four in-range clusters. EOC after the fourth
 * is clean; changing only the final successor to the first cluster records the
 * cycle as a cross-link.
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_dir_alloc(void)
{
  TEST_BEGIN("ra8_fs_check exFAT directory allocation validates its exact-bound tail");
  ra8_fs_mount_t* h     = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t  first = (uint32_t)k_chk_first_clus;
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    disk_wr32(internal_exfat_fat_off(h, first + i), first + i + 1U);
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_exfat_fat_off(h, tail);

  disk_wr32(tail_off, (uint32_t)k_chk_exfat_fat_eoc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t               visited[1] = {};
  ra8_fs_check_report_t r          = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx        = {.m           = h,
                                      .rep         = &r,
                                      .bitmap      = visited,
                                      .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_exfat_mark_dir_alloc(&ctx, first));
  TEST_ASSERT_EQ(0U, r.faults_total);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  disk_wr32(tail_off, first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check_test_exfat_mark_dir_alloc(&ctx, first));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_crosslink, r.first_fault.kind);
  TEST_ASSERT_EQ(first, r.first_fault.cluster);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT directory allocation validates its exact-bound tail");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_defective(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 tallies a defective-marked cluster");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  disk_wr16(fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_defective);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.clusters_bad >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 tallies a defective-marked cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat32_manyfiles(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 walks a multi-cluster root directory");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat32);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint8_t payload[k_chk_small] = {1};
  for (uint32_t i = 0U; i < (uint32_t)k_chk_many; i++) {
    char name[k_chk_name_cap];
    (void)snprintf(name, sizeof(name), "F%u.BIN", i);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, name, payload, (uint32_t)k_chk_small));
  }
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.files_visited >= (uint32_t)k_chk_many);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 walks a multi-cluster root directory");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat32_fsinfo_bad(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 detects a stale FSInfo free count");
  ra8_fs_mount_t* h    = chk_setup((uint32_t)k_fmt_blocks_fat32,
                                   k_ra8_fs_type_fat32,
                                   "B.BIN",
                                   (uint32_t)k_chk_payload_mc);
  const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  const uint32_t  fsi  = (uint32_t)disk_rd16(boot + (uint32_t)k_chk_fsi_off_num);
  const uint32_t  foff = (h->partition_base_lba + fsi) * (uint32_t)k_fmt_block_size;
  disk_wr32(foff + (uint32_t)k_chk_fsi_off_free,
            disk_rd32(foff + (uint32_t)k_chk_fsi_off_free) + 1U);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_free_count_bad, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 detects a stale FSInfo free count");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_subdir(void)
{
  TEST_BEGIN("ra8_fs_check exFAT walks a subdirectory (clean)");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                k_ra8_fs_type_exfat,
                                "T.BIN",
                                (uint32_t)k_chk_payload_ex);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.dirs_visited >= 2U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT walks a subdirectory (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_fatchain(void)
{
  TEST_BEGIN("ra8_fs_check exFAT walks a fragmented (FAT-chained) file (clean)");
  alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  for (uint32_t i = 0U; i < (uint32_t)k_chk_cluster_ex; i++) {
    s_chk_payload[i] = (uint8_t)i;
  }
  ra8_fs_file_t* fa = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fa, s_chk_payload, (uint32_t)k_chk_cluster_ex));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "B.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fa, s_chk_payload, (uint32_t)k_chk_cluster_ex));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT walks a fragmented (FAT-chained) file (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_crosslink(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a cross-linked file run");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                 k_ra8_fs_type_exfat,
                                 "T.BIN",
                                 (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = exfat_first_file(h);
  disk_wr32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus, exfat_bitmap_clus(h));
  exfat_reseal(fe); /* keep the checksum valid so only the overlap is reported */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.chains_crosslinked >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a cross-linked file run");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_run_overrun(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a data run that overruns the volume");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                 k_ra8_fs_type_exfat,
                                 "T.BIN",
                                 (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = exfat_first_file(h);
  disk_wr32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_dlen,
            (uint32_t)k_chk_huge_dlen);
  exfat_reseal(fe);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a data run that overruns the volume");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_count_only(void)
{
  TEST_BEGIN("ra8_fs_check exFAT without a bitmap reports used/free from the bitmap");
  ra8_fs_mount_t*       h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                      k_ra8_fs_type_exfat,
                                      "T.BIN",
                                      (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check(h, nullptr, 0U, &r));
  TEST_ASSERT(!r.referenced_scan);
  TEST_ASSERT_EQ(k_ra8_fs_check_unknown, r.clusters_lost);
  TEST_ASSERT(r.clusters_used >= 1U);
  TEST_ASSERT(r.clusters_free >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT without a bitmap reports used/free from the bitmap");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bad_secondary(void)
{
  TEST_BEGIN("ra8_fs_check exFAT rejects a File entry with a bad SecondaryCount");
  ra8_fs_mount_t* h                                 = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                                                k_ra8_fs_type_exfat,
                                                                "T.BIN",
                                                                (uint32_t)k_chk_payload_ex);
  const uint32_t  fe                                = exfat_first_file(h);
  s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt] = 0U; /* SecondaryCount 0 -> set of one */
  ra8_fs_check_report_t r                           = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT rejects a File entry with a bad SecondaryCount");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat_read_faults(void)
{
  TEST_BEGIN("ra8_fs_check FAT propagates every backend read failure");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t*       h = nullptr;
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "A.BIN", s_chk_payload, (uint32_t)k_chk_payload_mc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB")); /* a cluster-chained subdirectory */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  /* Count the reads a COLD check makes, then fail each one in turn from a fresh
   * mount -- a cold cache every time keeps the read sequence deterministic, which
   * the shared FAT-sector cache would otherwise warm away. */
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  const uint32_t total = s_fault_read_seen;
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT(total > 0U);
  for (uint32_t n = 1U; n <= total; n++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
    fault_reset();
    s_fault_read_at = n; /* fail the nth read of this cold check */
    TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
    fault_reset();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  }
  free_volume();
  TEST_END("ra8_fs_check FAT propagates every backend read failure");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_read_faults(void)
{
  TEST_BEGIN("ra8_fs_check exFAT propagates every backend read failure");
  alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  exfat_build_rich(h); /* subdir + fragmented file -> the FAT-chain and dir-alloc read paths */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_check_report_t r = {};
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  const uint32_t total = s_fault_read_seen;
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT(total > 0U);
  for (uint32_t n = 1U; n <= total; n++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
    fault_reset();
    s_fault_read_at = n;
    TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
    fault_reset();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  }
  free_volume();
  TEST_END("ra8_fs_check exFAT propagates every backend read failure");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat16_entry_variants(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 skips label, empty and deleted entries (clean)");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_fat16,
                                k_ra8_fs_type_fat16,
                                "A.BIN",
                                (uint32_t)k_chk_payload_mc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "MYVOL")); /* a root ATTR_VOLUME_ID entry */
  ra8_fs_file_t* ef = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.BIN", k_ra8_fs_mode_write, &ef)); /* empty file */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "D.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "D.BIN")); /* a deleted 0xE5 entry */
  /* A VFAT long name lays down 0x0F entries the walk must step over. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "LongName.TXT", s_chk_payload, (uint32_t)k_chk_small));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 skips label, empty and deleted entries (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_fat32_fsinfo_untrusted(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 ignores an FSInfo it cannot trust");
  const uint32_t sig_offs[3] = {(uint32_t)k_chk_fsi_off_lead,
                                (uint32_t)k_chk_fsi_off_struct,
                                (uint32_t)k_chk_fsi_off_trail};
  for (uint32_t i = 0U; i < 3U; i++) {
    ra8_fs_mount_t* h    = chk_setup((uint32_t)k_fmt_blocks_fat32,
                                     k_ra8_fs_type_fat32,
                                     "B.BIN",
                                     (uint32_t)k_chk_payload_mc);
    const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
    const uint32_t  fsi  = (uint32_t)disk_rd16(boot + (uint32_t)k_chk_fsi_off_num);
    const uint32_t  foff = (h->partition_base_lba + fsi) * (uint32_t)k_fmt_block_size;
    disk_wr32(foff + sig_offs[i], (uint32_t)k_chk_wreck); /* wreck one FSInfo signature */
    ra8_fs_check_report_t r = {};
    TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
    TEST_ASSERT_EQ(0U, r.faults_total); /* an untrusted FSInfo is not compared */
    chk_teardown(h);
  }
  /* And an FSInfo the BPB does not point at is likewise not compared. */
  ra8_fs_mount_t* h    = chk_setup((uint32_t)k_fmt_blocks_fat32,
                                   k_ra8_fs_type_fat32,
                                   "B.BIN",
                                   (uint32_t)k_chk_payload_mc);
  const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  disk_wr16(boot + (uint32_t)k_chk_fsi_off_num, 0U); /* BPB_FSInfo -> absent */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 ignores an FSInfo it cannot trust");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_bad_first_cluster(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a Stream entry naming an out-of-range cluster");
  ra8_fs_mount_t* h   = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                  k_ra8_fs_type_exfat,
                                  "T.BIN",
                                  (uint32_t)k_chk_payload_ex);
  const uint32_t  fe  = exfat_first_file(h);
  const uint32_t  off = fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus;
  disk_wr32(off, (uint32_t)k_chk_wreck); /* a cluster the volume does not have */
  exfat_reseal(fe);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a Stream entry naming an out-of-range cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_entry_variants(void)
{
  TEST_BEGIN("ra8_fs_check exFAT skips deleted and empty entries (clean)");
  ra8_fs_mount_t* h  = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                 k_ra8_fs_type_exfat,
                                 "T.BIN",
                                 (uint32_t)k_chk_payload_ex);
  ra8_fs_file_t*  ef = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.BIN", k_ra8_fs_mode_write, &ef)); /* empty file */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "D.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "D.BIN")); /* a deleted (in-use-bit clear) entry */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT skips deleted and empty entries (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. */
static void test_check_exfat_no_bitmap(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a volume with no allocation bitmap");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat,
                                k_ra8_fs_type_exfat,
                                "T.BIN",
                                (uint32_t)k_chk_payload_ex);
  /* Clear the in-use bit on the 0x81 allocation-bitmap entry so it is not found. */
  const uint32_t base = exfat_clus_off(h, h->root_cluster);
  const uint32_t nb   = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t o = 0U; o < nb; o += (uint32_t)k_chk_dir_ent) {
    if (s_disk.bytes[base + o] == (uint8_t)k_chk_exfat_bitmap) {
      s_disk.bytes[base + o] =
        (uint8_t)((uint32_t)k_chk_exfat_bitmap & (uint32_t)k_chk_inuse_clear);
      break;
    }
  }
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a volume with no allocation bitmap");
}

/**
 * @test An exFAT directory ending exactly at the entry ceiling remains valid.
 *
 * @par MC/DC:
 * The scanner reaches `k_chk_scan_limit` without an EOD entry, then the single
 * terminal probe receives end-of-chain. This is the negative control for the
 * adjacent true-truncation vector.
 */
RA8_INTERNAL
static void internal_test_check_exfat_valid_exact_scan_bound(void)
{
  TEST_BEGIN("ra8_fs_check exFAT accepts an exact-bound directory ending at EOC");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t  entries_per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_none, r.first_fault.kind);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT accepts an exact-bound directory ending at EOC");
}

/**
 * @test An exFAT directory with an entry beyond the ceiling reports truncation.
 *
 * @par MC/DC:
 * Geometry and the first 65536 non-EOD entries match the exact-bound control;
 * adding one allocated directory cluster varies only whether the terminal probe
 * reads EOC/exhaustion or another non-EOD entry.
 */
RA8_INTERNAL
static void internal_test_check_exfat_scan_truncated(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a directory beyond the entry ceiling");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t  entries_per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters + 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, r.first_fault.kind);
  TEST_ASSERT_EQ(h->root_cluster, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.faults_total);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a directory beyond the entry ceiling");
}

/**
 * @test An exFAT File set cannot consume secondary entries past the scan ceiling.
 *
 * @par MC/DC:
 * The primary File entry is the 65536th scanned entry and declares one
 * secondary. The set-size check identifies truncation before the set reader can
 * reinterpret physical EOC as a generic malformed-directory finding.
 */
RA8_INTERNAL
static void internal_test_check_exfat_set_straddles_scan_bound(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a File set crossing the entry ceiling");
  ra8_fs_mount_t* h = chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t  cbytes              = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  const uint32_t  entries_per_cluster = cbytes / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters);
  const uint32_t first_extra = disk_rd32(internal_exfat_fat_off(h, h->root_cluster));
  const uint32_t last        = first_extra + exact_clusters - 2U;
  const uint32_t file        = exfat_clus_off(h, last) + cbytes - (uint32_t)k_chk_dir_ent;
  memset(&s_disk.bytes[file], 0, (size_t)k_chk_dir_ent);
  s_disk.bytes[file]                                  = (uint8_t)k_chk_exfat_file;
  s_disk.bytes[file + (uint32_t)k_chk_off_file_secnt] = 1U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, r.first_fault.kind);
  TEST_ASSERT_EQ(h->root_cluster, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.faults_total);
  TEST_ASSERT_EQ(0U, r.entries_bad);
  chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a File set crossing the entry ceiling");
}

/**
 * @test A backend failure leaves the caller's report byte-for-byte unchanged.
 *
 * @par MC/DC:
 * The existing successful scan vectors publish a new report. This vector varies
 * only the first backend read to fail and proves the private candidate is not
 * published on the error path.
 */
RA8_INTERNAL
static void internal_test_check_report_transaction(void)
{
  TEST_BEGIN("ra8_fs_check publishes its report only after a complete scan");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  ra8_fs_check_report_t r = {};
  memset(&r, (int)k_chk_report_fill, sizeof(r));
  fault_reset();
  s_fault_read_at = 1U;
  TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
  const uint8_t* const bytes = (const uint8_t*)&r;
  for (uint32_t i = 0U; i < (uint32_t)sizeof(r); i++) {
    TEST_ASSERT_EQ((uint8_t)k_chk_report_fill, bytes[i]);
  }
  fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_check publishes its report only after a complete scan");
}

int32_t main(void)
{
  test_check_fat16_clean();
  test_check_fat_only_mode();
  test_check_fat16_lost();
  test_check_fat16_crosslink();
  test_check_fat16_bad_dir_entry();
  test_check_fat16_bad_fat_value();
  test_check_fat16_subdir();
  test_check_fat16_bad_chain();
  internal_test_check_fat16_exact_bound_tail();
  internal_test_check_fat16_exact_bound_dir_tail();
  internal_test_check_exfat_exact_bound_tails();
  internal_test_check_exfat_exact_bound_run();
  internal_test_check_exfat_exact_bound_dir_alloc();
  test_check_fat16_defective();
  test_check_fat12_clean();
  test_check_fat32_clean();
  test_check_fat32_manyfiles();
  test_check_fat32_fsinfo_bad();
  test_check_exfat_clean();
  test_check_exfat_subdir();
  test_check_exfat_fatchain();
  test_check_exfat_bad_checksum();
  test_check_exfat_bad_name_hash();
  test_check_exfat_crosslink();
  test_check_exfat_run_overrun();
  test_check_exfat_bitmap_lost();
  test_check_exfat_bitmap_mismatch();
  test_check_exfat_count_only();
  test_check_exfat_no_bitmap();
  test_check_exfat_bad_secondary();
  test_check_fat16_entry_variants();
  test_check_fat32_fsinfo_untrusted();
  test_check_fat_read_faults();
  test_check_exfat_bad_first_cluster();
  test_check_exfat_entry_variants();
  test_check_exfat_read_faults();
  internal_test_check_exfat_valid_exact_scan_bound();
  internal_test_check_exfat_scan_truncated();
  internal_test_check_exfat_set_straddles_scan_bound();
  internal_test_check_report_transaction();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_check.c\n");
  return 0;
}
