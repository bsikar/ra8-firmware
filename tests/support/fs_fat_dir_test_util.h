/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file fs_fat_dir_test_util.h
 * @brief Shared fixture for the test_ra8_fs_fat_dir_*_cov.c split siblings.
 *
 * @details
 * Header-only test fixture providing the three synthetic block-device
 * backends (normal heap-backed, read-countdown + write-fail inject, and
 * write-countdown) plus the FAT16 / exFAT volume builders and small helpers
 * shared by test_ra8_fs_fat_dir_list_cov.c and
 * test_ra8_fs_fat_dir_mutate_cov.c. Each including test executable gets its
 * own private copy of the backend state (everything here has internal
 * linkage), so the two binaries stay fully independent.
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
 * @see build_fat16_vol()
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
 * Geometry constants
 * ===========================================================================
 */

/**
 * @enum dir_cov_geo_t
 * @brief Block-count constants for the synthetic block devices used here.
 *
 * @details `k_geo_exfat` must clear `k_exfat_fmt_part_lba` (2048, the 1 MiB
 *          partition alignment gap) PLUS `k_exfat_fmt_min_sectors` (65536, the
 *          32 MiB minimum volume), because the formatter now places the volume
 *          inside an MBR partition instead of at LBA 0. A 32 MiB card can no
 *          longer hold a 32 MiB partition; 64 MiB is the smallest workable
 *          size, matching the exFAT mutate/read/write fixtures.
 */
typedef enum : uint32_t {
  k_geo_blk_sz     = 512U,        /**< Bytes per logical block.             */
  k_geo_fat16_spc1 = 8U * 1024U,  /**< 4 MiB FAT16 volume, SPC=1.           */
  k_geo_fat16_spc2 = 16U * 1024U, /**< 8 MiB FAT16 volume, SPC=2.           */
  k_geo_exfat      = 131072U,     /**< 64 MiB exFAT volume (see @details).  */
  k_geo_reads_inf  = 0xFFFFFFFFU, /**< Sentinel: unlimited reads in inject. */
} dir_cov_geo_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/** @brief Heap-backed sector store for the normal backend. */
typedef struct {
  uint8_t* bytes;       /**< Heap-allocated sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.      */
  uint32_t byte_count;  /**< Total bytes (blocks * size). */
} mem_disk_t;

/** @var s_disk
 * @brief Global singleton heap disk; freed by free_vol().
 * @warning Shared across tests; free_vol() must be called at test end.
 * @since 0.1.0
 */
static mem_disk_t s_disk = {};

static inline ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         (size_t)count * (uint32_t)k_geo_blk_sz);
  return k_ra8_ok;
}

static inline ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_geo_blk_sz);
  return k_ra8_ok;
}

static inline ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const mem_disk_t* d = (const mem_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_geo_blk_sz;
  return k_ra8_ok;
}

/** @brief Normal backend pointing at s_disk. */
static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* ===========================================================================
 * Inject (read-countdown + write-fail) backend
 * ===========================================================================
 */

/**
 * @struct inject_disk_t
 * @brief Shared-memory backend with deterministic I/O failure injection.
 *
 * @details Wraps the same byte array as mem_disk_t. When reads_left reaches
 *          zero the next read returns k_ra8_err_hw_error. When writes_fail is
 *          non-zero every write immediately returns k_ra8_err_hw_error.
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                   */
  uint32_t block_count; /**< Same geometry as s_disk.                    */
  uint32_t byte_count;  /**< Same geometry as s_disk.                    */
  uint32_t reads_left;  /**< Reads allowed; k_geo_reads_inf = unlimited. */
  uint8_t  writes_fail; /**< Non-zero -> all writes return hw_error.     */
} inject_disk_t;

/** @var s_inject
 * @brief Global inject backend state; populated by swap_to_inject().
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static inject_disk_t s_inject = {};

static inline ra8_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_geo_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         (size_t)count * (uint32_t)k_geo_blk_sz);
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
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_geo_blk_sz);
  return k_ra8_ok;
}

static inline ra8_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const inject_disk_t* d = (const inject_disk_t*)ctx;
  *block_count           = d->block_count;
  *block_size            = (uint32_t)k_geo_blk_sz;
  return k_ra8_ok;
}

/* ===========================================================================
 * Write-countdown backend
 * ===========================================================================
 */

/**
 * @struct wcount_disk_t
 * @brief Shared-memory backend that counts down writes only.
 *
 * @details All reads succeed unconditionally. Each write_block call
 *          decrements writes_left; when zero the write returns
 *          k_ra8_err_hw_error. This allows targeting the Nth write in a
 *          call chain (e.g. FAT copies vs. cluster-init vs. dir-entry).
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                    */
  uint32_t block_count; /**< Same geometry as s_disk.                     */
  uint32_t byte_count;  /**< Same geometry as s_disk.                     */
  uint32_t writes_left; /**< Writes allowed before failure; 0 = fail now. */
} wcount_disk_t;

