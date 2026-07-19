/**
 * @file test_ra8_fs_fat_lfn_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_lfn.c.
 *
 * @details
 * Dedicated companion test executable that drives the branches in
 * ra8_fs_fat_lfn.c not yet exercised by test_ra8_fs_lfn.c:
 *
 *   - line 147: priv_lfn_name_for -- checksum mismatch path
 *   - lines 262, 301: priv_dir_find_long_sector k_lfn_scan_continue +
 *                     while-exit when the fixed root region is exhausted
 *   - lines 275-276: priv_dir_find_long -- leading-slash strip
 *   - line 286: priv_dir_find_long -- priv_read_sector I/O failure
 *   - lines 296-298: priv_dir_find_long -- priv_dir_walk_next_sector failure
 *   - line 317: priv_dir_find_free -- priv_read_sector I/O failure
 *   - lines 328-330: priv_dir_find_free -- priv_dir_walk_next_sector failure
 *   - line 333: priv_dir_find_free -- directory exhausted (no free slot)
 *   - line 346: priv_free_chain -- priv_fat_get I/O failure
 *   - line 350: priv_free_chain -- priv_fat_set write failure
 *   - line 359: priv_free_chain -- cyclic FAT chain guard fires
 *
 * Line 129 of ra8_fs_fat_lfn.c (defensive buffer-overflow guard in
 * priv_lfn_add) is marked GCOVR_EXCL_LINE in the source because the
 * condition requires pos >= k_lfn_name_cap-1 = 255, but with
 * k_lfn_max_entries=19 the maximum reachable pos is
 * (19-1)*13+12 = 246 < 255; no host input can trigger it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum fs_fat_lfn_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_fs_fat_lfn_cov_bpb_55   = 0x55U,
  k_fs_fat_lfn_cov_bpb_aa   = 0xAAU,
  k_fs_fat_lfn_cov_i_13     = 13U,
  k_fs_fat_lfn_cov_put16_11 = 11U,
  k_fs_fat_lfn_cov_put16_14 = 14U,
  k_fs_fat_lfn_cov_put16_17 = 17U,
  k_fs_fat_lfn_cov_put16_19 = 19U,
  k_fs_fat_lfn_cov_put16_20 = 20U,
  k_fs_fat_lfn_cov_put16_22 = 22U,
  k_fs_fat_lfn_cov_put16_26 = 26U,
  k_fs_fat_lfn_cov_v_ff     = 0xFFU,
  k_fs_fat_lfn_cov_val_11   = 11,
  k_fs_fat_lfn_cov_val_12   = 12,
  k_fs_fat_lfn_cov_val_13   = 13,
} fs_fat_lfn_cov_uint8_const_t;

/**
 * @enum fs_fat_lfn_cov_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_fs_fat_lfn_cov_val_510 = 510,
  k_fs_fat_lfn_cov_val_511 = 511,
} fs_fat_lfn_cov_uint16_const_t;

/* ===========================================================================
 * Geometry constants
 * ===========================================================================
 */

/**
 * @enum lfn_cov_geo_t
 * @brief Volume geometry constants for the 4 MiB FAT16 test image (SPC=1).
 * @details Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(8126).
 *          first_data_lba = 1 + 2*32 + 1 = 66; cluster 2 -> LBA 66.
 */
typedef enum : uint32_t {
  k_lcov_blk_sz    = 512U,                 /**< Bytes per logical sector.   */
  k_lcov_blocks    = 8U * 1024U,           /**< Total sectors (4 MiB).      */
  k_lcov_fat1_lba  = 1U,                   /**< FAT1 starts at LBA 1.       */
  k_lcov_fat_sz    = 32U,                  /**< Sectors per FAT copy.       */
  k_lcov_num_fats  = 2U,                   /**< Number of FAT copies.       */
  k_lcov_root_lba  = 1U + (2U * 32U),      /**< Root dir at LBA 65.         */
  k_lcov_data_lba  = 1U + (2U * 32U) + 1U, /**< First data sector (LBA 66). */
  k_lcov_reads_inf = 0xFFFFFFFFU,          /**< Sentinel: unlimited reads.  */
} lfn_cov_geo_t;

/**
 * @enum lfn_cov_fat_t
 * @brief Byte offsets for FAT[2..5] entries within the FAT1 sector image.
 * @details FAT1 sector resides at byte k_lcov_fat1_lba*k_lcov_blk_sz = 512
 *          in the disk image. FAT16 entry n occupies bytes n*2 and n*2+1
 *          within that sector.
 */
