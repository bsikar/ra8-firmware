/**
 * @file fs_fat_exfat_mutate_test_util.h
 * @brief Shared fixture for test_ra8_fs_fat_exfat_mutate_cov.c.
 *
 * @details
 * Header-only test fixture providing the RAM-backed block device, the 64 MiB
 * exFAT volume builder, and the raw directory-entry / FAT patching helpers
 * used by the exFAT mutate (unlink / rename / listdir corruption) coverage
 * suite. Everything here has internal linkage, so the including test
 * executable owns a private copy of the fixture state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/* ---- sizing constants --------------------------------------------------- */

/**
 * @enum mut_cov_const_t
 * @brief Sizing / offset constants for the exFAT mutate coverage tests.
 *
 * @details All magic numbers in this file appear here as named enum values.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mut_block_size      = 512U,        /**< Bytes per sector.                      */
  k_mut_blocks_exfat    = 131072U,     /**< 64 MiB -- smallest workable exFAT vol. */
  k_mut_entry_bytes     = 32U,         /**< Directory entry size.                  */
  k_mut_strm_off_clus   = 0x14U,       /**< Stream-ext FirstCluster byte offset.   */
  k_mut_strm_off_dlen   = 0x18U,       /**< Stream-ext DataLength byte offset.     */
  k_mut_strm_off_flags  = 1U,          /**< Stream-ext GeneralSecondaryFlags byte. */
  k_mut_file_secnt_off  = 1U,          /**< File entry SecondaryCount byte.        */
  k_mut_payload_two_cls = 5000U,       /**< Payload > 1 cluster -> two clusters.   */
  k_mut_fat_eoc         = 0xFFFFFFFFU, /**< exFAT end-of-chain FAT value.          */
  k_mut_no_fat_bit      = 0x02U,       /**< NoFatChain bit in SecondaryFlags.      */
  k_mut_max_secondary   = 19U,         /**< k_exfat_set_max_entries from source.   */
  k_mut_root_bitmap_idx = 0U,          /**< Root dir: bitmap entry index.          */
  k_mut_root_upcase_idx = 1U,          /**< Root dir: up-case entry index.         */
  k_mut_root_label_idx  = 2U,          /**< Root dir: label entry index.           */
  k_mut_root_file0_idx  = 3U,          /**< Root dir: first user File entry.       */
  k_mut_root_strm0_idx  = 4U,          /**< Root dir: first user Stream entry.     */
  k_mut_root_name0_idx  = 5U,          /**< Root dir: first user Name entry.       */
  k_mut_root_file1_idx  = 6U,          /**< Root dir: second user File entry.      */
  k_mut_shift_byte8     = 8U,          /**< 8-bit left shift for LE16/LE32 packs.  */
  k_mut_shift_byte16    = 16U,         /**< 16-bit left shift.                     */
  k_mut_shift_byte24    = 24U,         /**< 24-bit left shift.                     */
  k_mut_mask_byte       = 0xFFU,       /**< Byte mask.                             */
  k_mut_type_file       = 0x85U,       /**< exFAT File entry type byte.            */
  k_mut_type_stream     = 0xC0U,       /**< exFAT Stream-extension type byte.      */
  k_mut_type_name       = 0xC1U,       /**< exFAT File-name type byte.             */
  k_mut_type_bogus      = 0x42U,       /**< Invalid type used to corrupt entries.  */
  k_mut_fill_byte       = 0xA5U,       /**< Pre-format poison fill pattern.        */
} mut_cov_const_t;

/* ---- RAM-backed block device ------------------------------------------- */

/**
 * @struct mut_disk_t
 * @brief Memory-backed disk handed to `ra8_fs` as a block device.
 *
 * @details Sector store is a flat `malloc` buffer.  All reads and writes go
 *          straight to this buffer, making it trivial to corrupt directory
 *          entries by patching individual bytes after normal filesystem ops.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mut_disk_t;

/**
 * @struct mut_list_ctx_t
 * @brief Listdir callback context for asserting entry counts.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t count; /**< Number of directory entries reported. */
} mut_list_ctx_t;

/** @var s_disk
 * @brief Module-level RAM disk instance shared across all tests.
 * @warning Modify only through `build_exfat_volume()` / `free_volume()`.
 * @since 0.1.0
 */
static mut_disk_t s_disk = {};

/**
 * @brief `ra8_fs` backend: read @p count sectors from the RAM disk.
 *
 * @param[in]  ctx   Pointer to `mut_disk_t`.
 * @param[in]  lba   Start sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok Sectors copied into @p buf.
 * @retval k_ra8_err_out_of_range Request exceeds disk size.
 *
 * @pre ctx and buf are non-NULL.
 * @post buf contains the requested sectors on success.
 *
 * @since 0.1.0
 */
static inline ra8_err_t mut_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mut_disk_t* d = (const mut_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_mut_block_size],
         (size_t)count * (size_t)k_mut_block_size);
  return k_ra8_ok;
}

/**
 * @brief `ra8_fs` backend: write @p count sectors to the RAM disk.
 *
 * @param[in] ctx   Pointer to `mut_disk_t`.
 * @param[in] lba   Start sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok Sectors copied from @p buf.
 * @retval k_ra8_err_out_of_range Request exceeds disk size.
 *
 * @pre ctx and buf are non-NULL.
 * @post Sector data in the RAM disk equals @p buf on success.
 *
 * @since 0.1.0
 */