/** @var s_wcount
 * @brief Global write-countdown backend state.
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static wcount_disk_t s_wcount = {};

static inline ra8_err_t wco_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const wcount_disk_t* d = (const wcount_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         (size_t)count * (uint32_t)k_geo_blk_sz);
  return k_ra8_ok;
}

static inline ra8_err_t wco_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  wcount_disk_t* d = (wcount_disk_t*)ctx;
  if (d->writes_left == 0U) {
    return k_ra8_err_hw_error;
  }
  d->writes_left--;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_geo_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_geo_blk_sz);
  return k_ra8_ok;
}

static inline ra8_err_t wco_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const wcount_disk_t* d = (const wcount_disk_t*)ctx;
  *block_count           = d->block_count;
  *block_size            = (uint32_t)k_geo_blk_sz;
  return k_ra8_ok;
}

/* ===========================================================================
 * Volume builders
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
 * @brief Allocate and hand-build a minimal FAT16 BPB in s_disk (SPC=1).
 *
 * @details 8192 sectors, 2 FATs of 32 sectors each, 16 root entries, SPC=1.
 *          Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(8126).
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a calloc-zeroed 4 MiB image with a valid BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_fat16_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_fat16_spc1 * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_fat16_spc1;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_geo_blk_sz);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_geo_fat16_spc1);
  put16(bpb, (uint32_t)k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
}

/**
 * @brief Allocate and hand-build a minimal FAT16 BPB in s_disk (SPC=2).
 *
 * @details 16384 sectors, 2 FATs of 32 sectors each, 16 root entries, SPC=2.
 *          Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(16318).
 *          Cluster count = 16318/2 = 8159 >= 4085 -> FAT16.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a calloc-zeroed 8 MiB image with a valid SPC=2 BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_fat16_spc2_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_fat16_spc2 * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_fat16_spc2;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_geo_blk_sz);
  bpb[k_bpb_off_sec_per_clus] = 2U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_geo_fat16_spc2);
  put16(bpb, (uint32_t)k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
}

/**
 * @brief Allocate a 32 MiB exFAT volume in s_disk via ra8_fs_format.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a formatted exFAT image ready to mount.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_exfat_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_exfat * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Release the heap-backed sector store in s_disk.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void free_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/* ===========================================================================
 * Shared helpers
 * ===========================================================================
 */

/**
 * @brief Count visible directory entries via listdir callback.
 *
 * @param[in]  name Unused.
 * @param[in]  attr Unused.
 * @param[in]  size Unused.
 * @param[in]  ctx  Pointer to a uint32_t counter.
 *
 * @pre ctx is non-NULL and points to a uint32_t.
 * @post Counter at ctx is incremented by one.
 *
 * @note Trivially thread-safe (no shared state modified by this function).
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
 * @brief Redirect h->backend to the inject (read-countdown) backend.
 *
 * @details Shares s_disk.bytes with s_inject; the caller saves and restores
 *          h->backend when done.
 *
 * @param[in,out] h           Mounted volume to redirect.
 * @param[in]     reads_left  Reads allowed before failure.
 * @param[in]     writes_fail Non-zero to make every write fail immediately.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend uses inj_read / inj_write callbacks.
 *
 * @note Caller must restore h->backend = saved after use.
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

/**
 * @brief Redirect h->backend to the write-countdown backend.
 *
 * @details Shares s_disk.bytes with s_wcount; all reads succeed. Each
 *          write_block call consumes one token from writes_left.
 *
 * @param[in,out] h           Mounted volume to redirect.
 * @param[in]     writes_left Writes allowed before failure.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend uses wco_read / wco_write callbacks.
 *
 * @note Caller must restore h->backend = saved after use.
 * @since 0.1.0
 */
static inline void swap_to_wcount(ra8_fs_mount_t* h, uint32_t writes_left)
{
  s_wcount.bytes          = s_disk.bytes;
  s_wcount.block_count    = s_disk.block_count;
  s_wcount.byte_count     = s_disk.byte_count;
  s_wcount.writes_left    = writes_left;
  h->backend.read_block   = wco_read;
  h->backend.write_block  = wco_write;
  h->backend.get_capacity = wco_capacity;
  h->backend.ctx          = &s_wcount;
}

/**
 * @brief Create count empty files in the directory at dir_path.
 *
 * @details Each file is opened (write mode, creates dir entry with
 *          first_cluster=0) then immediately closed. The file name is
 *          "Gnn.TXT" where nn is the zero-padded index (00-15).
 *
 * @param[in,out] h        Mounted volume.
 * @param[in]     dir_path Parent directory (e.g. "/" or "/SUB").
 * @param[in]     count    Number of files to create (0..16).
 *
 * @pre h is non-NULL and mounted; dir_path is a valid directory.
 * @pre count <= 16 (maximum root-dir entries).
 * @post count new dir entries exist under dir_path.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void create_empty_files(ra8_fs_mount_t* h, const char* dir_path, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    char name[32] = {};
    if (strcmp(dir_path, "/") == 0) {
      (void)snprintf(name, sizeof(name), "/F%02u.TXT", i);
    } else {
      (void)snprintf(name, sizeof(name), "%s/G%02u.TXT", dir_path, i);
    }
    ra8_fs_file_t* f = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  }
}
