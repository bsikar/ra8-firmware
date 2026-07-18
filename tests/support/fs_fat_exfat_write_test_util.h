/**
 * @file fs_fat_exfat_write_test_util.h
 * @brief Shared fixture for the test_ra8_fs_fat_exfat_write_*_cov.c siblings.
 *
 * @details
 * Header-only test fixture providing the countdown-faulting RAM block device,
 * the 64 MiB exFAT volume builder, and the raw disk-patching helpers shared
 * by test_ra8_fs_fat_exfat_write_bmp_cov.c and
 * test_ra8_fs_fat_exfat_write_dir_cov.c. Each including test executable gets
 * its own private copy of the backend state (everything here has internal
 * linkage), so the two binaries stay fully independent.
 *
 * The countdown read sequence for a 1-byte file on a fresh 64 MiB exFAT
 * volume (SPC=8, 4 KiB clusters, 128 entries per cluster) is:
 *
 *   R1  priv_exfat_find_bitmap    reads root cluster sector (entry 0 = 0x81)
 *   R2  priv_exfat_bitmap_scan    reads bitmap cluster sector
 *   W1  priv_exfat_write_data     writes file data sector
 *   R3  priv_exfat_bmp_switch     reads bitmap sector (loaded=UINT32_MAX path)
 *   W2  priv_exfat_bitmap_mark    writes updated bitmap sector
 *   R4-R9   priv_exfat_find_dir_space  reads entries 0-5 of root cluster
 *   R10 priv_exfat_write_dir_set  reads root sector for first dir entry
 *   W3  priv_exfat_write_dir_set  writes root sector (entry 0)
 *   ...
 *
 * For the full-root-cluster tests (entries 3-127 all patched to 0x85):
 *   R4-R131  find_dir_space reads entries 0-127 (128 reads)
 *   R132     priv_fat_get reads the FAT sector
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/* ---- constants ------------------------------------------------------------- */

/**
 * @enum wc_const_t
 * @brief Sizing and offset constants for the exFAT write coverage tests.
 *
 * @details All numeric literals in this file appear here as named enum values.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wc_block_size    = 512U,        /**< Bytes per disk sector.                 */
  k_wc_blocks_exfat  = 131072U,     /**< 64 MiB exFAT volume in sectors.        */
  k_wc_entry_bytes   = 32U,         /**< Directory entry size in bytes.         */
  k_wc_per_cluster   = 128U,        /**< Directory entries per 4 KiB cluster.   */
  k_wc_entry_inuse   = 0x85U,       /**< exFAT File entry type (in-use, bit 7). */
  k_wc_entry_eod     = 0x00U,       /**< exFAT end-of-directory type.           */
  k_wc_fat_eoc       = 0xFFFFFFFFU, /**< exFAT end-of-chain FAT value.          */
  k_wc_patch_start   = 3U,          /**< First root entry to mark in-use.       */
  k_wc_chain_offset  = 100U,        /**< Cluster offset for FAT-chain target.   */
  k_wc_shift_byte8   = 8U,          /**< 8-bit shift for LE16/32 pack.          */
  k_wc_shift_byte16  = 16U,         /**< 16-bit shift for LE32 pack.            */
  k_wc_shift_byte24  = 24U,         /**< 24-bit shift for LE32 pack.            */
  k_wc_mask_byte     = 0xFFU,       /**< Low-byte mask.                         */
  k_wc_name_too_long = 65U,         /**< One over k_exfat_name_cap (64).        */
  k_wc_fill_byte     = 0xA5U,       /**< Pre-format poison fill pattern.        */
} wc_const_t;

/**
 * @enum wc_rd_fail_t
 * @brief Read-countdown seeds for fault injection.
 *
 * @details Negative means never fail; 0 means fail the very next read;
 *          N means fail after N successful reads.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_wc_rd_never   = -1,  /**< Never inject a read error.                       */
  k_wc_rd_at_r1   = 0,   /**< Fail R1: find_bitmap read (line 79).             */
  k_wc_rd_at_r2   = 1,   /**< Fail R2: bitmap_scan read (line 124).            */
  k_wc_rd_at_r3   = 2,   /**< Fail R3: bmp_switch read (lines 160, 197).       */
  k_wc_rd_at_r4   = 3,   /**< Fail R4: first read_entry read (273, 341, 531).  */
  k_wc_rd_at_r10  = 9,   /**< Fail R10: write_dir_set read (line 441).         */
  k_wc_rd_fat_get = 131, /**< Fail R132: fat_get in find_dir_space (line 359). */
} wc_rd_fail_t;