typedef enum : uint32_t {
  k_lcov_fat1_off  = 1U * 512U, /**< FAT1 byte offset in disk image.       */
  k_lcov_fat_cl2lo = 2U * 2U,   /**< FAT[2] low-byte offset in FAT sector. */
  k_lcov_fat_cl3lo = 3U * 2U,   /**< FAT[3] low-byte offset.               */
  k_lcov_fat_cl4lo = 4U * 2U,   /**< FAT[4] low-byte offset.               */
  k_lcov_fat_cl5lo = 5U * 2U,   /**< FAT[5] low-byte offset.               */
  k_lcov_fat_hi    = 1U,        /**< High-byte offset within a FAT entry.  */
} lfn_cov_fat_t;

/**
 * @enum lfn_cov_cyclic_t
 * @brief Cluster numbers used in the cyclic-FAT protocol_error test.
 * @details The cyclic chain: 2->3->4->5->2. With count_of_clusters=4,
 *          priv_free_chain walks all 4 clusters, revisits cluster 2 (already
 *          freed), then guard=5 > 4 triggers k_ra8_err_protocol_error.
 */
typedef enum : uint32_t {
  k_lcov_cy_a   = 2U, /**< First cluster in cyclic chain.                   */
  k_lcov_cy_b   = 3U, /**< Second cluster.                                  */
  k_lcov_cy_c   = 4U, /**< Third cluster.                                   */
  k_lcov_cy_d   = 5U, /**< Fourth cluster (points back to cy_a).            */
  k_lcov_cy_cnt = 4U, /**< count_of_clusters override to trigger the guard. */
} lfn_cov_cyclic_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/** @brief Heap-backed sector store for the normal backend. */
typedef struct {
  uint8_t* bytes;       /**< Heap-allocated sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.      */
  uint32_t byte_count;  /**< Total bytes.                 */
} mem_disk_t;

/** @var s_disk
 * @brief Global singleton heap disk; freed by free_vol() at test end.
 * @warning Shared across tests; each test must call free_vol() at end.
 * @since 0.1.0
 */
static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_lcov_blk_sz],
         (size_t)count * (uint32_t)k_lcov_blk_sz);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_lcov_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_lcov_blk_sz);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_lcov_blk_sz;
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
 * @details Wraps the same byte array as mem_disk_t. When reads_left reaches
 *          zero the next read returns k_ra8_err_hw_error. When writes_fail is
 *          non-zero every write immediately returns k_ra8_err_hw_error.
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                   */
  uint32_t block_count; /**< Same geometry as s_disk.                    */
  uint32_t byte_count;  /**< Same geometry as s_disk.                    */
  uint32_t reads_left;  /**< Reads allowed; k_lcov_reads_inf = infinite. */
  uint8_t  writes_fail; /**< Non-zero -> all writes return hw_error.     */
} inject_disk_t;

/** @var s_inject
 * @brief Global inject backend state; populated by swap_to_inject().
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static inject_disk_t s_inject = {};

static ra8_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_lcov_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_lcov_blk_sz],
         (size_t)count * (uint32_t)k_lcov_blk_sz);
  return k_ra8_ok;
}

static ra8_err_t inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->writes_fail != 0U) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_lcov_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_lcov_blk_sz);
  return k_ra8_ok;
}

static ra8_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  *block_count     = d->block_count;
  *block_size      = (uint32_t)k_lcov_blk_sz;
  return k_ra8_ok;
}

/* ===========================================================================
 * Shared helpers
 * ===========================================================================
 */

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]      = (uint8_t)(v & k_fs_fat_lfn_cov_v_ff);
  p[off + 1U] = (uint8_t)((v >> 8U) & k_fs_fat_lfn_cov_v_ff);
}

/**
 * @enum lfn_cov_sfn_t
 * @brief Constants for the sfn_checksum rotate-right-add algorithm.
 */
typedef enum : uint32_t {
  k_name83_len = 11U,   /**< Bytes in a packed 8.3 name field.      */
  k_csum_hibit = 0x80U, /**< Rotate-in bit when running sum is odd. */
} lfn_cov_sfn_t;

