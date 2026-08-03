/**
 * @file fs_fat_file_test_util.h
 * @brief Shared fixture for the test_ra8_fs_fat_file_*_cov.c split siblings.
 *
 * @details
 * Header-only test fixture providing the synthetic block-device backends
 * (normal heap-backed and read-countdown + write-fail inject) plus the FAT16
 * / exFAT volume builders and small helpers shared by
 * test_ra8_fs_fat_file_open_cov.c and test_ra8_fs_fat_file_err_cov.c. Each
 * including test executable gets its own private copy of the backend state
 * (everything here has internal linkage), so the two binaries stay fully
 * independent.
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

/* ===========================================================================
 * On-disk format offsets
 * ===========================================================================
 */

/**
 * @enum fat_bpb_off_t
 * @brief Byte offsets of the FAT16 BPB fields written by the volume builders.
 *
 * @details Classic FAT BIOS Parameter Block layout inside sector 0. Only the
 *          fields the hand-built volumes populate are named here.
 *
 * @invariant All offsets lie inside one 512-byte boot sector.
 * @see build_fat16_volume()
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec (u16).            */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus (u8).             */
  k_bpb_off_rsvd_sec_cnt  = 14U,  /**< BPB_RsvdSecCnt (u16).            */
  k_bpb_off_num_fats      = 16U,  /**< BPB_NumFATs (u8).                */
  k_bpb_off_root_ent_cnt  = 17U,  /**< BPB_RootEntCnt (u16).            */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16 (u16).              */
  k_bpb_off_fat_sz16      = 22U,  /**< BPB_FATSz16 (u16).               */
  k_bpb_off_sig0          = 510U, /**< Boot signature low byte (0x55).  */
  k_bpb_off_sig1          = 511U, /**< Boot signature high byte (0xAA). */
} fat_bpb_off_t;

/**
 * @enum fat_bpb_val_t
 * @brief BPB field values and byte-packing constants for the builders.
 *
 * @invariant k_bpb_sig0_val / k_bpb_sig1_val form the mandatory 0xAA55 boot
 *            signature.
 * @see put16()
 */
typedef enum : uint8_t {
  k_bpb_sig0_val = 0x55U, /**< First boot-signature byte.     */
  k_bpb_sig1_val = 0xAAU, /**< Second boot-signature byte.    */
  k_byte_mask    = 0xFFU, /**< Low-byte mask used by put16(). */
} fat_bpb_val_t;

/* ===========================================================================
 * Disk geometry constants
 * ===========================================================================
 */

/**
 * @enum ra8_fs_cov_disk_t
 * @brief Block-count and geometry constants for synthetic block devices.
 */
typedef enum : uint32_t {
  k_cov_block_size   = 512U,        /**< Bytes per logical block.             */
  k_cov_blocks_fat16 = 8U * 1024U,  /**< 4 MiB FAT16 card (matches sibling).  */
  k_cov_blocks_exfat = 131072U,     /**< 64 MiB -- minimum valid exFAT size.  */
  k_cov_reads_inf    = 0xFFFFFFFFU, /**< Sentinel: unlimited reads in inject. */
} ra8_fs_cov_disk_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/** @brief Flat sector store for the normal backend. */
typedef struct {
  uint8_t* bytes;       /**< Heap-backed sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.   */
  uint32_t byte_count;  /**< Total bytes (block*size). */
} mem_disk_t;

static mem_disk_t s_disk = {};

static inline ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_cov_block_size],
         (size_t)count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static inline ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_cov_block_size],
         buf,
         (size_t)count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static inline ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_cov_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* ===========================================================================
 * Injecting (failure-injecting) backend
 * ===========================================================================
 */

/**
 * @struct inject_disk_t
 * @brief Shared-memory backend with deterministic I/O failure injection.
 *
 * @details Wraps the same byte array as ::mem_disk_t.  When `reads_left`
 *          reaches zero the next read returns ::k_ra8_err_hw_error.  When
 *          `writes_fail` is non-zero every write immediately returns
 *          ::k_ra8_err_hw_error.
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                   */
  uint32_t block_count; /**< Same geometry as s_disk.                    */
  uint32_t byte_count;  /**< Same geometry as s_disk.                    */
  uint32_t reads_left;  /**< Reads allowed; k_cov_reads_inf = unlimited. */
  uint8_t  writes_fail; /**< Non-zero -> all writes return hw_error.     */
} inject_disk_t;

