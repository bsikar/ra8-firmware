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
 * @test FAT and exFAT worklists report only their first bounded-stack overflow.
 *
 * @details Drives both private push guards through their test-only seams with a
 *          full worklist. The first push records the dropped directory; the
 *          already-truncated control proves repeated drops do not inflate the
 *          report.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_directory_worklist_overflow(void)
{
  TEST_BEGIN("ra8_fs_check bounds FAT and exFAT directory worklists");
  ra8_fs_check_report_t report = {};
  ra8_fs_check_ctx_t    ctx    = {.rep = &report};

  ra8_fs_check_test_fat_push_overflow(&ctx, (uint32_t)k_chk_overflow_clus, false);
  TEST_ASSERT_EQ(1U, report.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, report.first_fault.kind);
  TEST_ASSERT_EQ(k_chk_overflow_clus, report.first_fault.cluster);

  report = (ra8_fs_check_report_t){};
  ra8_fs_check_test_fat_push_overflow(&ctx, (uint32_t)k_chk_overflow_clus, true);
  TEST_ASSERT_EQ(0U, report.faults_total);

  ra8_fs_check_test_exfat_push_overflow(&ctx, (uint32_t)k_chk_overflow_clus, false);
  TEST_ASSERT_EQ(1U, report.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_scan_truncated, report.first_fault.kind);
  TEST_ASSERT_EQ(k_chk_overflow_clus, report.first_fault.cluster);

  report = (ra8_fs_check_report_t){};
  ra8_fs_check_test_exfat_push_overflow(&ctx, (uint32_t)k_chk_overflow_clus, true);
  TEST_ASSERT_EQ(0U, report.faults_total);
  TEST_END("ra8_fs_check bounds FAT and exFAT directory worklists");
}

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
  TEST_ASSERT_EQ(k_chk_first_clus, first);
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
    const uint32_t base    = (uint32_t)((h->partition_base_lba + h->first_data_lba +
                                         ((uint64_t)i * h->sectors_per_cluster)) *
                                        (uint64_t)k_fmt_block_size);
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
  TEST_ASSERT_EQ(k_chk_first_clus, r.first_fault.cluster);

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
 * @test A FAT-sector read failure aborts an exFAT chained-file walk.
 *
 * @details Mounts the formatted image through the one-shot fault backend and
 * arms the first FAT sector read after mount. The direct checker helper must
 * propagate that backend failure instead of treating the chain as complete.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after `priv_fat_get()` in
 * `internal_exchk_mark_fatchain()` (1 condition).
 * - V1: the unarmed walk in `internal_test_check_exfat_exact_bound_tails()`
 *   reads the same FAT entry successfully, making the decision false.
 * - V2: this vector faults that FAT-sector read, making the decision true and
 *   returning `k_ra8_err_hw_error`.
 * N = 1 condition, N+1 = 2 vectors differing only in the backend read result.
 *
 * @pre The shared RAM disk can hold a formatted exFAT image.
 * @pre The targeted fault backend is initially disarmed.
 * @post The helper propagated the exact backend error.
 * @post The one-shot fault and mounted image were released.
 * @note Test-only; not thread-safe because the RAM disk and fault controls are shared.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_fatchain_read_fault(void)
{
  TEST_BEGIN("ra8_fs_check exFAT chained-file walk propagates a FAT read failure");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_fault_reset();
  internal_fault_read_lba((uint64_t)internal_exfat_fat_off(h, (uint32_t)k_chk_first_clus) /
                            (uint64_t)k_fmt_block_size,
                          0U);

  uint8_t               visited[1] = {};
  ra8_fs_check_report_t report     = {.clusters_total = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_ctx_t    ctx        = {.m           = h,
                                      .rep         = &report,
                                      .bitmap      = visited,
                                      .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_fs_check_test_exfat_mark_fatchain(&ctx, (uint32_t)k_chk_first_clus));
  internal_fault_reset();
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT chained-file walk propagates a FAT read failure");
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
  TEST_ASSERT_EQ(k_chk_first_clus + (uint32_t)k_chk_bound_clusters, r.first_fault.cluster);

  memset(visited, 0, sizeof(visited));
  r   = (ra8_fs_check_report_t){.clusters_total = (uint32_t)k_chk_bound_clusters};
  ctx = (ra8_fs_check_ctx_t){.m           = h,
                             .rep         = &r,
                             .bitmap      = visited,
                             .bitmap_bits = (uint32_t)k_chk_bound_clusters};
  ra8_fs_check_test_exfat_mark_run(&ctx, (uint32_t)k_chk_first_clus, UINT64_MAX);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_bad_dir_entry, r.first_fault.kind);
  TEST_ASSERT_EQ(k_chk_first_clus + (uint32_t)k_chk_bound_clusters, r.first_fault.cluster);
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
  TEST_ASSERT_EQ(0U, k_chk_scan_limit % entries_per_cluster);
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
  TEST_ASSERT_EQ(0U, k_chk_scan_limit % entries_per_cluster);
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
  TEST_ASSERT_EQ(0U, k_chk_scan_limit % entries_per_cluster);
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
    TEST_ASSERT_EQ(k_chk_report_fill, bytes[i]);
  }
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_check publishes its report only after a complete scan");
}