/**
 * @brief Rotate-right-add checksum over an 11-byte 8.3 name (FAT spec sec 7).
 * @param[in] name83 Pointer to the 11-byte packed name field.
 * @return 8-bit checksum.
 * @pre name83 is non-NULL and points to at least 11 valid bytes.
 * @post Return value binds the LFN chain to its 8.3 entry.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static uint8_t sfn_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_name83_len; i++) {
    sum = (uint8_t)((((sum & 1U) != 0U) ? (uint32_t)k_csum_hibit : 0U) + (uint32_t)(sum >> 1U) +
                    (uint32_t)name83[i]);
  }
  return sum;
}

/** @brief Packed 8.3 alias for "mybook.epub": base MYBOOK~1, ext EPU. */
static const uint8_t k_alias83[11] = {'M', 'Y', 'B', 'O', 'O', 'K', '~', '1', 'E', 'P', 'U'};

/**
 * @enum lfn_cov_entry_t
 * @brief Constants for building raw LFN and 8.3 directory entries.
 */
typedef enum : uint8_t {
  k_lfn_ord_last   = 0x41U, /**< Order=1 with last-logical flag (0x40).   */
  k_lfn_attr_magic = 0x0FU, /**< LFN attribute byte (ATTR_LFN).           */
  k_attr_archive   = 0x20U, /**< FAT ARCHIVE attribute for 8.3 entries.   */
  k_csum_scramble  = 0xFFU, /**< XOR mask to produce an invalid checksum. */
  k_del_marker     = 0xE5U, /**< FAT "used-then-deleted" name[0] marker.  */
} lfn_cov_entry_t;

/**
 * @enum lfn_cov_utf16_t
 * @brief UTF-16 sentinel values stored inside LFN directory entries.
 */
typedef enum : uint16_t {
  k_utf16_term = 0x0000U, /**< UTF-16 NUL terminator (end of name). */
  k_utf16_pad  = 0xFFFFU, /**< UTF-16 padding past the terminator.  */
} lfn_cov_utf16_t;

/**
 * @enum lfn_cov_dir_t
 * @brief Constants describing the root directory layout for these tests.
 */
typedef enum : uint32_t {
  k_del_entries = 16U, /**< Total entries to mark deleted in the root dir. */
  k_del_stride  = 32U, /**< Bytes per FAT directory entry.                 */
} lfn_cov_dir_t;

/**
 * @brief UTF-16LE char slot byte-offsets within a 32-byte LFN directory entry.
 * @details FAT spec sec 7: LDIR_Name1 at 1,3,5,7,9; LDIR_Name2 at
 *          14,16,18,20,22,24; LDIR_Name3 at 28,30. Thirteen chars total.
 * @since 0.1.0
 */
static const uint8_t k_lfn_char_off[13] =
  {1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};

/* ===========================================================================
 * Volume builders
 * ===========================================================================
 */

/**
 * @brief Write a minimal FAT16 BPB into s_disk (SPC=1, 8192 sectors).
 * @details Allocates and zero-initialises s_disk. Layout:
 *          BPB(1) FAT1(32) FAT2(32) root(1) data(8126).
 * @pre s_disk.bytes is nullptr or will be freed.
 * @post s_disk holds a calloc-zeroed 4 MiB image with a valid BPB.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_fat16_vol(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_lcov_blocks * (uint32_t)k_lcov_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1U, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_lcov_blocks;
  TEST_ASSERT(s_disk.bytes != nullptr);
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, k_fs_fat_lfn_cov_put16_11, (uint16_t)k_lcov_blk_sz);
  bpb[k_fs_fat_lfn_cov_val_13] = 1U;
  put16(bpb, k_fs_fat_lfn_cov_put16_14, (uint16_t)k_lcov_fat1_lba);
  bpb[16] = (uint8_t)k_lcov_num_fats;
  put16(bpb, k_fs_fat_lfn_cov_put16_17, 16U);
  put16(bpb, k_fs_fat_lfn_cov_put16_19, (uint16_t)k_lcov_blocks);
  put16(bpb, k_fs_fat_lfn_cov_put16_22, (uint16_t)k_lcov_fat_sz);
  bpb[k_fs_fat_lfn_cov_val_510] = k_fs_fat_lfn_cov_bpb_55;
  bpb[k_fs_fat_lfn_cov_val_511] = k_fs_fat_lfn_cov_bpb_aa;
}

/**
 * @brief Stamp one LFN entry for "mybook.epub" into a 32-byte slot.
 * @param[out] slot  32-byte destination buffer.
 * @param[in]  csum  Checksum to embed at offset 13.
 * @pre slot is non-NULL and points to 32 writable bytes.
 * @post slot contains a valid LFN directory entry for "mybook.epub".
 * @note Pure helper; not thread-safe with concurrent slot access.
 * @since 0.1.0
 */
