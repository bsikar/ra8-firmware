/**
 * @file test_ra8_fs_exfat_blockdev_cov.c
 * @brief Block-device and bounded-scan coverage for exFAT mutation helpers.
 *
 * @details Uses the existing RAM-backed fault backend for exact read/write
 * failures. The scan cases construct real 2 MiB directory images containing
 * exactly ::k_exfat_scan_limit retired entries, without a production seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat_exfat_mutate_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "ra8_fs_meta.h"
#include "unity_minimal.h"

/** @enum blockdev_cov_const_t @brief Stable fixture values for these vectors. */
typedef enum : uint32_t {
  k_bcov_retired_type = 0x05U, /**< In-use-clear, non-EOD entry.    */
  k_bcov_fake_root    = 1024U, /**< Synthetic root's first cluster. */
  k_bcov_far_cluster  = 128U,  /**< FAT sector 1 cluster.           */
} blockdev_cov_const_t;

/** @brief Return @p cluster's byte offset in the RAM disk. */
RA8_INTERNAL static uint32_t internal_cluster_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint64_t lba =
    h->partition_base_lba + h->first_data_lba +
    ((uint64_t)(cluster - (uint32_t)k_mut_cluster_first) * h->sectors_per_cluster);
  return (uint32_t)(lba * (uint64_t)k_mut_block_size);
}

/** @brief Fill and FAT-link a real retired-entry directory chain. */
RA8_INTERNAL static void
internal_make_retired_chain(const ra8_fs_mount_t* h, uint32_t first, uint32_t count)
{
  const uint32_t cluster_bytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  TEST_ASSERT(first >= (uint32_t)k_mut_cluster_first);
  TEST_ASSERT((first + count) <= (h->count_of_clusters + (uint32_t)k_mut_cluster_first));
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t cluster = first + i;
    const uint32_t base    = internal_cluster_byte(h, cluster);
    memset(&s_disk.bytes[base], 0, (size_t)cluster_bytes);
    for (uint32_t off = 0U; off < cluster_bytes; off += (uint32_t)k_mut_entry_bytes) {
      s_disk.bytes[base + off] = (uint8_t)k_bcov_retired_type;
    }
    const uint32_t next = ((i + 1U) < count) ? (cluster + 1U) : (uint32_t)k_mut_fat_eoc;
    internal_disk_set_u32le(internal_fat_byte(h, cluster), next);
  }
}