/**
 * @brief Format a FAT32 card cleanly, then mount it through the faulting
 *        backend with no fault armed.
 *
 * @details The format must go through the plain backend: a fault there would
 *          produce an unmountable image and prove nothing about the checker.
 *          The MOUNT is what has to be faultable, since the reads under test
 *          happen after it.
 *
 * @return The mounted handle.
 *
 * @pre No volume is currently allocated.
 * @pre The caller frees the volume via internal_chk_teardown.
 * @post A FAT32 volume is mounted through s_fault_backend.
 * @post No fault is armed on return.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_chk_fat32_fault_mount(void)
{
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat32);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_fault_reset();
  return h;
}

/**
 * @test A boot-sector read failure inside the FSInfo pass fails the check.
 *
 * @details
 * `internal_fat_fsinfo` runs only on FAT32 and is the last stage of
 * `priv_check_fat`. It re-reads sector 0 to learn where BPB_FSInfo points,
 * and that read is the ONLY read of sector 0 the whole check makes -- so an
 * LBA-targeted fault on sector 0, armed after the mount, lands exactly there
 * and nowhere else. Without the guard the checker would go on to interpret
 * whatever stale bytes the buffer held as a boot sector.
 *
 * @par MC/DC:
 * Decision: `if (be != k_ra8_ok)` after the sector-0 read in
 * libs/ra8_fs/src/ra8_fs_fat_check.c@internal_fat_fsinfo (1 condition).
 * - V1: no fault -> the read succeeds -> the decision is false and the pass
 *   continues to the FSInfo sector (control, the clean re-run below).
 * - V2: sector 0 armed to fail -> the decision is true and the backend error
 *   propagates out of ra8_fs_check (this case).
 * N = 1 condition, N+1 = 2 vectors over the identical volume, differing only
 * in the outcome of that one read. @brief Exercise the check fat32 fsinfo boot read fault filesystem operation. @details Runs the vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_fat32_fsinfo_boot_read_fault(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 surfaces a boot-sector read failure from the FSInfo pass");
  ra8_fs_mount_t*       h = internal_chk_fat32_fault_mount();
  ra8_fs_check_report_t r = {};

  internal_fault_read_lba(h->partition_base_lba, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_chk_run(h, &r));

  /* Control: the identical check over the identical volume passes once the
     fault is cleared, so the failure was the injected read and nothing else. */
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);

  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 surfaces a boot-sector read failure from the FSInfo pass");
}