static void stamp_lfn_entry(uint8_t* slot, uint8_t csum)
{
  slot[0]                       = (uint8_t)k_lfn_ord_last;
  slot[k_fs_fat_lfn_cov_val_11] = (uint8_t)k_lfn_attr_magic;
  slot[k_fs_fat_lfn_cov_val_12] = 0x00U;
  slot[k_fs_fat_lfn_cov_val_13] = csum;
  put16(slot, k_fs_fat_lfn_cov_put16_26, 0U);

  static const char k_lname[] = "mybook.epub"; /* 11 chars */
  for (uint32_t i = 0U; i < k_fs_fat_lfn_cov_i_13; i++) {
    if (i < (uint32_t)(sizeof(k_lname) - 1U)) {
      put16(slot, (uint32_t)k_lfn_char_off[i], (uint16_t)(uint8_t)k_lname[i]);
    } else if (i == (uint32_t)(sizeof(k_lname) - 1U)) {
      put16(slot, (uint32_t)k_lfn_char_off[i], (uint16_t)k_utf16_term);
    } else {
      put16(slot, (uint32_t)k_lfn_char_off[i], (uint16_t)k_utf16_pad);
    }
  }
}

/**
 * @brief Build FAT16 volume with a valid LFN entry for "mybook.epub".
 * @details Root dir: slot 0 = LFN (correct checksum), slot 1 = 8.3 entry,
 *          slot 2 = EoD (0x00).
 * @pre None.
 * @post s_disk contains a valid FAT16 image; LFN entry is findable.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_vol_lfn_good(void)
{
  build_fat16_vol();
  uint8_t* root = &s_disk.bytes[(size_t)(uint32_t)k_lcov_root_lba * (uint32_t)k_lcov_blk_sz];
  stamp_lfn_entry(&root[0], sfn_checksum(k_alias83));
  uint8_t* sfn = &root[32U];
  memcpy(sfn, k_alias83, 11U);
  sfn[k_fs_fat_lfn_cov_val_11] = (uint8_t)k_attr_archive;
  put16(sfn, k_fs_fat_lfn_cov_put16_26, 0U);
  put16(sfn, k_fs_fat_lfn_cov_put16_20, 0U);
}

/**
 * @brief Build FAT16 volume with a CORRUPTED checksum in the LFN entry.
 * @details Root dir: slot 0 = LFN (wrong checksum), slot 1 = 8.3 entry.
 *          priv_lfn_name_for will reject the chain, triggering line 147.
 * @pre None.
 * @post s_disk contains a FAT16 image; checksum mismatch prevents LFN match.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_vol_lfn_bad_csum(void)
{
  build_fat16_vol();
  uint8_t* root     = &s_disk.bytes[(size_t)(uint32_t)k_lcov_root_lba * (uint32_t)k_lcov_blk_sz];
  uint8_t  bad_csum = (uint8_t)(sfn_checksum(k_alias83) ^ (uint8_t)k_csum_scramble);
  stamp_lfn_entry(&root[0], bad_csum);
  uint8_t* sfn = &root[32U];
  memcpy(sfn, k_alias83, 11U);
  sfn[k_fs_fat_lfn_cov_val_11] = (uint8_t)k_attr_archive;
  put16(sfn, k_fs_fat_lfn_cov_put16_26, 0U);
  put16(sfn, k_fs_fat_lfn_cov_put16_20, 0U);
}

/**
 * @brief Build FAT16 volume with all 16 root entries marked 0xE5 (deleted).
 * @details priv_dir_find_long_sector skips 0xE5 entries; after all 16 are
 *          skipped it returns k_lfn_scan_continue (line 262). The root fixed
 *          region then sets eod=1 and line 301 is reached.
 * @pre None.
 * @post s_disk contains a FAT16 image; root sector has 16 deleted entries.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_vol_all_deleted(void)
{
  build_fat16_vol();
  uint8_t* root = &s_disk.bytes[(size_t)(uint32_t)k_lcov_root_lba * (uint32_t)k_lcov_blk_sz];
  for (uint32_t i = 0U; i < (uint32_t)k_del_entries; i++) {
    root[(size_t)i * (uint32_t)k_del_stride] = (uint8_t)k_del_marker;
  }
}

/**
 * @brief Release the heap-backed sector store in s_disk.
 * @pre None.
 * @post s_disk.bytes is nullptr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void free_vol(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/**
 * @brief Redirect h->backend to the inject (read-countdown/write-fail) backend.
 * @details Shares s_disk.bytes with s_inject; caller saves and restores
 *          h->backend when done.
 * @param[in,out] h           Mounted volume to redirect.
 * @param[in]     reads_left  Reads allowed before I/O failure.
 * @param[in]     writes_fail Non-zero to make every write fail immediately.
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend uses inj_read / inj_write callbacks.
 * @note Caller must restore h->backend = saved after use.
 * @since 0.1.0
 */
