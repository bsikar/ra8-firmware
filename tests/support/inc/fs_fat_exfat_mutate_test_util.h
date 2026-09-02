/**
 * @file fs_fat_exfat_mutate_test_util.h
 * @brief Shared fixture for the exFAT mutate + write_file test executables.
 *
 * @details
 * Header-only test fixture providing the RAM-backed block device, the 64 MiB
 * exFAT volume builder, the raw directory-entry / FAT patching helpers used by
 * the exFAT mutate (unlink / rename / listdir corruption) coverage suite, and
 * the allocation-bitmap census `test_ra8_fs_exfat_write_file.c` uses to prove
 * a repeated create leaks nothing. Everything here has internal linkage, so
 * each including test executable owns a private copy of the fixture state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
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
  k_mut_file_attr_off   = 4U,          /**< File entry FileAttributes byte offset. */
  k_mut_attr_directory  = 0x10U,       /**< FileAttributes: directory bit.         */
  k_mut_cluster_first   = 2U,          /**< exFAT cluster numbering starts at 2.   */
  k_mut_bits_per_byte   = 8U,          /**< Allocation-bitmap bits per byte.       */
} mut_cov_const_t;

/**
 * @enum mut_fault_t
 * @brief Sentinel for the backend fault countdowns.
 *
 * @details Signed, because "never fail" has to be distinguishable from
 *          "fail on the very next call".
 *
 * @invariant A countdown at ::k_mut_fault_never never reaches zero.
 * @see s_mut_rd_fail_in
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_mut_fault_never = -1, /**< Injection disabled (the default). */
} mut_fault_t;

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

/** @var s_mut_rd_fail_in
 * @brief Read countdown: negative = never fail, 0 = fail this call, N = fail
 *        after N more successful reads.
 * @details Defaulted to "never", so every test written before fault injection
 *          existed behaves exactly as it did. Set it to place a backend read
 *          failure at a precise point in a call sequence.
 * @details ONE-SHOT: the countdown disarms itself the moment it fires. That is
 *          what lets a test observe a driver's ERROR HANDLING rather than only
 *          its error return -- a rollback that frees the cluster it just
 *          allocated needs a working device to do it with, and a latching fault
 *          would make every rollback in the tree look broken.
 * @warning Reset to ::k_mut_fault_never if a test arms it and does not reach it.
 * @since 0.1.0
 */
static int32_t s_mut_rd_fail_in = (int32_t)k_mut_fault_never;

/** @var s_mut_wr_fail_in
 * @brief Write countdown, with the same convention as ::s_mut_rd_fail_in.
 * @details The one that matters for `mkdir`: it is how a failure BETWEEN
 *          allocating a directory's cluster and linking its entry set is
 *          reached, which is the path the cluster rollback exists for. One-shot,
 *          for the reason ::s_mut_rd_fail_in records.
 * @warning Reset to ::k_mut_fault_never if a test arms it and does not reach it.
 * @since 0.1.0
 */
static int32_t s_mut_wr_fail_in = (int32_t)k_mut_fault_never;

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
 * @since 0.1.0 @details Implements the bounded mut read fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline ra8_err_t
internal_mut_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  if (s_mut_rd_fail_in == 0) {
    s_mut_rd_fail_in = (int32_t)k_mut_fault_never;
    return k_ra8_err_out_of_range;
  }
  if (s_mut_rd_fail_in > 0) {
    s_mut_rd_fail_in--;
  }
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
 * @since 0.1.0 @details Implements the bounded mut write fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline ra8_err_t
internal_mut_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  if (s_mut_wr_fail_in == 0) {
    s_mut_wr_fail_in = (int32_t)k_mut_fault_never;
    return k_ra8_err_out_of_range;
  }
  if (s_mut_wr_fail_in > 0) {
    s_mut_wr_fail_in--;
  }
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
 * @since 0.1.0 @details Implements the bounded mut capacity fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline ra8_err_t
internal_mut_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
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
  .read_block   = internal_mut_read,
  .write_block  = internal_mut_write,
  .get_capacity = internal_mut_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ---------------------------------------------------- */