/**
 * @enum wc_wr_fail_t
 * @brief Write-countdown seeds for fault injection.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_wc_wr_never = -1, /**< Never inject a write error.                 */
  k_wc_wr_at_w1 = 0,  /**< Fail W1: write_data write (lines 240, 494). */
  k_wc_wr_at_w3 = 2,  /**< Fail W3: write_dir_set write (line 448).    */
} wc_wr_fail_t;

/* ---- RAM-backed block device with countdown fault injection -------------- */

/**
 * @struct wc_disk_t
 * @brief Memory-backed disk presented to `ra8_fs` as a block device.
 *
 * @details Sector store is a flat `malloc` buffer; all I/O goes straight to
 *          this buffer so tests can inspect or corrupt on-disk state by
 *          patching individual bytes.
 *
 * @invariant bytes is non-NULL when the volume is alive.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.          */
  uint32_t block_count; /**< Number of 512-byte sectors. */
} wc_disk_t;

/**
 * @var s_disk
 * @brief Module-level RAM disk shared across all tests.
 * @warning Modify only through `build_exfat_volume()` / `free_volume()`.
 * @since 0.1.0
 */
static wc_disk_t s_disk = {};

/**
 * @var s_rd_remaining
 * @brief Read countdown: negative=never fail, 0=fail next, N=fail after N.
 * @warning Set only through test setup helpers.
 * @since 0.1.0
 */
static int32_t s_rd_remaining = (int32_t)k_wc_rd_never;

/**
 * @var s_wr_remaining
 * @brief Write countdown: negative=never fail, 0=fail next, N=fail after N.
 * @warning Set only through test setup helpers.
 * @since 0.1.0
 */
static int32_t s_wr_remaining = (int32_t)k_wc_wr_never;

/**
 * @brief Countdown-faulting `ra8_fs` read backend.
 *
 * @details Decrements `s_rd_remaining` on each call. When the counter
 *          reaches zero it returns `k_ra8_err_out_of_range` without touching
 *          the disk, simulating an I/O failure at a precise position in the
 *          call sequence.
 *
 * @param[in]  ctx   Pointer to `wc_disk_t`.
 * @param[in]  lba   Start sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok             Sectors read successfully.
 * @retval k_ra8_err_out_of_range Request out of range, or countdown expired.
 *
 * @pre ctx and buf are non-NULL.
 * @post buf holds the requested sectors when k_ra8_ok is returned.
 *
 * @since 0.1.0
 */
static inline ra8_err_t wc_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  if (s_rd_remaining == 0) {
    return k_ra8_err_out_of_range;
  }
  if (s_rd_remaining > 0) {
    s_rd_remaining--;
  }
  const wc_disk_t* d = (const wc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_wc_block_size],
         (size_t)count * (size_t)k_wc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Countdown-faulting `ra8_fs` write backend.
 *
 * @param[in] ctx   Pointer to `wc_disk_t`.
 * @param[in] lba   Start sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok             Sectors written successfully.
 * @retval k_ra8_err_out_of_range Request out of range, or countdown expired.
 *
 * @pre ctx and buf are non-NULL.
 * @post Disk bytes match @p buf on k_ra8_ok.
 *
 * @since 0.1.0
 */
static inline ra8_err_t wc_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  if (s_wr_remaining == 0) {
    return k_ra8_err_out_of_range;
  }
  if (s_wr_remaining > 0) {
    s_wr_remaining--;
  }
  wc_disk_t* d = (wc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_wc_block_size],
         buf,
         (size_t)count * (size_t)k_wc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Report disk geometry to `ra8_fs`.
 *
 * @param[in]  ctx         Pointer to `wc_disk_t`.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry populated.
 *
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect disk dimensions.
 *
 * @since 0.1.0
 */