static void swap_to_inject(ra8_fs_mount_t* h, uint32_t reads_left, uint8_t writes_fail)
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
 * @brief Create count empty files in dir_path; names "Gnn.TXT" (8.3 valid).
 * @param[in,out] h        Mounted volume.
 * @param[in]     dir_path Parent directory ("/SUB" or "/").
 * @param[in]     count    Number of files to create (0..14).
 * @pre h is non-NULL and mounted; dir_path is a valid directory.
 * @post count new directory entries exist under dir_path.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void create_files_in(ra8_fs_mount_t* h, const char* dir_path, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
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

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_lfn_cov_checksum_mismatch
 * @brief priv_lfn_name_for returns nullptr when checksum mismatches (line 147).
 *
 * @details A volume with a valid LFN entry BUT a corrupted LDIR_Chksum field
 *          is mounted. ra8_fs_open("mybook.epub") calls priv_dir_find_long,
 *          which calls priv_lfn_name_for. The checksum != computed value
 *          check at line 146 is true; line 147 (return nullptr) is executed.
 *
 * @par MC/DC:
 * Decision at line 146: `s->checksum != priv_sfn_checksum(name83)` (single
 * condition). This test provides the "true" vector: corrupt checksum mismatches
 * the computed one, executing the return path at line 147. The sibling test
 * test_ra8_fs_lfn.c provides the "false" vector (correct checksum -> line 149).
 */
static void test_lfn_cov_checksum_mismatch(void)
{
  TEST_BEGIN("ra8_fs LFN cov: checksum mismatch -> line 147");
  build_vol_lfn_bad_csum();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* "mybook.epub" has a 4-char extension -> have83=0 -> priv_dir_find_long
   * is called directly. The LFN checksum mismatch returns nullptr (line 147);
   * the 8.3 entry is not an LFN match -> k_ra8_err_not_found. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "mybook.epub", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: checksum mismatch -> line 147");
}

/**
 * @test test_lfn_cov_leading_slash
 * @brief priv_dir_find_long strips a leading '/' from the needle (lines 275-276).
 *
 * @details priv_dir_find_long is invoked directly with want="/mybook.epub".
 *          The if-branch at line 274-275 strips the '/' (line 276: needle++)
 *          and the search succeeds, returning k_ra8_ok.
 *
 * @par MC/DC:
 * Decision at line 274: `needle[0] == '/'` (single condition).
 * - This test: needle[0]='/' -> true -> line 276 (needle++) executed.
 * - Normal path via ra8_fs_open: priv_resolve_parent strips the path separator
 *   before passing the leaf to priv_dir_find_long; needle[0]!='/' -> false.
 * Together, both vectors of the single-condition decision are covered.
 */
static void test_lfn_cov_leading_slash(void)
{
  TEST_BEGIN("ra8_fs LFN cov: leading-slash strip -> lines 275-276");
  build_vol_lfn_good();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  dir_loc_t root_loc                      = {.is_root = 1U, .cluster = 0U};
  uint32_t  lba                           = 0U;
  uint32_t  off                           = 0U;
  uint8_t   ent[k_ra8_fs_dir_entry_bytes] = {};
  /* want="/mybook.epub": needle[0]=='/' -> needle++ (lines 275-276),
   * then the scan finds the LFN+8.3 entry -> k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_find_long(h, &root_loc, "/mybook.epub", &lba, &off, ent));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: leading-slash strip -> lines 275-276");
}

/**
 * @test test_lfn_cov_scan_continue_eod
 * @brief priv_dir_find_long_sector returns k_lfn_scan_continue (line 262)
 *        then the fixed-root while-loop exits and line 301 is reached.
 *
 * @details A volume whose entire root sector is filled with 0xE5 (deleted)
 *          entries is mounted. ra8_fs_open("mybook.epub") -> priv_dir_find_long:
 *          all 16 entries are skipped via continue -> k_lfn_scan_continue
 *          (line 262). priv_dir_walk_next_sector sees fixed_remaining=0 -> sets
 *          eod=1. The while-loop exits and line 301 fires.
 *
 * @par MC/DC:
 * Not applicable -- each individual condition on this path is a simple
 * single-branch check already covered individually by this and sibling tests.
 */
static void test_lfn_cov_scan_continue_eod(void)
{
  TEST_BEGIN("ra8_fs LFN cov: scan_continue + eod -> lines 262 + 301");
  build_vol_all_deleted();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  /* All 16 root entries are 0xE5 (deleted) -> priv_dir_find_long_sector
   * hits the continue path 16 times then returns k_lfn_scan_continue (262).
   * Fixed root exhausted -> eod=1 -> while exits -> line 301. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "mybook.epub", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: scan_continue + eod -> lines 262 + 301");
}

/**
 * @test test_lfn_cov_read_fail_long
 * @brief priv_dir_find_long returns the backend error when priv_read_sector
 *        fails at the very first read (line 286).
 *
 * @details A plain FAT16 volume is mounted. reads_left is set to 0 via the
 *          inject backend before calling ra8_fs_open("mybook.epub"). The path
 *          has no sub-directory component so priv_resolve_parent does no I/O.
 *          priv_dir_find_long's first priv_read_sector call fails -> line 286.
 *
 * @par MC/DC:
 * Not applicable -- line 286 is a single-condition early-return on I/O error.
 */
static void test_lfn_cov_read_fail_long(void)
{
  TEST_BEGIN("ra8_fs LFN cov: priv_read_sector fail in find_long -> line 286");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 0U, 0U); /* reads_left=0: first read fails */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "mybook.epub", k_ra8_fs_mode_read, &f));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: priv_read_sector fail in find_long -> line 286");
}