/**
 * @brief Release the heap buffer held by `s_disk`.
 *
 * @pre None.
 * @post `s_disk.bytes` is `nullptr`.
 *
 * @since 0.1.0 @details Implements the bounded free volume fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void internal_free_volume(void)
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void internal_build_exfat_volume(void)
{
  internal_free_volume();
  const size_t total = (size_t)k_mut_blocks_exfat * (size_t)k_mut_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_mut_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, (int)k_mut_fill_byte, total);
  s_mut_rd_fail_in          = (int32_t)k_mut_fault_never;
  s_mut_wr_fail_in          = (int32_t)k_mut_fault_never;
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
 * @since 0.1.0 @details Implements the bounded count cb fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void
internal_count_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
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
 * @since 0.1.0 @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline uint32_t internal_root_byte(const ra8_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba = h->partition_base_lba + h->first_data_lba +
                            ((uint64_t)(h->root_cluster - 2U) * h->sectors_per_cluster);
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
 * @since 0.1.0 @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline uint32_t internal_fat_byte(const ra8_fs_mount_t* h, uint32_t clus)
{
  return ((h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_mut_block_size) +
         ((uint64_t)clus * 4U);
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
 * @since 0.1.0 @details Implements the bounded disk set u32le fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void internal_disk_set_u32le(uint32_t off, uint32_t val)
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
 * @since 0.1.0 @details Implements the bounded disk get u32le fixture step using caller-owned state. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline uint32_t internal_disk_get_u32le(uint32_t off)
{
  const uint8_t* p = &s_disk.bytes[off];
  return (uint32_t)p[0] | ((uint32_t)p[1] << (uint32_t)k_mut_shift_byte8) |
         ((uint32_t)p[2] << (uint32_t)k_mut_shift_byte16) |
         ((uint32_t)p[3] << (uint32_t)k_mut_shift_byte24);
}

/**
 * @brief Count the clusters currently marked allocated in the exFAT bitmap.
 *
 * @details Reads the allocation-bitmap directory entry (root slot 0, written by
 *          the formatter), locates its data run, and counts the set bits over
 *          the volume's cluster count. The bitmap is the authoritative record
 *          of allocation in exFAT, so this is the direct measurement of whether
 *          an operation leaked space: a create-then-recreate that leaks leaves
 *          the first file's bits set with no directory entry referencing them.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Number of clusters whose bitmap bit is 1.
 * @retval 0..count_of_clusters The allocated-cluster count.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes holds the image.
 * @pre The bitmap is contiguous (true for a freshly formatted volume).
 * @post No disk state is modified.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline uint32_t internal_alloc_bitmap_used(const ra8_fs_mount_t* h)
{
  const uint32_t entry_off = internal_root_byte(h, (uint32_t)k_mut_root_bitmap_idx);
  const uint32_t bmp_clus  = internal_disk_get_u32le(entry_off + (uint32_t)k_mut_strm_off_clus);
  /* Partition-adjusted for the same reason as root_byte(): the formatter lays
   * the volume down inside an MBR partition, so a census that forgets the base
   * counts the pre-partition gap instead of the bitmap. */
  const uint32_t bmp_lba =
    h->partition_base_lba + h->first_data_lba +
    ((uint64_t)(bmp_clus - (uint32_t)k_mut_cluster_first) * h->sectors_per_cluster);
  const uint32_t base = bmp_lba * (uint32_t)k_mut_block_size;
  uint32_t       used = 0U;
  for (uint32_t i = 0U; i < h->count_of_clusters; i++) {
    const uint8_t byte = s_disk.bytes[base + (i / (uint32_t)k_mut_bits_per_byte)];
    if (((byte >> (i % (uint32_t)k_mut_bits_per_byte)) & 1U) != 0U) {
      used++;
    }
  }
  return used;
}

/**
 * @brief Mark every cluster of the exFAT allocation bitmap as used.
 *
 * @details The direct way to present a driver with a volume that has no free
 *          cluster left, so directory growth (#677) has nowhere to extend to and
 *          must report ::k_ra8_err_no_mem. Sets every bit of the bitmap region
 *          the formatter laid down; the on-disk structure is deliberately
 *          inconsistent afterwards (the bitmap claims clusters no entry set
 *          references), so a test that calls this must not also run the
 *          structural scan.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @pre @p h is non-NULL and mounted; s_disk.bytes holds the image.
 * @pre The bitmap is contiguous (true for a freshly formatted volume).
 * @post Every allocation-bitmap bit for the volume's clusters reads as 1.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void internal_alloc_bitmap_fill(const ra8_fs_mount_t* h)
{
  const uint32_t entry_off = internal_root_byte(h, (uint32_t)k_mut_root_bitmap_idx);
  const uint32_t bmp_clus  = internal_disk_get_u32le(entry_off + (uint32_t)k_mut_strm_off_clus);
  const uint32_t bmp_lba =
    h->partition_base_lba + h->first_data_lba +
    ((uint64_t)(bmp_clus - (uint32_t)k_mut_cluster_first) * h->sectors_per_cluster);
  const uint32_t base = bmp_lba * (uint32_t)k_mut_block_size;
  const uint32_t bytes =
    (h->count_of_clusters + (uint32_t)k_mut_bits_per_byte - 1U) / (uint32_t)k_mut_bits_per_byte;
  memset(&s_disk.bytes[base], (int)k_mut_mask_byte, (size_t)bytes);
}

/**
 * @brief Stamp the directory attribute onto the first user File entry.
 *
 * @details The library has no exFAT `mkdir`, so the only way to present the
 *          driver with an exFAT directory is to flip the FileAttributes bit on
 *          an entry set it already wrote. Nothing in the lookup path verifies
 *          the set's checksum, so the patched set resolves exactly like the
 *          real thing -- which is precisely the situation the directory guards
 *          have to survive.
 *
 * @param[in] h Mounted exFAT volume whose root slot 3 holds a File entry.
 *
 * @pre h is non-NULL and mounted; exactly one user file has been created.
 * @post That file's entry set reports ::k_mut_attr_directory.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static inline void internal_mark_first_file_as_directory(const ra8_fs_mount_t* h)
{
  const uint32_t entry_off = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[entry_off + (uint32_t)k_mut_file_attr_off] |= (uint8_t)k_mut_attr_directory;
}
