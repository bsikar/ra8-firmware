/**
 * @file test_ra8_fs_check_bounds.c
 * @brief Exact-bound, truncation, and transactional filesystem-check vectors.
 *
 * @details
 * Constructs hostile FAT and exFAT images at the checker's declared traversal
 * ceilings. Each vector independently proves that an exact valid tail remains
 * clean while a cycle, overrun, truncated entry set, or backend read failure
 * fails closed. General clean-volume and corruption detection stays in
 * `test_ra8_fs_check.c`; shared RAM-image construction is provided by the
 * existing typed test fixture rather than textual source inclusion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fs_fat_check_internal.h"
#include "test_ra8_fs_check_util.h"

/**
 * @test A FAT chain that consumes every declared cluster still validates its tail.
 *
 * @par MC/DC:
 * The exact-bound end-of-chain control returns clean. Changing only the final
 * FAT value to the in-range first cluster makes the post-bound visit report a
 * cross-link; the old ceiling exit returned a false-clean report. @brief Exercise the check fat16 exact bound tail filesystem operation. @details Runs the check fat16 exact bound tail vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_fat16_exact_bound_tail(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 validates the successor after the exact hop bound");
  ra8_fs_mount_t* h  = internal_chk_setup((uint32_t)k_fmt_blocks_fat16,
                                          k_ra8_fs_type_fat16,
                                          "A.BIN",
                                          (uint32_t)k_chk_payload_mc);
  const uint32_t  eo = internal_fat_root_entry(h, "A       BIN");
  TEST_ASSERT(eo != 0U);
  const uint32_t first =
    (uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_lo) |
    ((uint32_t)internal_disk_rd16(eo + (uint32_t)k_chk_off_fst_hi) << (uint32_t)k_chk_shl_b2);
  TEST_ASSERT_EQ((uint32_t)k_chk_first_clus, first);
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    internal_disk_wr16(internal_fat16_off(h, first + i), (uint16_t)(first + i + 1U));
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_fat16_off(h, tail);
  internal_disk_wr16(tail_off, (uint16_t)k_chk_eoc_fat16);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->count_of_clusters    = (uint32_t)k_chk_bound_clusters;
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_disk_wr16(tail_off, (uint16_t)first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->count_of_clusters = (uint32_t)k_chk_bound_clusters;
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_crosslink, r.first_fault.kind);
  TEST_ASSERT_EQ(first, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.chains_crosslinked);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 validates the successor after the exact hop bound");
}

/**
 * @test A FAT directory chain consuming every declared cluster validates its tail.
 *
 * @par MC/DC:
 * Both vectors scan the same four deleted-entry clusters. EOC after the fourth
 * cluster is clean; changing only that successor to the first cluster records a
 * cross-link at the directory walk's former false-clean ceiling. @brief Exercise the check fat16 exact bound dir tail filesystem operation. @details Runs the check fat16 exact bound dir tail vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_fat16_exact_bound_dir_tail(void)
{
  TEST_BEGIN("ra8_fs_check FAT16 directory validates its exact-bound successor");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr, 0U);
  const uint32_t first  = (uint32_t)k_chk_first_clus;
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t i = 0U; i < (uint32_t)k_chk_bound_clusters; i++) {
    const uint32_t cluster = first + i;
    const uint32_t base =
      (h->partition_base_lba + h->first_data_lba + (i * h->sectors_per_cluster)) *
      (uint32_t)k_fmt_block_size;
    memset(&s_disk.bytes[base], (int)k_chk_fat_deleted, cbytes);
    const uint16_t next = (i + 1U < (uint32_t)k_chk_bound_clusters) ? (uint16_t)(cluster + 1U)
                                                                    : (uint16_t)k_chk_eoc_fat16;
    internal_disk_wr16(internal_fat16_off(h, cluster), next);
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_fat16_off(h, tail);
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
  internal_disk_wr16(tail_off, (uint16_t)first);
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
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT16 directory validates its exact-bound successor");
}

/**
 * @test The exFAT FAT-chain walker fails closed at every exact-bound tail shape.
 *
 * @par MC/DC:
 * Three vectors share four distinct in-range clusters: an EOC tail is clean, an
 * in-range tail back to the head is a cross-link, and an out-of-range tail is a
 * bad chain. The cycle varies only terminality at the former false-clean exit. @brief Exercise the check exfat exact bound tails filesystem operation. @details Runs the check exfat exact bound tails vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_tails(void)
{
  TEST_BEGIN("ra8_fs_check exFAT fragmented chain validates exact-bound tails");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t first = (uint32_t)k_chk_first_clus;
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    internal_disk_wr32(internal_exfat_fat_off(h, (uint32_t)k_chk_first_clus + i),
                       (uint32_t)k_chk_first_clus + i + 1U);
  }
  const uint32_t tail     = (uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_exfat_fat_off(h, tail);
  internal_disk_wr32(tail_off, (uint32_t)k_chk_exfat_fat_eoc);
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
  internal_disk_wr32(tail_off, (uint32_t)k_chk_first_clus);
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
  internal_disk_wr32(tail_off, (uint32_t)k_chk_first_clus + (uint32_t)k_chk_bound_clusters);
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
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT fragmented chain validates exact-bound tails");
}

/**
 * @test An exFAT contiguous run faults instead of clamping a one-cluster overrun.
 *
 * @par MC/DC:
 * Both vectors begin at cluster two with a four-cluster volume. A declared
 * length of four is the valid exact-bound control; increasing only the length
 * to five records a bad-directory finding at the first unavailable cluster.
 * A maximum-width declaration proves the cluster count cannot wrap clean. @brief Exercise the check exfat exact bound run filesystem operation. @details Runs the check exfat exact bound run vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_run(void)
{
  TEST_BEGIN("ra8_fs_check exFAT contiguous run rejects an exact-bound overrun");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  uint8_t               visited[1] = {};
  ra8_fs_check_report_t r          = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx        = {.m           = h,
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
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT contiguous run rejects an exact-bound overrun");
}

/**
 * @test An exFAT directory-allocation chain validates its exact-bound successor.
 *
 * @par MC/DC:
 * Both vectors traverse the same four in-range clusters. EOC after the fourth
 * is clean; changing only the final successor to the first cluster records the
 * cycle as a cross-link. @brief Exercise the check exfat exact bound dir alloc filesystem operation. @details Runs the check exfat exact bound dir alloc vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_exact_bound_dir_alloc(void)
{
  TEST_BEGIN("ra8_fs_check exFAT directory allocation validates its exact-bound tail");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t first = (uint32_t)k_chk_first_clus;
  for (uint32_t i = 0U; i + 1U < (uint32_t)k_chk_bound_clusters; i++) {
    internal_disk_wr32(internal_exfat_fat_off(h, first + i), first + i + 1U);
  }
  const uint32_t tail     = first + (uint32_t)k_chk_bound_clusters - 1U;
  const uint32_t tail_off = internal_exfat_fat_off(h, tail);

  internal_disk_wr32(tail_off, (uint32_t)k_chk_exfat_fat_eoc);
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
  internal_disk_wr32(tail_off, first);
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
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT directory allocation validates its exact-bound tail");
}

/**
 * @test An exFAT directory ending exactly at the entry ceiling remains valid.
 *
 * @par MC/DC:
 * The scanner reaches `k_chk_scan_limit` without an EOD entry, then the single
 * terminal probe receives end-of-chain. This is the negative control for the
 * adjacent true-truncation vector. @brief Exercise the check exfat valid exact scan bound filesystem operation. @details Runs the check exfat valid exact scan bound vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_valid_exact_scan_bound(void)
{
  TEST_BEGIN("ra8_fs_check exFAT accepts an exact-bound directory ending at EOC");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t entries_per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_none, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT accepts an exact-bound directory ending at EOC");
}

/**
 * @test An exFAT directory with an entry beyond the ceiling reports truncation.
 *
 * @par MC/DC:
 * Geometry and the first 65536 non-EOD entries match the exact-bound control;
 * adding one allocated directory cluster varies only whether the terminal probe
 * reads EOC/exhaustion or another non-EOD entry. @brief Exercise the check exfat scan truncated filesystem operation. @details Runs the check exfat scan truncated vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_scan_truncated(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a directory beyond the entry ceiling");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t entries_per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters + 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, r.first_fault.kind);
  TEST_ASSERT_EQ(h->root_cluster, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.faults_total);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a directory beyond the entry ceiling");
}

/**
 * @test An exFAT File set cannot consume secondary entries past the scan ceiling.
 *
 * @par MC/DC:
 * The primary File entry is the 65536th scanned entry and declares one
 * secondary. The set-size check identifies truncation before the set reader can
 * reinterpret physical EOC as a generic malformed-directory finding. @brief Exercise the check exfat set straddles scan bound filesystem operation. @details Runs the check exfat set straddles scan bound vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_set_straddles_scan_bound(void)
{
  TEST_BEGIN("ra8_fs_check exFAT reports a File set crossing the entry ceiling");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  const uint32_t cbytes              = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  const uint32_t entries_per_cluster = cbytes / (uint32_t)k_chk_dir_ent;
  TEST_ASSERT_EQ(0U, (uint32_t)k_chk_scan_limit % entries_per_cluster);
  const uint32_t exact_clusters = (uint32_t)k_chk_scan_limit / entries_per_cluster;
  internal_exfat_build_bound_directory(h, exact_clusters);
  const uint32_t first_extra = internal_disk_rd32(internal_exfat_fat_off(h, h->root_cluster));
  const uint32_t last        = first_extra + exact_clusters - 2U;
  const uint32_t file        = internal_exfat_clus_off(h, last) + cbytes - (uint32_t)k_chk_dir_ent;
  memset(&s_disk.bytes[file], 0, (size_t)k_chk_dir_ent);
  s_disk.bytes[file]                                  = (uint8_t)k_chk_exfat_file;
  s_disk.bytes[file + (uint32_t)k_chk_off_file_secnt] = 1U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, r.first_fault.kind);
  TEST_ASSERT_EQ(h->root_cluster, r.first_fault.cluster);
  TEST_ASSERT_EQ(1U, r.faults_total);
  TEST_ASSERT_EQ(0U, r.entries_bad);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT reports a File set crossing the entry ceiling");
}

/**
 * @test A backend failure leaves the caller's report byte-for-byte unchanged.
 *
 * @par MC/DC:
 * The existing successful scan vectors publish a new report. This vector varies
 * only the first backend read to fail and proves the private candidate is not
 * published on the error path. @brief Exercise the check report transaction filesystem operation. @details Runs the check report transaction vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_report_transaction(void)
{
  TEST_BEGIN("ra8_fs_check publishes its report only after a complete scan");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  ra8_fs_check_report_t r = {};
  memset(&r, (int)k_chk_report_fill, sizeof(r));
  internal_fault_reset();
  s_fault_read_at = 1U;
  TEST_ASSERT(ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), &r) != k_ra8_ok);
  const uint8_t* const bytes = (const uint8_t*)&r;
  for (uint32_t i = 0U; i < (uint32_t)sizeof(r); i++) {
    TEST_ASSERT_EQ((uint8_t)k_chk_report_fill, bytes[i]);
  }
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_check publishes its report only after a complete scan");
}

int32_t main(void)
{
  internal_test_check_fat16_exact_bound_tail();
  internal_test_check_fat16_exact_bound_dir_tail();
  internal_test_check_exfat_exact_bound_tails();
  internal_test_check_exfat_exact_bound_run();
  internal_test_check_exfat_exact_bound_dir_alloc();
  internal_test_check_exfat_valid_exact_scan_bound();
  internal_test_check_exfat_scan_truncated();
  internal_test_check_exfat_set_straddles_scan_bound();
  internal_test_check_report_transaction();
  return 0;
}