/**
 * @test test_lfn_cov_walk_fail_long
 * @brief priv_dir_find_long returns the backend error when
 *        priv_dir_walk_next_sector fails (lines 262 + 296-298).
 *
 * @details A FAT16 volume with /SUB holding 16 entries (dot, dotdot, 14 files)
 *          is mounted. reads_left is set to 2 via the inject backend, then
 *          ra8_fs_open("/SUB/mybook.epub") is called.
 *          Read 1: root sector (priv_resolve_parent finds SUB).
 *          Read 2: subdir sector (priv_dir_find_long) -> 16 non-matching
 *                  entries -> k_lfn_scan_continue (line 262).
 *          Read 3: FAT sector (priv_dir_walk_next_sector) -> FAILS.
 *          Lines 296-298 are reached.
 *
 * @par MC/DC:
 * Not applicable -- lines 296-298 are a single-condition I/O error return.
 */
static void test_lfn_cov_walk_fail_long(void)
{
  TEST_BEGIN("ra8_fs LFN cov: walk_next fail in find_long -> lines 262+296-298");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  /* 14 files + dot + dotdot = 16 entries; fills the first (only) cluster. */
  create_files_in(h, "/SUB", k_fs_fat_lfn_cov_put16_14);

  ra8_fs_backend_t saved = h->backend;
  /* Read 1 = root sector (find SUB), Read 2 = subdir sector, Read 3 = FAT. */
  swap_to_inject(h, 2U, 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "/SUB/mybook.epub", k_ra8_fs_mode_read, &f));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: walk_next fail in find_long -> lines 262+296-298");
}

/**
 * @test test_lfn_cov_read_fail_free
 * @brief priv_dir_find_free returns the backend error when priv_read_sector
 *        fails at the first read (line 317).
 *
 * @details priv_dir_find_free is called directly via the internal API with
 *          reads_left=0. The very first priv_read_sector call fails and
 *          line 317 is executed.
 *
 * @par MC/DC:
 * Not applicable -- line 317 is a single-condition early-return.
 */