static inject_disk_t s_inject = {};

static inline ra8_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_cov_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_cov_block_size],
         (size_t)count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static inline ra8_err_t inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->writes_fail != 0U) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_cov_block_size],
         buf,
         (size_t)count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static inline ra8_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const inject_disk_t* d = (const inject_disk_t*)ctx;
  *block_count           = d->block_count;
  *block_size            = (uint32_t)k_cov_block_size;
  return k_ra8_ok;
}

/* ===========================================================================
 * Volume setup helpers
 * ===========================================================================
 */

/**
 * @brief Store a 16-bit value little-endian at byte offset off.
 *
 * @param[out] p   Destination byte buffer.
 * @param[in]  off Byte offset of the low byte.
 * @param[in]  v   Value to store.
 *
 * @pre p has at least off + 2 bytes of space.
 * @post p[off] and p[off + 1] hold v little-endian.
 *
 * @note Trivially thread-safe (writes only through p).
 * @since 0.1.0
 */
static inline void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8U) & k_byte_mask);
}

/**
 * @brief Build a minimal valid FAT16 BPB at sector 0 of s_disk.
 *
 * @details SPC=1, 2 FATs, 32 FAT sectors, 16 root-directory entries,
 *          512-byte sectors, 8192-sector disk (4 MiB).  Matches the
 *          geometry used by test_ra8_fs_fat.c so shared assumptions hold.
 *
 * @pre s_disk.bytes is nullptr (previous volume freed or first call).
 * @post s_disk holds a calloc-zeroed 4 MiB image with valid BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_fat16_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_cov_blocks_fat16 * (uint32_t)k_cov_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_cov_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_cov_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_cov_blocks_fat16);
  put16(bpb, (uint32_t)k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
}

/**
 * @brief Allocate and format an exFAT volume stored in s_disk.
 *
 * @details Allocates 131072 sectors (64 MiB) and calls ra8_fs_format
 *          to produce a mountable exFAT image. 64 MiB is the smallest size
 *          that works now that the formatter places the volume inside an MBR
 *          partition: the card must hold the 1 MiB alignment gap
 *          (`k_exfat_fmt_part_lba`) on top of the 32 MiB minimum volume
 *          (`k_exfat_fmt_min_sectors`).
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a formatted exFAT image.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_exfat_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_cov_blocks_exfat * (uint32_t)k_cov_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_cov_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed for exfat volume");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Free the heap-backed sector store in s_disk.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
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
 * @brief Listdir callback that counts visible entries.
 *
 * @param[in] name Unused.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a uint32_t counter.
 *
 * @pre ctx is non-NULL and points to a uint32_t.
 * @post Counter at ctx is incremented by one.
 *
 * @note Trivially thread-safe.
 * @since 0.1.0
 */
static inline void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  if (ctx != nullptr) {
    (*(uint32_t*)ctx)++;
  }
}

/**
 * @brief Swap the active backend in *h to the inject backend.
 *
 * @details Copies s_disk geometry into s_inject (sharing the byte array),
 *          then overwrites h->backend with the inject function pointers.
 *          The caller must restore h->backend when done.
 *
 * @param[in,out] h           Mounted FAT volume to redirect.
 * @param[in]     reads_left  Allowed reads before failure; k_cov_reads_inf
 *                            means unlimited.
 * @param[in]     writes_fail Non-zero to make all writes fail immediately.
 *
 * @pre h is non-NULL and mounted.
 * @pre s_disk.bytes is non-NULL.
 * @post h->backend redirects I/O through the inject functions.
 *
 * @note Restore with: h->backend = saved_backend.
 * @since 0.1.0
 */
static inline void swap_to_inject(ra8_fs_mount_t* h, uint32_t reads_left, uint8_t writes_fail)
{
  s_inject.bytes          = s_disk.bytes;
  s_inject.block_count    = s_disk.block_count;
  s_inject.byte_count     = s_disk.byte_count;
  s_inject.reads_left     = reads_left;
  s_inject.writes_fail    = writes_fail;
  h->backend.read_block   = inj_read;
  h->backend.write_block  = inj_write;
  h->backend.get_capacity = inj_capacity;
  h->backend.ctx          = &s_inject;
}