static inline ra8_err_t wc_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const wc_disk_t* d = (const wc_disk_t*)ctx;
  *block_count       = d->block_count;
  *block_size        = (uint32_t)k_wc_block_size;
  return k_ra8_ok;
}

/**
 * @var s_ctrl_backend
 * @brief Countdown backend wired to `s_disk`.
 * @since 0.1.0
 */
static const ra8_fs_backend_t s_ctrl_backend = {
  .read_block   = wc_read,
  .write_block  = wc_write,
  .get_capacity = wc_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ------------------------------------------------------- */

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
 * @brief Allocate a fresh 64 MiB RAM disk and format it as exFAT.
 *
 * @details Resets the countdown faults to -1 (never fail) before format so
 *          the format itself always succeeds. Pre-fills with 0xA5 to prove
 *          the formatter writes the VBR rather than leaving a lucky zero
 *          pattern.
 *
 * @pre None.
 * @post `s_disk` holds a valid exFAT image; countdown faults are at -1.
 *
 * @since 0.1.0
 */
static inline void build_exfat_volume(void)
{
  free_volume();
  s_rd_remaining     = (int32_t)k_wc_rd_never;
  s_wr_remaining     = (int32_t)k_wc_wr_never;
  const size_t total = (size_t)k_wc_blocks_exfat * (size_t)k_wc_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_wc_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, (int)k_wc_fill_byte, total);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_ctrl_backend, &opts));
}

/**
 * @brief Byte offset in `s_disk.bytes` for root-dir entry @p idx.
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
static inline uint32_t root_entry_off(const ra8_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba = h->first_data_lba + ((h->root_cluster - 2U) * h->sectors_per_cluster);
  return (root_lba * (uint32_t)k_wc_block_size) + (idx * (uint32_t)k_wc_entry_bytes);
}

/**
 * @brief Byte offset in `s_disk.bytes` for the FAT entry of cluster @p clus.
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
static inline uint32_t fat_entry_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  return (h->first_fat_lba * (uint32_t)k_wc_block_size) + (clus * 4U);
}

/**
 * @brief Write a 4-byte LE value at byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 * @param[in] val 32-bit value to store in little-endian order.
 *
 * @pre off + 4 is within the disk image.
 * @post Bytes at @p off encode @p val in LE order.
 *
 * @since 0.1.0
 */
static inline void disk_set_u32le(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_disk.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_wc_mask_byte);
  p[1]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte8) & (uint32_t)k_wc_mask_byte);
  p[2]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte16) & (uint32_t)k_wc_mask_byte);
  p[3]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte24) & (uint32_t)k_wc_mask_byte);
}

/**
 * @brief Patch root-cluster entries @p start..127 to the in-use type 0x85.
 *
 * @details Forces `priv_exfat_find_dir_space` to scan the entire root cluster
 *          without finding a free run, triggering the FAT chain walk.  Entries
 *          0-2 (system entries written by the formatter) and entries 16-127
 *          (pre-fill 0xA5, bit 7 set) are already treated as in-use by
 *          `priv_exfat_slot_free`; only entries 3-15 (zeroed by the formatter)
 *          need explicit patching.  For safety this function patches every
 *          entry from @p start to 127.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] start First entry index to patch (typically 3).
 *
 * @pre h is non-NULL.
 * @post Entries @p start-127 have type byte 0x85.
 *
 * @since 0.1.0
 */
static inline void patch_root_full(const ra8_fs_mount_t* h, uint32_t start)
{
  for (uint32_t i = start; i < (uint32_t)k_wc_per_cluster; i++) {
    s_disk.bytes[root_entry_off(h, i)] = (uint8_t)k_wc_entry_inuse;
  }
}

/* ---- forward declaration of the internal function under test ------------- */

/*
 * `priv_exfat_bmp_switch` is cross-TU external linkage (non-static) in
 * `ra8_fs_fat_exfat_write.c`.  Its full declaration is in the umbrella header
 * `ra8_fs_fat_internal.h` (included above), which is on the include path for
 * tests per CLAUDE.md "Test access to internal symbols".
 */