/**
 * @test internal_test_label_scan_bound
 * @brief A label-free, no-EOD root reports not-found at the scan bound.
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post ::ra8_fs_get_label reports ::k_ra8_err_not_found.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_scan_bound(void)
{
  TEST_BEGIN("exfat blockdev cov: label scan reaches its bound");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cluster_bytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t clusters =
    ((uint32_t)k_exfat_scan_limit * (uint32_t)k_exfat_entry_bytes) / cluster_bytes;
  const uint32_t saved_root = h->root_cluster;
  internal_make_retired_chain(h, (uint32_t)k_bcov_fake_root, clusters);
  h->root_cluster                = (uint32_t)k_bcov_fake_root;
  char label[k_ra8_fs_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_get_label(h, label, (uint32_t)sizeof(label)));
  h->root_cluster = saved_root;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: label scan reaches its bound");
}

/**
 * @test internal_test_find_set_rejects_missing_eod_at_bound
 * @brief A directory with no EOD marker is corruption, not a missing name.
 * @par MC/DC:
 * The scan-bound return has no decision of its own. The exact-size retired
 * chain drives the loop condition false after 65,536 entries; ordinary
 * missing-name tests take the EOD return from inside the loop instead.
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Unlink reports ::k_ra8_err_protocol_error at the exact scan bound.
 * @note The synthetic root is restored before unmounting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_find_set_rejects_missing_eod_at_bound(void)
{
  TEST_BEGIN("exfat blockdev cov: find-set rejects a no-EOD bounded directory");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cluster_bytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t clusters =
    ((uint32_t)k_exfat_scan_limit * (uint32_t)k_exfat_entry_bytes) / cluster_bytes;
  const uint32_t saved_root = h->root_cluster;
  internal_make_retired_chain(h, (uint32_t)k_bcov_fake_root, clusters);
  h->root_cluster = (uint32_t)k_bcov_fake_root;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_fs_unlink(h, "/MISSING.TXT"));
  h->root_cluster = saved_root;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: find-set rejects a no-EOD bounded directory");
}

/**
 * @test internal_test_empty_directory_scan_bound
 * @brief An exact 65,536-entry retired directory is empty and removable.
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Removal restores the allocation census.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_empty_directory_scan_bound(void)
{
  TEST_BEGIN("exfat blockdev cov: empty directory reaches its scan bound");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t before = internal_alloc_bitmap_used(h);
  const uint32_t bytes  = (uint32_t)k_exfat_scan_limit * (uint32_t)k_exfat_entry_bytes;
  uint8_t*       image  = (uint8_t*)malloc((size_t)bytes);
  TEST_ASSERT_NOT_NULL(image);
  memset(image, 0, (size_t)bytes);
  for (uint32_t off = 0U; off < bytes; off += (uint32_t)k_exfat_entry_bytes) {
    image[off] = (uint8_t)k_bcov_retired_type;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BOUND", image, bytes));
  free(image);
  const uint32_t file_off = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_mut_file_attr_off] |= (uint8_t)k_mut_attr_directory;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/BOUND"));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: empty directory reaches its scan bound");
}

/**
 * @test internal_test_free_cluster_read_faults
 * @brief Bitmap discovery and FAT-chain read failures propagate exactly.
 * @pre A freshly formatted, mounted exFAT volume.
 * @post Both calls report the injected backend error.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_free_cluster_read_faults(void)
{
  TEST_BEGIN("exfat blockdev cov: free-cluster read errors propagate");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t stream[k_exfat_entry_bytes] = {};
  priv_wr32(&stream[k_exfat_strm_off_clus], (uint32_t)k_bcov_far_cluster);
  priv_wr64(&stream[k_exfat_strm_off_dlen], 1U);
  priv_alloc_state_release(h);
  priv_alloc_state_bind(h);
  s_mut_rd_fail_in = 0;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, priv_exfat_free_clusters(h, stream));
  s_mut_rd_fail_in = (int32_t)k_mut_fault_never;

  uint64_t bitmap_lba = 0U;
  uint32_t ignored    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_exfat_bitmap_lba(h, &bitmap_lba));
  TEST_ASSERT_EQ(k_ra8_ok, priv_fat_get(h, h->root_cluster, &ignored));
  TEST_ASSERT((internal_fat_byte(h, h->root_cluster) / (uint32_t)k_mut_block_size) !=
              (internal_fat_byte(h, (uint32_t)k_bcov_far_cluster) / (uint32_t)k_mut_block_size));
  s_mut_rd_fail_in = 1;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, priv_exfat_free_clusters(h, stream));
  s_mut_rd_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: free-cluster read errors propagate");
}

/**
 * @test internal_test_mutation_write_faults
 * @brief Unlink, relocated-rename, and in-place rename write errors propagate.
 * @pre Each mounted fixture holds `A.TXT`.
 * @post Every injected error propagates without deleting the old name.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mutation_write_faults(void)
{
  TEST_BEGIN("exfat blockdev cov: mutation write errors preserve old names");
  const uint8_t payload = (uint8_t)'A';
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &payload, 1U));
  s_mut_wr_fail_in = 0;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_unlink(h, "A.TXT"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "A.TXT", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();

  internal_build_exfat_volume();
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &payload, 1U));
  s_mut_wr_fail_in = 0;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_rename(h, "A.TXT", "ABCDEFGHIJKLMNOP"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "A.TXT", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "ABCDEFGHIJKLMNOP", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();

  internal_build_exfat_volume();
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &payload, 1U));
  s_mut_wr_fail_in = 0;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_rename(h, "A.TXT", "B.TXT"));
  s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "A.TXT", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "B.TXT", &(ra8_fs_stat_t){}));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: mutation write errors preserve old names");
}

/**
 * @test internal_test_list_entry_read_faults
 * @brief Stream and Name read failures propagate from directory emission.
 * @pre A mounted exFAT volume holds one short-named file.
 * @post Both failures propagate and a clean control emits the entry.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_list_entry_read_faults(void)
{
  TEST_BEGIN("exfat blockdev cov: list emission propagates entry read errors");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint8_t payload = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &payload, 1U));
  exfat_cursor_t  cur     = {.cluster          = h->root_cluster,
                             .entry_in_cluster = (uint32_t)k_mut_root_file0_idx,
                             .scanned          = 0U,
                             .contig_end       = 0U};
  ra8_fs_dirent_t entry   = {};
  bool            present = false;
  s_mut_rd_fail_in        = 1;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, priv_exfat_dir_next(h, &cur, &entry, &present));
  cur              = (exfat_cursor_t){.cluster          = h->root_cluster,
                                      .entry_in_cluster = (uint32_t)k_mut_root_file0_idx,
                                      .scanned          = 0U,
                                      .contig_end       = 0U};
  s_mut_rd_fail_in = 2;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, priv_exfat_dir_next(h, &cur, &entry, &present));
  s_mut_rd_fail_in = (int32_t)k_mut_fault_never;
  cur              = (exfat_cursor_t){.cluster          = h->root_cluster,
                                      .entry_in_cluster = (uint32_t)k_mut_root_file0_idx,
                                      .scanned          = 0U,
                                      .contig_end       = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, priv_exfat_dir_next(h, &cur, &entry, &present));
  TEST_ASSERT_EQ(true, present);
  TEST_ASSERT_EQ(0, strcmp("A.TXT", entry.name));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat blockdev cov: list emission propagates entry read errors");
}

/** @brief Run every exFAT block-device coverage vector. */
int main(void)
{
  internal_test_label_scan_bound();
  internal_test_find_set_rejects_missing_eod_at_bound();
  internal_test_empty_directory_scan_bound();
  internal_test_free_cluster_read_faults();
  internal_test_mutation_write_faults();
  internal_test_list_entry_read_faults();
  return 0;
}