static void test_lfn_cov_read_fail_free(void)
{
  TEST_BEGIN("ra8_fs LFN cov: priv_read_sector fail in find_free -> line 317");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  dir_loc_t        root_loc = {.is_root = 1U, .cluster = 0U};
  uint32_t         lba      = 0U;
  uint32_t         off      = 0U;
  ra8_fs_backend_t saved    = h->backend;
  swap_to_inject(h, 0U, 0U); /* reads_left=0 */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, priv_dir_find_free(h, &root_loc, &lba, &off));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: priv_read_sector fail in find_free -> line 317");
}

/**
 * @test test_lfn_cov_walk_fail_free
 * @brief priv_dir_find_free returns the backend error when
 *        priv_dir_walk_next_sector fails (lines 328-330).
 *
 * @details A FAT16 volume with /SUB holding 16 non-free entries (dot, dotdot,
 *          14 files) is mounted. priv_dir_find_free is called directly on the
 *          subdir location with reads_left=1.
 *          Read 1: subdir sector -> all 16 entries non-free -> loop exits.
 *          Read 2: FAT sector (priv_dir_walk_next_sector) -> FAILS.
 *          Lines 328-330 are reached.
 *
 * @par MC/DC:
 * Not applicable -- lines 328-330 are a single-condition I/O error return.
 */
static void test_lfn_cov_walk_fail_free(void)
{
  TEST_BEGIN("ra8_fs LFN cov: walk_next fail in find_free -> lines 328-330");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  /* dot + dotdot + 14 files = 16 entries; fills cluster 2 entirely. */
  create_files_in(h, "/SUB", k_fs_fat_lfn_cov_put16_14);

  /* Subdir lives at cluster 2 (first allocated after mount on fresh volume). */
  dir_loc_t        subdir_loc = {.is_root = 0U, .cluster = 2U};
  uint32_t         lba        = 0U;
  uint32_t         off        = 0U;
  ra8_fs_backend_t saved      = h->backend;
  /* Read 1 = subdir sector (16 non-free), Read 2 = FAT (fails). */
  swap_to_inject(h, 1U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, priv_dir_find_free(h, &subdir_loc, &lba, &off));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: walk_next fail in find_free -> lines 328-330");
}

/**
 * @test test_lfn_cov_no_mem_free
 * @brief priv_dir_find_free returns k_ra8_err_no_mem when the fixed root dir
 *        is fully occupied (line 333).
 *
 * @details A FAT16 volume with 16 files in the root directory is mounted.
 *          priv_dir_find_free is called directly on the root location.
 *          The 16-entry root sector has no 0x00 or 0xE5 entries -> for-loop
 *          exits without a match. priv_dir_walk_next_sector sees
 *          fixed_remaining=0 -> eod=1. The while-loop exits -> line 333.
 *
 * @par MC/DC:
 * Not applicable -- line 333 is a simple sentinel return when the loop exits.
 */
static void test_lfn_cov_no_mem_free(void)
{
  TEST_BEGIN("ra8_fs LFN cov: root dir full -> line 333 (k_ra8_err_no_mem)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  /* Fill all 16 root entries (F00.TXT through F15.TXT). */
  create_files_in(h, "/", 16U);

  dir_loc_t root_loc = {.is_root = 1U, .cluster = 0U};
  uint32_t  lba      = 0U;
  uint32_t  off      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, priv_dir_find_free(h, &root_loc, &lba, &off));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: root dir full -> line 333 (k_ra8_err_no_mem)");
}

/**
 * @test test_lfn_cov_fat_get_fail
 * @brief priv_free_chain returns the backend error when priv_fat_get fails
 *        on the very first cluster read (line 346).
 *
 * @details A plain FAT16 volume is mounted (count_of_clusters=8126). The
 *          inject backend is armed with reads_left=0 and priv_free_chain(h,2)
 *          is called directly. The while condition (2>=2 && 0<8126) is true;
 *          priv_fat_get reads the FAT sector -> fails -> line 346.
 *
 * @par MC/DC:
 * Decision at line 342: `cur >= k_cluster_first_data &&
 *                        (cur - k_cluster_first_data) < m->count_of_clusters`
 * This test exercises the "enter loop body" vector (both conditions true).
 * The protocol_error test exercises the "exit via cur<2" sub-vector for the
 * outer `while`, and the guard-triggers path for the inner `if`.
 */
