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
 * `test_ra8_fs_check_mcdc.c`. Exact-bound and truncation
 * fail-closed vectors live in `test_ra8_fs_check_bounds.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_fs_fat_check_internal.h"
#include "test_ra8_fs_check_util.h"

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 clean filesystem operation. @details Runs the check fat16 clean vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 clean volume -> no faults");
  ra8_fs_mount_t*       h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                               k_ra8_fs_type_fat16,
                                               "A.BIN",
                                               (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, r.type);
  TEST_ASSERT(r.referenced_scan);
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_none, r.first_fault.kind);
  TEST_ASSERT(r.files_visited >= 1U);
  TEST_ASSERT_EQ(r.clusters_total, r.clusters_free + r.clusters_used + r.clusters_bad);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat only mode filesystem operation. @details Runs the check fat only mode vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat_only_mode(void)
{
  TEST_BEGIN("ra8_fs_check without a bitmap reports counts, clusters_lost unknown");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr, 0U);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check(h, nullptr, 0U, &r));
  TEST_ASSERT(!r.referenced_scan);
  TEST_ASSERT_EQ(k_ra8_fs_check_unknown, r.clusters_lost);
  TEST_ASSERT(r.clusters_free > 0U);
  TEST_ASSERT_EQ(0U, r.dirs_visited); /* the tree is not walked without a bitmap */
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check without a bitmap reports counts, clusters_lost unknown");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 lost filesystem operation. @details Runs the check fat16 lost vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_lost(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a lost cluster");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                         k_ra8_fs_type_fat16,
                                         "A.BIN",
                                         (uint32_t)k_chk_payload_mc);
  /* Mark a spare cluster allocated (end-of-chain) that no entry references. */
  internal_disk_wr16(internal_fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_eoc_fat16);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.clusters_lost >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_lost_cluster, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a lost cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 crosslink filesystem operation. @details Runs the check fat16 crosslink vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_crosslink(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a cross-linked chain");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                          k_ra8_fs_type_fat16,
                                          "A.BIN",
                                          (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = internal_fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t c0 =
    (uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  const uint16_t c1 = internal_disk_rd16(internal_fat16_off(h, c0));
  TEST_ASSERT((uint32_t)c1 >= (uint32_t)k_chk_first_clus); /* A.BIN spans >= 2 clusters      */
  internal_disk_wr16(internal_fat16_off(h, (uint32_t)c1),
                     (uint16_t)c0); /* loop the tail back to the head */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.chains_crosslinked >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a cross-linked chain");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 bad dir entry filesystem operation. @details Runs the check fat16 bad dir entry vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_bad_dir_entry(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects an out-of-range directory entry");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                          k_ra8_fs_type_fat16,
                                          "A.BIN",
                                          (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = internal_fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  internal_disk_wr16(eo + (uint32_t)k_chk_off_fst_lo, (uint16_t)k_chk_huge_lo);
  internal_disk_wr16(eo + (uint32_t)k_chk_off_fst_hi, (uint16_t)k_chk_huge_hi);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects an out-of-range directory entry");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 bad fat value filesystem operation. @details Runs the check fat16 bad fat value vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_bad_fat_value(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a reserved/illegal FAT value");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                         k_ra8_fs_type_fat16,
                                         "A.BIN",
                                         (uint32_t)k_chk_payload_mc);
  internal_disk_wr16(internal_fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_bad_fat_val);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_fat_value, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a reserved/illegal FAT value");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat32 clean filesystem operation. @details Runs the check fat32 clean vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat32_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 clean volume -> no faults");
  ra8_fs_mount_t*       h = internal_chk_setup((uint32_t)k_fmt_blocks_fat32,
                                               k_ra8_fs_type_fat32,
                                               "B.BIN",
                                               (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 clean volume -> no faults");
}

/* --------------------------------------------------------------------------
 * exFAT tests.
 * -------------------------------------------------------------------------- */

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat clean filesystem operation. @details Runs the check exfat clean vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_clean(void)
{
  TEST_BEGIN("ra8_fs_check exFAT clean volume -> no faults");
  ra8_fs_mount_t*       h = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                               k_ra8_fs_type_exfat,
                                               "T.BIN",
                                               (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.clusters_used >= 1U);
  TEST_ASSERT(r.clusters_free >= 1U);
  TEST_ASSERT(r.files_visited >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bad checksum filesystem operation. @details Runs the check exfat bad checksum vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bad_checksum(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bad SetChecksum");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = internal_exfat_first_file(h);
  TEST_ASSERT(fe != 0U);
  internal_disk_wr16(
    fe + (uint32_t)k_chk_off_set_csum,
    (uint16_t)(internal_disk_rd16(fe + (uint32_t)k_chk_off_set_csum) ^ (uint16_t)k_chk_csum_flip));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_set_checksum, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bad SetChecksum");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bad name hash filesystem operation. @details Runs the check exfat bad name hash vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bad_name_hash(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bad NameHash (valid checksum)");
  ra8_fs_mount_t* h     = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                             k_ra8_fs_type_exfat,
                                             "T.BIN",
                                             (uint32_t)k_chk_payload_ex);
  const uint32_t  fe    = internal_exfat_first_file(h);
  const uint32_t  count = 1U + (uint32_t)s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt];
  uint8_t         set[k_chk_set_max_bytes] = {};
  const uint32_t  bytes                    = count * (uint32_t)k_chk_dir_ent;
  memcpy(set, &s_disk.bytes[fe], (size_t)bytes);
  /* Corrupt the NameHash, then re-seal the SetChecksum so only the hash is wrong. */
  const uint32_t hoff     = (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_hash;
  const uint16_t bad      = (uint16_t)(internal_disk_rd16(fe + hoff) ^ (uint16_t)k_chk_hash_flip);
  set[hoff]               = (uint8_t)(bad & (uint16_t)k_chk_byte_full);
  set[hoff + 1U]          = (uint8_t)((bad >> (uint16_t)k_chk_shl_b1) & (uint16_t)k_chk_byte_full);
  const uint16_t cs       = internal_chk_set_checksum(set, bytes);
  set[k_chk_off_set_csum] = (uint8_t)(cs & (uint16_t)k_chk_byte_full);
  set[k_chk_off_set_csum + 1U] =
    (uint8_t)((cs >> (uint16_t)k_chk_shl_b1) & (uint16_t)k_chk_byte_full);
  memcpy(&s_disk.bytes[fe], set, (size_t)bytes);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_name_hash, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bad NameHash (valid checksum)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bitmap lost filesystem operation. @details Runs the check exfat bitmap lost vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bitmap_lost(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a bitmap bit set with nothing referencing it");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                         k_ra8_fs_type_exfat,
                                         "T.BIN",
                                         (uint32_t)k_chk_payload_ex);
  internal_exfat_set_spare_bit(h); /* a bit set with nothing referencing it */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.clusters_lost >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_lost_cluster, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a bitmap bit set with nothing referencing it");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bitmap mismatch filesystem operation. @details Runs the check exfat bitmap mismatch vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bitmap_mismatch(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a referenced cluster with its bitmap bit clear");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = internal_exfat_first_file(h);
  const uint32_t  first =
    internal_disk_rd32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus);
  internal_exfat_clear_ref_bit(h, first); /* referenced, but its bitmap bit is now clear */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.bitmap_mismatches >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bitmap_ref_unset, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a referenced cluster with its bitmap bit clear");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat12 clean filesystem operation. @details Runs the check fat12 clean vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat12_clean(void)
{
  TEST_BEGIN("ra8_fs_check FAT12 clean volume -> no faults");
  ra8_fs_mount_t*       h = internal_chk_setup((uint32_t)k_fmt_blocks_fat12,
                                               k_ra8_fs_type_fat12,
                                               "A.BIN",
                                               (uint32_t)k_chk_payload_mc);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat12, r.type);
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT12 clean volume -> no faults");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 subdir filesystem operation. @details Runs the check fat16 subdir vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_subdir(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 walks a subdirectory (clean)");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                         k_ra8_fs_type_fat16,
                                         "A.BIN",
                                         (uint32_t)k_chk_payload_mc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.dirs_visited >= 2U); /* root + SUB */
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 walks a subdirectory (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 bad chain filesystem operation. @details Runs the check fat16 bad chain vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_bad_chain(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 detects a chain that runs into free space");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                          k_ra8_fs_type_fat16,
                                          "A.BIN",
                                          (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = internal_fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t c0 =
    (uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  internal_disk_wr16(internal_fat16_off(h, c0), 0U); /* the head now points at a free cluster */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_chain, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 detects a chain that runs into free space");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 defective filesystem operation. @details Runs the check fat16 defective vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_defective(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 tallies a defective-marked cluster");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                         k_ra8_fs_type_fat16,
                                         "A.BIN",
                                         (uint32_t)k_chk_payload_mc);
  internal_disk_wr16(internal_fat16_off(h, h->count_of_clusters), (uint16_t)k_chk_defective);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.clusters_bad >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 tallies a defective-marked cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat32 manyfiles filesystem operation. @details Runs the check fat32 manyfiles vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat32_manyfiles(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 walks a multi-cluster root directory");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat32);
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
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.files_visited >= (uint32_t)k_chk_many);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 walks a multi-cluster root directory");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat32 fsinfo bad filesystem operation. @details Runs the check fat32 fsinfo bad vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat32_fsinfo_bad(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 detects a stale FSInfo free count");
  ra8_fs_mount_t* h    = internal_chk_setup((uint32_t)k_fmt_blocks_fat32,
                                            k_ra8_fs_type_fat32,
                                            "B.BIN",
                                            (uint32_t)k_chk_payload_mc);
  const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  const uint32_t  fsi  = (uint32_t)internal_disk_rd16(boot + (uint32_t)k_chk_fsi_off_num);
  const uint32_t  foff = (h->partition_base_lba + fsi) * (uint32_t)k_fmt_block_size;
  internal_disk_wr32(foff + (uint32_t)k_chk_fsi_off_free,
                     internal_disk_rd32(foff + (uint32_t)k_chk_fsi_off_free) + 1U);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_free_count_bad, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 detects a stale FSInfo free count");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat subdir filesystem operation. @details Runs the check exfat subdir vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_subdir(void)
{
  TEST_BEGIN("ra8_fs_check exFAT walks a subdirectory (clean)");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                         k_ra8_fs_type_exfat,
                                         "T.BIN",
                                         (uint32_t)k_chk_payload_ex);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT(r.dirs_visited >= 2U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT walks a subdirectory (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat fatchain filesystem operation. @details Runs the check exfat fatchain vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_fatchain(void)
{
  TEST_BEGIN("ra8_fs_check exFAT walks a fragmented (FAT-chained) file (clean)");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
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
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT walks a fragmented (FAT-chained) file (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat crosslink filesystem operation. @details Runs the check exfat crosslink vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_crosslink(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a cross-linked file run");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = internal_exfat_first_file(h);
  internal_disk_wr32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus,
                     internal_exfat_bitmap_clus(h));
  internal_exfat_reseal(fe); /* keep the checksum valid so only the overlap is reported */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.chains_crosslinked >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a cross-linked file run");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat run overrun filesystem operation. @details Runs the check exfat run overrun vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_run_overrun(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a data run that overruns the volume");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = internal_exfat_first_file(h);
  internal_disk_wr32(fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_dlen,
                     (uint32_t)k_chk_huge_dlen);
  internal_exfat_reseal(fe);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a data run that overruns the volume");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat count only filesystem operation. @details Runs the check exfat count only vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_count_only(void)
{
  TEST_BEGIN("ra8_fs_check exFAT without a bitmap reports used/free from the bitmap");
  ra8_fs_mount_t*       h = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                               k_ra8_fs_type_exfat,
                                               "T.BIN",
                                               (uint32_t)k_chk_payload_ex);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check(h, nullptr, 0U, &r));
  TEST_ASSERT(!r.referenced_scan);
  TEST_ASSERT_EQ(k_ra8_fs_check_unknown, r.clusters_lost);
  TEST_ASSERT(r.clusters_used >= 1U);
  TEST_ASSERT(r.clusters_free >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT without a bitmap reports used/free from the bitmap");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bad secondary filesystem operation. @details Runs the check exfat bad secondary vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bad_secondary(void)
{
  TEST_BEGIN("ra8_fs_check exFAT rejects a File entry with a bad SecondaryCount");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  const uint32_t  fe = internal_exfat_first_file(h);
  s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt] = 0U; /* SecondaryCount 0 -> set of one */
  ra8_fs_check_report_t r                           = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT rejects a File entry with a bad SecondaryCount");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat read faults filesystem operation. @details Runs the check fat read faults vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat_read_faults(void)
{
  TEST_BEGIN("ra8_fs_check FAT propagates every backend read failure");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
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
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  const uint32_t total = s_fault_read_seen;
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT(total > 0U);
  for (uint32_t n = 1U; n <= total; n++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
    internal_fault_reset();
    s_fault_read_at = n; /* fail the nth read of this cold check */
    TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
    internal_fault_reset();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  }
  internal_free_volume();
  TEST_END("ra8_fs_check FAT propagates every backend read failure");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat read faults filesystem operation. @details Runs the check exfat read faults vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_read_faults(void)
{
  TEST_BEGIN("ra8_fs_check exFAT propagates every backend read failure");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_exfat_build_rich(
    h); /* subdir + fragmented file -> the FAT-chain and dir-alloc read paths */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_check_report_t r = {};
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  const uint32_t total = s_fault_read_seen;
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT(total > 0U);
  for (uint32_t n = 1U; n <= total; n++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
    internal_fault_reset();
    s_fault_read_at = n;
    TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
    internal_fault_reset();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  }
  internal_free_volume();
  TEST_END("ra8_fs_check exFAT propagates every backend read failure");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat16 entry variants filesystem operation. @details Runs the check fat16 entry variants vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat16_entry_variants(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 skips label, empty and deleted entries (clean)");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
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
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 skips label, empty and deleted entries (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check fat32 fsinfo untrusted filesystem operation. @details Runs the check fat32 fsinfo untrusted vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_fat32_fsinfo_untrusted(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 ignores an FSInfo it cannot trust");
  const uint32_t sig_offs[3] = {(uint32_t)k_chk_fsi_off_lead,
                                (uint32_t)k_chk_fsi_off_struct,
                                (uint32_t)k_chk_fsi_off_trail};
  for (uint32_t i = 0U; i < 3U; i++) {
    ra8_fs_mount_t* h    = internal_chk_setup((uint32_t)k_fmt_blocks_fat32,
                                              k_ra8_fs_type_fat32,
                                              "B.BIN",
                                              (uint32_t)k_chk_payload_mc);
    const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
    const uint32_t  fsi  = (uint32_t)internal_disk_rd16(boot + (uint32_t)k_chk_fsi_off_num);
    const uint32_t  foff = (h->partition_base_lba + fsi) * (uint32_t)k_fmt_block_size;
    internal_disk_wr32(foff + sig_offs[i], (uint32_t)k_chk_wreck); /* wreck one FSInfo signature */
    ra8_fs_check_report_t r = {};
    TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
    TEST_ASSERT_EQ(0U, r.faults_total); /* an untrusted FSInfo is not compared */
    internal_chk_teardown(h);
  }
  /* And an FSInfo the BPB does not point at is likewise not compared. */
  ra8_fs_mount_t* h    = internal_chk_setup((uint32_t)k_fmt_blocks_fat32,
                                            k_ra8_fs_type_fat32,
                                            "B.BIN",
                                            (uint32_t)k_chk_payload_mc);
  const uint32_t  boot = h->partition_base_lba * (uint32_t)k_fmt_block_size;
  internal_disk_wr16(boot + (uint32_t)k_chk_fsi_off_num, 0U); /* BPB_FSInfo -> absent */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 ignores an FSInfo it cannot trust");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat bad first cluster filesystem operation. @details Runs the check exfat bad first cluster vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_bad_first_cluster(void)
{
  TEST_BEGIN("ra8_fs_check exFAT detects a Stream entry naming an out-of-range cluster");
  ra8_fs_mount_t* h   = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                           k_ra8_fs_type_exfat,
                                           "T.BIN",
                                           (uint32_t)k_chk_payload_ex);
  const uint32_t  fe  = internal_exfat_first_file(h);
  const uint32_t  off = fe + (uint32_t)k_chk_dir_ent + (uint32_t)k_chk_strm_off_clus;
  internal_disk_wr32(off, (uint32_t)k_chk_wreck); /* a cluster the volume does not have */
  internal_exfat_reseal(fe);
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT detects a Stream entry naming an out-of-range cluster");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat entry variants filesystem operation. @details Runs the check exfat entry variants vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_entry_variants(void)
{
  TEST_BEGIN("ra8_fs_check exFAT skips deleted and empty entries (clean)");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                          k_ra8_fs_type_exfat,
                                          "T.BIN",
                                          (uint32_t)k_chk_payload_ex);
  ra8_fs_file_t*  ef = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.BIN", k_ra8_fs_mode_write, &ef)); /* empty file */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "D.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "D.BIN")); /* a deleted (in-use-bit clear) entry */
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT skips deleted and empty entries (clean)");
}

/** @par MC/DC: detection test; the compound decisions it exercises have their
 *  vector suite in `test_ra8_fs_check_mcdc.c`. @brief Exercise the check exfat no bitmap filesystem operation. @details Runs the check exfat no bitmap vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_check_exfat_no_bitmap(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a volume with no allocation bitmap");
  ra8_fs_mount_t* h = internal_chk_setup((uint32_t)k_fmt_blocks_exfat,
                                         k_ra8_fs_type_exfat,
                                         "T.BIN",
                                         (uint32_t)k_chk_payload_ex);
  /* Clear the in-use bit on the 0x81 allocation-bitmap entry so it is not found. */
  const uint32_t base = internal_exfat_clus_off(h, h->root_cluster);
  const uint32_t nb   = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t o = 0U; o < nb; o += (uint32_t)k_chk_dir_ent) {
    if (s_disk.bytes[base + o] == (uint8_t)k_chk_exfat_bitmap) {
      s_disk.bytes[base + o] =
        (uint8_t)((uint32_t)k_chk_exfat_bitmap & (uint32_t)k_chk_inuse_clear);
      break;
    }
  }
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT(r.entries_bad >= 1U);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a volume with no allocation bitmap");
}

int32_t main(void)
{
  internal_test_check_fat16_clean();
  internal_test_check_fat_only_mode();
  internal_test_check_fat16_lost();
  internal_test_check_fat16_crosslink();
  internal_test_check_fat16_bad_dir_entry();
  internal_test_check_fat16_bad_fat_value();
  internal_test_check_fat16_subdir();
  internal_test_check_fat16_bad_chain();
  internal_test_check_fat16_defective();
  internal_test_check_fat12_clean();
  internal_test_check_fat32_clean();
  internal_test_check_fat32_manyfiles();
  internal_test_check_fat32_fsinfo_bad();
  internal_test_check_exfat_clean();
  internal_test_check_exfat_subdir();
  internal_test_check_exfat_fatchain();
  internal_test_check_exfat_bad_checksum();
  internal_test_check_exfat_bad_name_hash();
  internal_test_check_exfat_crosslink();
  internal_test_check_exfat_run_overrun();
  internal_test_check_exfat_bitmap_lost();
  internal_test_check_exfat_bitmap_mismatch();
  internal_test_check_exfat_count_only();
  internal_test_check_exfat_no_bitmap();
  internal_test_check_exfat_bad_secondary();
  internal_test_check_fat16_entry_variants();
  internal_test_check_fat32_fsinfo_untrusted();
  internal_test_check_fat_read_faults();
  internal_test_check_exfat_bad_first_cluster();
  internal_test_check_exfat_entry_variants();
  internal_test_check_exfat_read_faults();
  return 0;
}