/**
 * @test An FSInfo-sector read failure fails the check rather than being ignored.
 *
 * @details
 * One stage further than the boot-sector case: sector 0 is read cleanly, its
 * BPB_FSInfo field names an in-range sector inside the reserved region, and
 * the read of THAT sector is the one faulted. The FSInfo sector is touched
 * nowhere else in the check, so the LBA-targeted fault is unambiguous. A
 * missing or untrustworthy FSInfo is deliberately not a fault -- but a
 * failing READ is, and this is the line that says so.
 *
 * @par MC/DC:
 * Decision: `if (se != k_ra8_ok)` after the FSInfo-sector read in
 * libs/ra8_fs/src/ra8_fs_fat_check.c@internal_fat_fsinfo (1 condition).
 * - V1: no fault -> the read succeeds -> the decision is false and the stored
 *   free count is compared (control, the clean re-run below).
 * - V2: the FSInfo sector armed to fail -> the decision is true and the
 *   backend error propagates out of ra8_fs_check (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in that read's outcome. The
 * two also differ from the boot-sector case in WHICH sector is armed, which
 * is what separates the two returns. @brief Exercise the check fat32 fsinfo sector read fault filesystem operation. @details Runs the vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_fat32_fsinfo_sector_read_fault(void)
{
  TEST_BEGIN("ra8_fs_check FAT32 surfaces an FSInfo-sector read failure");
  ra8_fs_mount_t*       h = internal_chk_fat32_fault_mount();
  ra8_fs_check_report_t r = {};

  /* Read BPB_FSInfo out of the image independently of the driver. */
  const uint32_t boot_off = (uint32_t)(h->partition_base_lba * (uint64_t)k_fmt_block_size);
  const uint32_t fsinfo   = (uint32_t)internal_disk_rd16(boot_off + (uint32_t)k_chk_fsi_off_num);
  TEST_ASSERT(fsinfo != 0U);
  TEST_ASSERT(fsinfo < h->reserved_sectors);

  internal_fault_read_lba(h->partition_base_lba + fsinfo, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_chk_run(h, &r));

  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);

  internal_chk_teardown(h);
  TEST_END("ra8_fs_check FAT32 surfaces an FSInfo-sector read failure");
}

/**
 * @test A directory run that ends on a cluster edge with no EOD is accepted.
 *
 * @details
 * `internal_exchk_scan_dir` stops on three things: an end-of-directory entry,
 * the ::k_exfat_scan_limit entry ceiling, or the cursor running out of
 * allocation. The third is what this case drives. The sibling exact-bound
 * cases build enough clusters to reach the CEILING; here the root is left at
 * a single cluster whose every slot after the system entries is a deleted
 * remnant, so the scan consumes the cluster, `priv_exfat_next_entry` reports
 * k_ra8_err_not_found on the hop past EOC, and the scanner must treat that as
 * a clean end rather than an error or a truncation fault.
 *
 * @par MC/DC:
 * Decision: `if (r == k_ra8_err_not_found)` in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_check.c@internal_exchk_scan_dir
 * (1 condition).
 * - V1: the cursor still has entries -> the decision is false and the scan
 *   continues (control; every other exFAT directory in the suite, including
 *   the first entries of this very directory).
 * - V2: the allocation is exhausted on a cluster boundary with no EOD marker
 *   -> the decision is true and the scan returns k_ra8_ok (this case).
 * N = 1 condition, N+1 = 2 vectors. The pair differs only in whether the
 * cursor could produce another entry.
 *
 * The distinguishing observation against the truncation case is the report: a
 * run that merely ends must raise NO fault, where one that runs past the
 * entry ceiling raises k_ra8_fs_check_fault_scan_truncated. @brief Exercise the check exfat run ends on cluster edge filesystem operation. @details Runs the vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_check_exfat_run_ends_on_cluster_edge(void)
{
  TEST_BEGIN("ra8_fs_check exFAT accepts a directory run ending on a cluster edge");
  ra8_fs_mount_t* h =
    internal_chk_setup((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, nullptr, 0U);
  /* One cluster, no end-of-directory marker anywhere in it, chain at EOC. */
  internal_exfat_build_bound_directory(h, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_check_report_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_chk_run(h, &r));
  TEST_ASSERT_EQ(0U, r.faults_total);
  TEST_ASSERT_EQ(k_ra8_fs_check_fault_none, r.first_fault.kind);
  internal_chk_teardown(h);
  TEST_END("ra8_fs_check exFAT accepts a directory run ending on a cluster edge");
}

int main(void)
{
  internal_test_check_directory_worklist_overflow();
  internal_test_check_fat16_exact_bound_tail();
  internal_test_check_fat16_exact_bound_dir_tail();
  internal_test_check_exfat_exact_bound_tails();
  internal_test_check_exfat_fatchain_read_fault();
  internal_test_check_exfat_exact_bound_run();
  internal_test_check_exfat_exact_bound_dir_alloc();
  internal_test_check_exfat_valid_exact_scan_bound();
  internal_test_check_exfat_scan_truncated();
  internal_test_check_exfat_set_straddles_scan_bound();
  internal_test_check_report_transaction();
  internal_test_check_fat32_fsinfo_boot_read_fault();
  internal_test_check_fat32_fsinfo_sector_read_fault();
  internal_test_check_exfat_run_ends_on_cluster_edge();
  return 0;
}