static void test_lfn_cov_fat_get_fail(void)
{
  TEST_BEGIN("ra8_fs LFN cov: priv_fat_get fail in free_chain -> line 346");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 0U, 0U); /* reads_left=0: FAT read fails immediately */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, priv_free_chain(h, 2U));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: priv_fat_get fail in free_chain -> line 346");
}

/**
 * @test test_lfn_cov_fat_set_fail
 * @brief priv_free_chain returns the backend error when priv_fat_set fails
 *        to write the FAT entry (line 350).
 *
 * @details A plain FAT16 volume is mounted. The inject backend is armed with
 *          unlimited reads and writes_fail=1. priv_free_chain(h,2) is called;
 *          priv_fat_get(h,2) reads FAT[2]=0 (calloc) successfully, then
 *          priv_fat_set(h,2,0) tries to write -> fails -> line 350.
 *
 * @par MC/DC:
 * Not applicable -- line 350 is a single-condition early-return on write error.
 */
static void test_lfn_cov_fat_set_fail(void)
{
  TEST_BEGIN("ra8_fs LFN cov: priv_fat_set fail in free_chain -> line 350");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_backend_t saved = h->backend;
  /* Unlimited reads, all writes fail. */
  swap_to_inject(h, (uint32_t)k_lcov_reads_inf, 1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, priv_free_chain(h, 2U));
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: priv_fat_set fail in free_chain -> line 350");
}

/**
 * @test test_lfn_cov_protocol_error
 * @brief priv_free_chain fires the cyclic-chain guard and returns
 *        k_ra8_err_protocol_error (line 359).
 *
 * @details A plain FAT16 volume is mounted. FAT entries are written directly
 *          into s_disk.bytes to form the cyclic chain 2->3->4->5->2.
 *          count_of_clusters is overridden to 4 (clusters 2-5 in range).
 *          priv_free_chain(h,2) walks 5 iterations: after freeing clusters
 *          2,3,4,5 it re-enters with cur=2 (already freed), reads FAT[2]=0,
 *          advances to cur=0, increments guard to 5. guard(5) > 4 fires
 *          the protocol_error check at line 359.
 *
 * @par MC/DC:
 * Decision at line 358: `guard > m->count_of_clusters` (single condition).
 * - This test: guard=5 > count_of_clusters=4 -> true -> line 359 executed.
 * - All other tests: guard never exceeds count_of_clusters -> false path.
 * Both vectors of this single-condition decision are covered across the suite.
 */
static void test_lfn_cov_protocol_error(void)
{
  TEST_BEGIN("ra8_fs LFN cov: cyclic FAT guard -> line 359 (protocol_error)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Stamp cyclic chain 2->3->4->5->2 into FAT1 (LBA 1, at byte 512). */
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl2lo] = (uint8_t)k_lcov_cy_b;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl2lo + (uint32_t)k_lcov_fat_hi] =
    0U;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl3lo] = (uint8_t)k_lcov_cy_c;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl3lo + (uint32_t)k_lcov_fat_hi] =
    0U;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl4lo] = (uint8_t)k_lcov_cy_d;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl4lo + (uint32_t)k_lcov_fat_hi] =
    0U;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl5lo] = (uint8_t)k_lcov_cy_a;
  s_disk.bytes[(uint32_t)k_lcov_fat1_off + (uint32_t)k_lcov_fat_cl5lo + (uint32_t)k_lcov_fat_hi] =
    0U;

  /* Override cluster count so that guard=5 > count_of_clusters=4 triggers. */
  h->count_of_clusters = (uint32_t)k_lcov_cy_cnt;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, priv_free_chain(h, (uint32_t)k_lcov_cy_a));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("ra8_fs LFN cov: cyclic FAT guard -> line 359 (protocol_error)");
}

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

int32_t main(void)
{
  test_lfn_cov_checksum_mismatch();
  test_lfn_cov_leading_slash();
  test_lfn_cov_scan_continue_eod();
  test_lfn_cov_read_fail_long();
  test_lfn_cov_walk_fail_long();
  test_lfn_cov_read_fail_free();
  test_lfn_cov_walk_fail_free();
  test_lfn_cov_no_mem_free();
  test_lfn_cov_fat_get_fail();
  test_lfn_cov_fat_set_fail();
  test_lfn_cov_protocol_error();
  return 0;
}