static inline ra8_err_t mut_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mut_disk_t* d = (mut_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_mut_block_size],
         buf,
         (size_t)count * (size_t)k_mut_block_size);
  return k_ra8_ok;
}

/**
 * @brief `ra8_fs` backend: report disk geometry.
 *
 * @param[in]  ctx         Pointer to `mut_disk_t`.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry populated.
 *
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect the disk dimensions.
 *
 * @since 0.1.0
 */
static inline ra8_err_t mut_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const mut_disk_t* d = (const mut_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_mut_block_size;
  return k_ra8_ok;
}

/** @var s_backend
 * @brief Shared backend wired to `s_disk` for all tests.
 * @since 0.1.0
 */
static const ra8_fs_backend_t s_backend = {
  .read_block   = mut_read,
  .write_block  = mut_write,
  .get_capacity = mut_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ---------------------------------------------------- */

/**
 * @brief Release the heap buffer held by `s_disk`.
 *
 * @pre None.
 * @post `s_disk.bytes` is `nullptr`.
 *
 * @since 0.1.0
 */
static inline void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Allocate a fresh garbage-filled 64 MiB RAM disk and format as exFAT.
 *
 * @details Pre-fills with 0xA5 so a valid format proves the formatter
 *          actually wrote the VBR (not a lucky all-zero read).  No label is
 *          written so the root directory starts with:
 *          [bitmap | upcase | label(empty) | EOD ...].
 *
 * @pre `s_disk.bytes` is `nullptr` (or call `free_volume()` first).
 * @post `s_disk` holds a freshly formatted exFAT image.
 *
 * @since 0.1.0
 */
static inline void build_exfat_volume(void)
{
  free_volume();
  const size_t total = (size_t)k_mut_blocks_exfat * (size_t)k_mut_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_mut_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, (int)k_mut_fill_byte, total);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Listdir callback that increments the count in @p ctx.
 *
 * @param[in] name Unused.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a `mut_list_ctx_t`.
 *
 * @pre ctx is non-NULL.
 * @post `((mut_list_ctx_t*)ctx)->count` is incremented by one.
 *
 * @since 0.1.0
 */
static inline void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  ((mut_list_ctx_t*)ctx)->count++;
}

/**
 * @brief Byte offset in `s_disk.bytes` for root-dir entry @p idx.
 *
 * @details Every LBA cached in `ra8_fs_mount_t` is PARTITION-relative -- the
 *          driver adds `partition_base_lba` inside `priv_read_sector()`. The
 *          formatter lays the volume down inside an MBR partition rather than
 *          at LBA 0, so a test poking `s_disk.bytes` directly must add that
 *          base itself or it lands in the pre-partition gap and corrupts
 *          nothing.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] idx Entry index within the root cluster (0-based).
 *
 * @return Absolute byte offset within `s_disk.bytes`.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value is within the root cluster's sector range.
 *
 * @since 0.1.0
 */
static inline uint32_t root_byte(const ra8_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba =
    h->partition_base_lba + h->first_data_lba + ((h->root_cluster - 2U) * h->sectors_per_cluster);
  return (root_lba * (uint32_t)k_mut_block_size) + (idx * (uint32_t)k_mut_entry_bytes);
}

/**
 * @brief Byte offset in `s_disk.bytes` for the FAT entry of cluster @p clus.
 *
 * @details Partition-adjusted for the same reason as root_byte().
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number.
 *
 * @return Absolute byte offset of the 4-byte FAT entry for @p clus.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value addresses a valid 4-byte region in `s_disk.bytes`.
 *
 * @since 0.1.0
 */
static inline uint32_t fat_byte(const ra8_fs_mount_t* h, uint32_t clus)
{
  return ((h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_mut_block_size) + (clus * 4U);
}

/**
 * @brief Write a 4-byte LE value at byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 * @param[in] val 32-bit value to write in little-endian order.
 *
 * @pre off + 4 is within the disk image.
 * @post Bytes at @p off encode @p val in LE order.
 *
 * @since 0.1.0
 */
static inline void disk_set_u32le(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_disk.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_mut_mask_byte);
  p[1]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte);
  p[2]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte16) & (uint32_t)k_mut_mask_byte);
  p[3]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte24) & (uint32_t)k_mut_mask_byte);
}

/**
 * @brief Read a 4-byte LE value from byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 *
 * @return LE uint32 decoded from the four bytes.
 *
 * @pre off + 4 is within the disk image.
 * @post Return value equals the LE uint32 stored at @p off.
 *
 * @since 0.1.0
 */
static inline uint32_t disk_get_u32le(uint32_t off)
{
  const uint8_t* p = &s_disk.bytes[off];
  return (uint32_t)p[0] | ((uint32_t)p[1] << (uint32_t)k_mut_shift_byte8) |
         ((uint32_t)p[2] << (uint32_t)k_mut_shift_byte16) |
         ((uint32_t)p[3] << (uint32_t)k_mut_shift_byte24);
}
