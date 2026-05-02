/**
 * @file ra_fs_fat.c
 * @brief FAT12/FAT16/FAT32 implementation of the `ra_fs` adapter.
 *
 * @details
 * Sector-by-sector implementation of the on-disk FAT filesystem. We keep
 * exactly one sector of scratch in static storage and re-use it for every
 * BPB, FAT, and directory access -- no dynamic memory anywhere.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000. Section numbers track that PDF.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: every loop is bounded by sector count, cluster count, or
 *     a small enum-defined max.
 *   - Rule 3: zero malloc; all state lives in static arrays.
 *   - Rule 5: every public entry checks pre/post-conditions.
 *   - Rule 7: every backend call is checked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_fs.h"

/* =============================================================================
 * BPB field offsets -- MS FAT spec sec 3.1, table "BPB and BS Fields".
 * Every magic number in the on-disk layout is named here; fat dispatcher
 * code reads via these offsets only.
 * =============================================================================
 */

/**
 * @enum ra_fs_bpb_off_t
 * @brief Byte offsets inside a 512-byte BPB / boot sector.
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11,  /**< MS FAT spec sec 3.1 "BPB_BytsPerSec".  */
  k_bpb_off_sec_per_clus  = 13,  /**< MS FAT spec sec 3.1 "BPB_SecPerClus".  */
  k_bpb_off_rsvd_sec_cnt  = 14,  /**< MS FAT spec sec 3.1 "BPB_RsvdSecCnt".  */
  k_bpb_off_num_fats      = 16,  /**< MS FAT spec sec 3.1 "BPB_NumFATs".     */
  k_bpb_off_root_ent_cnt  = 17,  /**< MS FAT spec sec 3.1 "BPB_RootEntCnt".  */
  k_bpb_off_tot_sec_16    = 19,  /**< MS FAT spec sec 3.1 "BPB_TotSec16".    */
  k_bpb_off_fat_sz_16     = 22,  /**< MS FAT spec sec 3.1 "BPB_FATSz16".     */
  k_bpb_off_tot_sec_32    = 32,  /**< MS FAT spec sec 3.1 "BPB_TotSec32".    */
  k_bpb_off_fat_sz_32     = 36,  /**< MS FAT spec sec 3.5 "BPB_FATSz32".     */
  k_bpb_off_root_clus     = 44,  /**< MS FAT spec sec 3.5 "BPB_RootClus".    */
  k_bpb_off_signature_lo  = 510, /**< Boot sig byte 1 (0x55).                */
  k_bpb_off_signature_hi  = 511, /**< Boot sig byte 2 (0xAA).                */
} ra_fs_bpb_off_t;

/**
 * @enum ra_fs_dir_off_t
 * @brief Byte offsets inside a 32-byte FAT directory entry. MS FAT spec sec 6.
 */
typedef enum : uint8_t {
  k_dir_off_name        = 0,  /**< MS FAT spec sec 6 "DIR_Name" (11 bytes).        */
  k_dir_off_attr        = 11, /**< MS FAT spec sec 6 "DIR_Attr".                   */
  k_dir_off_fst_clus_hi = 20, /**< MS FAT spec sec 6 "DIR_FstClusHI".              */
  k_dir_off_fst_clus_lo = 26, /**< MS FAT spec sec 6 "DIR_FstClusLO".              */
  k_dir_off_file_size   = 28, /**< MS FAT spec sec 6 "DIR_FileSize".               */
  k_dir_name_field_len  = 11, /**< 8 + 3 raw chars (no dot).                       */
} ra_fs_dir_off_t;

/**
 * @enum ra_fs_dir_marker_t
 * @brief Special markers in DIR_Name[0]. MS FAT spec sec 6.
 */
typedef enum : uint8_t {
  k_dir_marker_free_perm = 0x00, /**< End-of-directory.       */
  k_dir_marker_free_used = 0xE5, /**< Slot was used, deleted. */
  k_dir_marker_kanji_e5  = 0x05, /**< 0xE5 in raw name, escaped. */
} ra_fs_dir_marker_t;

/**
 * @enum ra_fs_sig_t
 * @brief Required boot-sector signature bytes (0x55 0xAA at off 510..511).
 */
typedef enum : uint8_t {
  k_bpb_sig_lo = 0x55,
  k_bpb_sig_hi = 0xAA,
} ra_fs_sig_t;

/**
 * @enum ra_fs_cluster_t
 * @brief Reserved cluster numbers used by the FAT itself.
 */
typedef enum : uint32_t {
  k_cluster_first_data      = 2,           /**< Cluster numbers start at 2.        */
  k_cluster_eoc_min_fat12   = 0x0FF8,      /**< MS FAT spec sec 4.1 EOC threshold.*/
  k_cluster_eoc_min_fat16   = 0xFFF8,      /**< MS FAT spec sec 4.1 EOC threshold.*/
  k_cluster_eoc_min_fat32   = 0x0FFFFFF8,  /**< MS FAT spec sec 4.2 EOC threshold.*/
  k_cluster_mask_fat32      = 0x0FFFFFFFU, /**< Top 4 bits reserved on FAT32.      */
  k_cluster_eoc_write_fat12 = 0x0FFF,      /**< Value we write to terminate.       */
  k_cluster_eoc_write_fat16 = 0xFFFF,
  k_cluster_eoc_write_fat32 = 0x0FFFFFFFU,
  k_cluster_free            = 0,
  k_cluster_count_fat12_max = 4085,    /**< MS FAT spec sec 3.5 boundary.      */
  k_cluster_count_fat16_max = 65525,   /**< MS FAT spec sec 3.5 boundary.      */
  k_fat12_value_mask        = 0x0FFFU, /**< 12-bit FAT12 entry mask.           */
  k_fat12_low_nibble_mask   = 0x000FU, /**< Untouched nibble (even cluster).   */
  k_fat12_high_nibble_mask  = 0xF000U, /**< Untouched nibble (odd cluster).    */
} ra_fs_cluster_t;

/**
 * @enum ra_fs_misc_t
 * @brief Misc small constants used by parsing/formatting code.
 */
typedef enum : uint16_t {
  k_byte_mask              = 0xFFU,
  k_shift_byte             = 8,
  k_shift_two_bytes        = 16,
  k_shift_three_bytes      = 24,
  k_shift_nibble           = 4,
  k_nibble_mask            = 0x0F,
  k_byte_mask_full         = 0xFF,
  k_word_mask              = 0xFFFFU,
  k_max_8_3_name           = 11, /**< 8.3 packed length without dot. */
  k_dot_pos                = 8,  /**< In an 8.3 packed name, dot would go here. */
  k_filename_base_len      = 8,
  k_filename_ext_len       = 3,
  k_dir_entries_per_sector = 16, /**< 512 / 32 */
  k_path_max               = 64,
} ra_fs_misc_t;

/* =============================================================================
 * Module state
 * =============================================================================
 */

/** @brief Mount table -- max `k_ra_fs_max_mounts` simultaneous volumes. */
static ra_fs_mount_t s_mounts[k_ra_fs_max_mounts] = {};

/** @brief File handle table -- max `k_ra_fs_max_files` open at once. */
static ra_fs_file_t s_files[k_ra_fs_max_files] = {};

/** @brief Single 512-byte sector scratch buffer reused across all I/O. */
static uint8_t s_scratch[k_ra_fs_bytes_per_sector] = {};

/* =============================================================================
 * Little-endian helpers
 * =============================================================================
 */

/**
 * @brief Decode a little-endian uint16_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler. Avoids `memcpy` so
 *          clang-tidy's strict-alias check stays happy.
 *
 * @param[in] p Pointer to two bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT16_MAX  Value assembled from `p[0]` and `p[1]`.
 *
 * @pre `p` is non-NULL and points to at least 2 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1] << 8)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint16_t priv_rd16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << k_shift_byte));
}

/**
 * @brief Decode a little-endian uint32_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler for 4 bytes.
 *
 * @param[in] p Pointer to four bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT32_MAX  Value assembled from `p[0..3]`.
 *
 * @pre `p` is non-NULL and points to at least 4 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint32_t priv_rd32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_shift_byte) | ((uint32_t)p[2] << k_shift_two_bytes) |
         ((uint32_t)p[3] << k_shift_three_bytes);
}

/**
 * @brief Encode a little-endian uint16_t into a byte buffer.
 *
 * @details Inverse of `priv_rd16`. Writes the low byte first.
 *
 * @param[out] p Pointer to two writable bytes.
 * @param[in]  v Value to encode.
 *
 * @return None.
 * @retval None
 *
 * @pre `p` is non-NULL and points to at least 2 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0]` and `p[1]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
static void priv_wr16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & k_byte_mask);
  p[1] = (uint8_t)((v >> k_shift_byte) & k_byte_mask);
}

/**
 * @brief Encode a little-endian uint32_t into a byte buffer.
 *
 * @details Inverse of `priv_rd32`. Writes lowest byte first.
 *
 * @param[out] p Pointer to four writable bytes.
 * @param[in]  v Value to encode.
 *
 * @return None.
 * @retval None
 *
 * @pre `p` is non-NULL and points to at least 4 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0..3]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
static void priv_wr32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & k_byte_mask);
  p[1] = (uint8_t)((v >> k_shift_byte) & k_byte_mask);
  p[2] = (uint8_t)((v >> k_shift_two_bytes) & k_byte_mask);
  p[3] = (uint8_t)((v >> k_shift_three_bytes) & k_byte_mask);
}

/**
 * @brief Length-checked byte copy used in place of memcpy().
 *
 * @details
 * Replaces memcpy() so clang-tidy's `clang-analyzer-security.insecureAPI`
 * checker stays happy. Same effect on -O2 generated code.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  src Source buffer.
 * @param[in]  n   Number of bytes to copy.
 *
 * @return None.
 * @retval None
 *
 * @pre `dst` and `src` are non-NULL and point to at least `n` bytes.
 * @pre `dst` and `src` do not overlap.
 * @post First `n` bytes of `dst` equal first `n` bytes of `src`.
 * @post No state outside `dst` is modified.
 *
 * @note Bounded loop, NASA Rule 2 compliant.
 *
 * @since 0.1.0
 */
static void priv_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/**
 * @brief Compare two byte buffers for equality (length n).
 *
 * @details Returns early on first mismatch. Used in place of memcmp().
 *
 * @param[in] a First buffer.
 * @param[in] b Second buffer.
 * @param[in] n Number of bytes to compare.
 *
 * @return 1 on equal, 0 on mismatch.
 * @retval 1  All `n` bytes equal.
 * @retval 0  At least one byte differs.
 *
 * @pre `a` and `b` are non-NULL and point to at least `n` bytes.
 * @pre Caller has bounds-checked both buffers.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint8_t priv_byte_equal(const uint8_t* a, const uint8_t* b, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    if (a[i] != b[i]) {
      return 0U;
    }
  }
  return 1U;
}

/* =============================================================================
 * Backend wrappers
 * =============================================================================
 */

/**
 * @brief Read a single sector into the module scratch buffer.
 *
 * @details Forwards to the mount's `backend.read_block` callback.
 *
 * @param[in]  m   Mount whose backend to use.
 * @param[in]  lba Logical block address to read.
 * @param[out] buf Destination of `k_ra_fs_bytes_per_sector` bytes.
 *
 * @return Backend-supplied error code.
 * @retval k_ra_ok    Sector read successfully.
 * @retval k_ra_err_* Whatever the backend returned.
 *
 * @pre `m`, `m->backend.read_block`, and `buf` are non-NULL.
 * @pre `lba` is within the volume's addressable range.
 * @post On success, `buf` holds the sector contents.
 * @post On failure, `buf` content is undefined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_read_sector(const ra_fs_mount_t* m, uint32_t lba, uint8_t* buf)
{
  return m->backend.read_block(m->backend.ctx, lba, 1, buf);
}

/**
 * @brief Write a single sector from a caller-provided buffer.
 *
 * @details Forwards to the mount's `backend.write_block` callback.
 *
 * @param[in] m   Mount whose backend to use.
 * @param[in] lba Logical block address to write.
 * @param[in] buf Source of `k_ra_fs_bytes_per_sector` bytes.
 *
 * @return Backend-supplied error code.
 * @retval k_ra_ok    Sector written successfully.
 * @retval k_ra_err_* Whatever the backend returned.
 *
 * @pre `m`, `m->backend.write_block`, and `buf` are non-NULL.
 * @pre `lba` is within the volume's addressable range.
 * @post On success, the underlying backend has the new sector contents.
 * @post On failure, backend state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_write_sector(const ra_fs_mount_t* m, uint32_t lba, const uint8_t* buf)
{
  return m->backend.write_block(m->backend.ctx, lba, 1, buf);
}

/* =============================================================================
 * FAT entry get/set -- dispatches across FAT12/16/32 layouts
 * =============================================================================
 */

/**
 * @brief Compute the byte offset of `cluster`'s FAT entry for this FAT type.
 *
 * @details FAT12 entries are 1.5 bytes, FAT16 are 2 bytes, FAT32 are 4
 *          bytes. Result is the byte offset within the FAT region.
 *
 * @param[in] m       Mount providing the FAT type.
 * @param[in] cluster Cluster number to look up.
 *
 * @return Byte offset within the FAT region.
 * @retval 0..UINT32_MAX  Byte offset.
 *
 * @pre `m` is non-NULL.
 * @pre `cluster` is within the addressable cluster range.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint32_t priv_fat_entry_byte_offset(const ra_fs_mount_t* m, uint32_t cluster)
{
  if (m->type == k_ra_fs_type_fat12) {
    return cluster + (cluster / 2U);
  }
  if (m->type == k_ra_fs_type_fat16) {
    return cluster * 2U;
  }
  return cluster * 4U;
}

/**
 * @brief Fetch the FAT entry for `cluster`, returning the next-cluster value.
 *
 * @details
 * On FAT12 a single entry can straddle two sectors, which is why we read
 * one sector at a time and re-read on overflow.
 *
 * @param[in]  m         Mount providing the FAT type and geometry.
 * @param[in]  cluster   Cluster whose FAT entry to read.
 * @param[out] out_value Receives the next-cluster value.
 *
 * @return Error code.
 * @retval k_ra_ok    Entry read successfully.
 * @retval k_ra_err_* Backend error from a sector read.
 *
 * @pre `m` and `out_value` are non-NULL.
 * @pre `cluster` is within the addressable cluster range.
 * @post On success, `*out_value` holds the FAT entry.
 * @post Stack buffers used; module scratch untouched.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_fat_get(const ra_fs_mount_t* m, uint32_t cluster, uint32_t* out_value)
{
  const uint32_t fat_offset = priv_fat_entry_byte_offset(m, cluster);
  const uint32_t sec_num    = m->first_fat_lba + (fat_offset / k_ra_fs_bytes_per_sector);
  const uint32_t sec_off    = fat_offset % k_ra_fs_bytes_per_sector;

  uint8_t  buf[k_ra_fs_bytes_per_sector] = {};
  ra_err_t err                           = priv_read_sector(m, sec_num, buf);
  if (err != k_ra_ok) {
    return err;
  }

  uint32_t v = 0;
  if (m->type == k_ra_fs_type_fat12) {
    /* MS FAT spec sec 4.1: read 16 bits straddling the byte and shift. */
    uint8_t b0 = buf[sec_off];
    uint8_t b1 = 0;
    if (sec_off + 1U < k_ra_fs_bytes_per_sector) {
      b1 = buf[sec_off + 1U];
    } else {
      uint8_t buf2[k_ra_fs_bytes_per_sector] = {};
      err                                    = priv_read_sector(m, sec_num + 1U, buf2);
      if (err != k_ra_ok) {
        return err;
      }
      b1 = buf2[0];
    }
    uint16_t raw = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << k_shift_byte));
    if ((cluster & 1U) != 0U) {
      v = (uint32_t)(raw >> k_shift_nibble);
    } else {
      v = (uint32_t)(raw & k_fat12_value_mask);
    }
  } else if (m->type == k_ra_fs_type_fat16) {
    v = priv_rd16(&buf[sec_off]);
  } else {
    v = priv_rd32(&buf[sec_off]) & k_cluster_mask_fat32;
  }

  *out_value = v;
  return k_ra_ok;
}

/**
 * @brief Write a FAT12 entry, handling sector-straddling 12-bit packing.
 *
 * @details FAT12 entries are 12 bits and may straddle a sector boundary.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry's first byte.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] cluster Cluster number (used to pick low/high nibble).
 * @param[in] value   12-bit value to write (low 12 bits used).
 *
 * @return Error code.
 * @retval k_ra_ok    Entry updated.
 * @retval k_ra_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off < k_ra_fs_bytes_per_sector`.
 * @post On success, the FAT12 entry on disk reflects the new value.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_fat12_set_one(const ra_fs_mount_t* m,
                                   uint32_t             sec_num,
                                   uint32_t             sec_off,
                                   uint32_t             cluster,
                                   uint32_t             value)
{
  uint8_t  buf[k_ra_fs_bytes_per_sector]  = {};
  uint8_t  buf2[k_ra_fs_bytes_per_sector] = {};
  uint8_t  straddle                       = 0U;
  ra_err_t err                            = priv_read_sector(m, sec_num, buf);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t b0 = buf[sec_off];
  uint8_t b1 = 0;
  if (sec_off + 1U < (uint32_t)k_ra_fs_bytes_per_sector) {
    b1 = buf[sec_off + 1U];
  } else {
    err = priv_read_sector(m, sec_num + 1U, buf2);
    if (err != k_ra_ok) {
      return err;
    }
    b1       = buf2[0];
    straddle = 1U;
  }
  uint16_t raw = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << k_shift_byte));
  if ((cluster & 1U) != 0U) {
    raw = (uint16_t)((raw & k_fat12_low_nibble_mask) |
                     (uint16_t)((value & k_fat12_value_mask) << k_shift_nibble));
  } else {
    raw = (uint16_t)((raw & k_fat12_high_nibble_mask) | (uint16_t)(value & k_fat12_value_mask));
  }
  buf[sec_off] = (uint8_t)(raw & k_byte_mask);
  if (straddle == 0U) {
    buf[sec_off + 1U] = (uint8_t)((raw >> k_shift_byte) & k_byte_mask);
  } else {
    buf2[0] = (uint8_t)((raw >> k_shift_byte) & k_byte_mask);
  }
  err = priv_write_sector(m, sec_num, buf);
  if (err != k_ra_ok) {
    return err;
  }
  if (straddle != 0U) {
    err = priv_write_sector(m, sec_num + 1U, buf2);
  }
  return err;
}

/**
 * @brief Write a FAT16 entry into one sector.
 *
 * @details FAT16 entries never straddle sectors. One read/write cycle
 *          updates the entry.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] value   Value to write (low 16 bits used).
 *
 * @return Error code.
 * @retval k_ra_ok    Entry updated.
 * @retval k_ra_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off <= k_ra_fs_bytes_per_sector - 2`.
 * @post On success, the FAT16 entry on disk reflects the new value.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t
priv_fat16_set_one(const ra_fs_mount_t* m, uint32_t sec_num, uint32_t sec_off, uint32_t value)
{
  uint8_t  buf[k_ra_fs_bytes_per_sector] = {};
  ra_err_t err                           = priv_read_sector(m, sec_num, buf);
  if (err != k_ra_ok) {
    return err;
  }
  priv_wr16(&buf[sec_off], (uint16_t)(value & k_word_mask));
  return priv_write_sector(m, sec_num, buf);
}

/**
 * @brief Write a FAT32 entry into one sector (preserves top 4 reserved bits).
 *
 * @details FAT32 entries are 32 bits but only the low 28 bits are
 *          cluster data; the high 4 reserved bits must be preserved.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] value   Value to write (low 28 bits used).
 *
 * @return Error code.
 * @retval k_ra_ok    Entry updated.
 * @retval k_ra_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off <= k_ra_fs_bytes_per_sector - 4`.
 * @post Low 28 bits of the entry equal `value`; high 4 bits preserved.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t
priv_fat32_set_one(const ra_fs_mount_t* m, uint32_t sec_num, uint32_t sec_off, uint32_t value)
{
  uint8_t  buf[k_ra_fs_bytes_per_sector] = {};
  ra_err_t err                           = priv_read_sector(m, sec_num, buf);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t prev = priv_rd32(&buf[sec_off]) & ~k_cluster_mask_fat32;
  priv_wr32(&buf[sec_off], (value & k_cluster_mask_fat32) | prev);
  return priv_write_sector(m, sec_num, buf);
}

/**
 * @brief Write `value` into the FAT entry for `cluster` across every FAT copy.
 *
 * @details Walks `m->num_fats` FAT copies and dispatches to the
 *          appropriate FAT12/16/32 set helper.
 *
 * @param[in] m       Mount providing geometry, backend, and FAT type.
 * @param[in] cluster Cluster whose FAT entry to update.
 * @param[in] value   Value to write.
 *
 * @return Error code.
 * @retval k_ra_ok    All FAT copies updated.
 * @retval k_ra_err_* Backend or set-helper failure.
 *
 * @pre `m` is non-NULL with a valid backend and `num_fats >= 1`.
 * @pre `cluster` is within the addressable cluster range.
 * @post On success, every FAT copy reflects the new value.
 * @post On partial failure, FAT copies may be inconsistent.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_fat_set(const ra_fs_mount_t* m, uint32_t cluster, uint32_t value)
{
  const uint32_t fat_offset = priv_fat_entry_byte_offset(m, cluster);
  for (uint32_t i = 0; i < m->num_fats; i++) {
    const uint32_t fat_base = m->first_fat_lba + (i * m->fat_size_sectors);
    const uint32_t sec_num  = fat_base + (fat_offset / k_ra_fs_bytes_per_sector);
    const uint32_t sec_off  = fat_offset % k_ra_fs_bytes_per_sector;
    ra_err_t       err      = k_ra_ok;
    if (m->type == k_ra_fs_type_fat12) {
      err = priv_fat12_set_one(m, sec_num, sec_off, cluster, value);
    } else if (m->type == k_ra_fs_type_fat16) {
      err = priv_fat16_set_one(m, sec_num, sec_off, value);
    } else {
      err = priv_fat32_set_one(m, sec_num, sec_off, value);
    }
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Test whether `value` is an end-of-chain marker for this FAT type.
 *
 * @details EOC markers differ across FAT12/16/32.
 *
 * @param[in] m     Mount providing the FAT type.
 * @param[in] value FAT entry value to test.
 *
 * @return 1 if EOC, 0 otherwise.
 * @retval 1  `value` indicates end-of-chain.
 * @retval 0  `value` is a normal next-cluster pointer.
 *
 * @pre `m` is non-NULL.
 * @pre `value` was obtained from a FAT entry read.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint8_t priv_is_eoc(const ra_fs_mount_t* m, uint32_t value)
{
  if (m->type == k_ra_fs_type_fat12) {
    return (uint8_t)(value >= k_cluster_eoc_min_fat12 ? 1U : 0U);
  }
  if (m->type == k_ra_fs_type_fat16) {
    return (uint8_t)(value >= k_cluster_eoc_min_fat16 ? 1U : 0U);
  }
  return (uint8_t)(value >= k_cluster_eoc_min_fat32 ? 1U : 0U);
}

/**
 * @brief End-of-chain value to write for this FAT type.
 *
 * @details Returns the canonical EOC value (`0xFFF`, `0xFFFF`, or
 *          `0x0FFFFFFF`).
 *
 * @param[in] m Mount providing the FAT type.
 *
 * @return Canonical EOC value for this volume.
 * @retval k_cluster_eoc_write_fat12   FAT12 EOC.
 * @retval k_cluster_eoc_write_fat16   FAT16 EOC.
 * @retval k_cluster_eoc_write_fat32   FAT32 EOC.
 *
 * @pre `m` is non-NULL.
 * @pre `m->type` has been computed by `priv_compute_geometry`.
 * @post No state modified.
 * @post Result is purely a function of `m->type`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint32_t priv_eoc_write(const ra_fs_mount_t* m)
{
  if (m->type == k_ra_fs_type_fat12) {
    return k_cluster_eoc_write_fat12;
  }
  if (m->type == k_ra_fs_type_fat16) {
    return k_cluster_eoc_write_fat16;
  }
  return k_cluster_eoc_write_fat32;
}

/**
 * @brief Convert a cluster number into its first data-region LBA.
 *
 * @details Cluster numbering starts at `k_cluster_first_data` (= 2).
 *
 * @param[in] m       Mount providing geometry.
 * @param[in] cluster Cluster number (>= `k_cluster_first_data`).
 *
 * @return Sector LBA of the cluster's first sector.
 * @retval 0..UINT32_MAX  Computed LBA.
 *
 * @pre `m` is non-NULL with valid geometry.
 * @pre `cluster >= k_cluster_first_data`.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint32_t priv_cluster_to_lba(const ra_fs_mount_t* m, uint32_t cluster)
{
  return m->first_data_lba + ((cluster - k_cluster_first_data) * m->sectors_per_cluster);
}

/**
 * @brief Linear free-cluster scan -- no FSInfo cache. O(count_of_clusters).
 *
 * @details Walks every cluster looking for one whose FAT entry is
 *          `k_cluster_free`. Returns the first match.
 *
 * @param[in]  m           Mount providing geometry and backend.
 * @param[out] out_cluster On success, the allocated cluster number.
 *
 * @return Error code.
 * @retval k_ra_ok          Cluster found; `*out_cluster` set.
 * @retval k_ra_err_no_mem  Volume is full -- no free clusters.
 * @retval k_ra_err_*       Backend read failure.
 *
 * @pre `m` and `out_cluster` are non-NULL.
 * @pre Volume is mounted and geometry is valid.
 * @post On success, `*out_cluster` is in range and free.
 * @post On failure, `*out_cluster` is unspecified.
 *
 * @note Caller must mark the cluster as EOC after carving it.
 *
 * @since 0.1.0
 */
static ra_err_t priv_alloc_cluster(const ra_fs_mount_t* m, uint32_t* out_cluster)
{
  for (uint32_t c = k_cluster_first_data; c < (k_cluster_first_data + m->count_of_clusters); c++) {
    uint32_t v   = 0;
    ra_err_t err = priv_fat_get(m, c, &v);
    if (err != k_ra_ok) {
      return err;
    }
    if (v == k_cluster_free) {
      *out_cluster = c;
      return k_ra_ok;
    }
  }
  return k_ra_err_no_mem;
}

/* =============================================================================
 * 8.3 short name pack / unpack
 * =============================================================================
 */

/**
 * @brief Convert "FILE.TXT" (caller-supplied path) to packed 11-byte 8.3.
 *
 * @details
 * Result is space-padded as on-disk. Lower-case input is upper-cased.
 * Returns 0 on bad name (>8 base, >3 ext, missing chars), 1 on success.
 */
/**
 * @brief Upper-case ASCII conversion (returns input unchanged if not lowercase).
 *
 * @details Locale-independent ASCII upcase.
 *
 * @param[in] c Input character.
 *
 * @return Upper-case form of `c` if it was lower-case ASCII, else `c`.
 * @retval 'A'..'Z'   Upper-cased input.
 * @retval c          Otherwise unchanged.
 *
 * @pre None.
 * @pre Caller wants ASCII-only handling.
 * @post No state modified.
 * @post Result is purely a function of `c`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static char priv_to_upper(char c)
{
  if (c >= 'a' && c <= 'z') {
    return (char)(c - 'a' + 'A');
  }
  return c;
}

/**
 * @brief Pack the base portion of a path into out11[0..7]. Returns 0 on error.
 *
 * @details Reads characters from `*path_io` up to a `.` or NUL,
 *          upper-cases them, and writes them into `out11[0..7]`.
 *
 * @param[in,out] path_io Cursor into the input path; advanced on success.
 * @param[out]    out11   11-byte buffer; first 8 bytes are written.
 *
 * @return 1 on success, 0 on overflow or empty base.
 * @retval 1  Base name packed.
 * @retval 0  Base too long or zero-length.
 *
 * @pre `path_io`, `*path_io`, and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[0..7]` holds the upper-cased base.
 * @post On success, `*path_io` points at the `.` or terminator.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */
static uint8_t priv_pack_base(const char** path_io, uint8_t* out11)
{
  const char* path     = *path_io;
  uint8_t     base_len = 0;
  while (*path != '\0' && *path != '.') {
    if (base_len >= k_filename_base_len) {
      return 0U;
    }
    out11[base_len++] = (uint8_t)priv_to_upper(*path++);
  }
  if (base_len == 0U) {
    return 0U;
  }
  *path_io = path;
  return 1U;
}

/**
 * @brief Pack the extension portion of a path into out11[8..10]. Returns 0 on error.
 *
 * @details If `*path` is not `.`, returns success with no writes.
 *
 * @param[in]  path  Cursor at the `.` or terminator following the base.
 * @param[out] out11 11-byte buffer; bytes 8..10 are written.
 *
 * @return 1 on success, 0 on overflow.
 * @retval 1  Extension packed (or absent).
 * @retval 0  Extension too long.
 *
 * @pre `path` and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[8..10]` holds the upper-cased extension.
 * @post `*path` is not modified.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */
static uint8_t priv_pack_ext(const char* path, uint8_t* out11)
{
  if (*path != '.') {
    return 1U;
  }
  path++;
  uint32_t ext_len = 0U;
  while (*path != '\0') {
    if (ext_len >= (uint32_t)k_filename_ext_len) {
      return 0U;
    }
    out11[k_filename_base_len + ext_len] = (uint8_t)priv_to_upper(*path++);
    ext_len++;
  }
  return 1U;
}

/**
 * @brief Convert a "/FILE.TXT"-style path to packed 11-byte 8.3 form.
 *
 * @details Strips leading `/`, pre-pads `out11` with spaces, calls the
 *          base/extension packers, and rewrites a leading 0xE5 byte to
 *          the kanji escape 0x05.
 *
 * @param[in]  path  NUL-terminated input path. Must be non-NULL.
 * @param[out] out11 11-byte output buffer. Must be non-NULL.
 *
 * @return 1 on success, 0 on invalid name.
 * @retval 1  Name packed into `out11`.
 * @retval 0  NULL input or name violates 8.3 rules.
 *
 * @pre `path` and `out11` are non-NULL when valid.
 * @pre `out11` has at least `k_max_8_3_name` writable bytes.
 * @post On success, `out11` holds the on-disk 8.3 representation.
 * @post On failure, `out11` content is unspecified.
 *
 * @note Pure ASCII upcase; no locale support.
 *
 * @since 0.1.0
 */
static uint8_t priv_path_to_83(const char* path, uint8_t* out11)
{
  if (path == NULL || out11 == NULL) {
    return 0U;
  }
  while (*path == '/') {
    path++;
  }
  for (uint32_t i = 0; i < (uint32_t)k_max_8_3_name; i++) {
    out11[i] = ' ';
  }
  if (priv_pack_base(&path, out11) == 0U) {
    return 0U;
  }
  if (priv_pack_ext(path, out11) == 0U) {
    return 0U;
  }
  if (out11[0] == k_dir_marker_free_used) {
    out11[0] = k_dir_marker_kanji_e5;
  }
  return 1U;
}

/**
 * @brief Unpack on-disk 11-byte 8.3 name into NUL-terminated "NAME.EXT".
 *
 * @details Trims trailing space pad in the base portion, restores the
 *          0x05 -> 0xE5 kanji escape, and emits the dot + extension
 *          only when the extension is non-empty.
 *
 * @param[in]  in11  Packed 11-byte name.
 * @param[out] out12 Buffer of at least 12 bytes (8 + . + 3 + NUL).
 *
 * @return None.
 * @retval None
 *
 * @pre `in11` and `out12` are non-NULL.
 * @pre `out12` has at least 13 writable bytes.
 * @post `out12` is NUL-terminated.
 * @post Trailing space padding has been stripped.
 *
 * @note Helper used only by `ra_fs_listdir`.
 *
 * @since 0.1.0
 */
static void priv_83_to_str(const uint8_t* in11, char* out12)
{
  uint32_t i = 0;
  uint32_t j = 0;
  for (i = 0; i < (uint32_t)k_filename_base_len; i++) {
    if (in11[i] == ' ') {
      break;
    }
    out12[j++] = (char)in11[i];
  }
  /* Restore kanji escape. */
  if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 && in11[0] == k_dir_marker_kanji_e5) {
    out12[0] = (char)k_dir_marker_free_used;
  }
  uint8_t has_ext = 0;
  for (i = 0; i < k_filename_ext_len; i++) {
    if (in11[k_filename_base_len + i] != ' ') {
      has_ext = 1;
      break;
    }
  }
  if (has_ext != 0U) {
    out12[j++] = '.';
    for (i = 0; i < k_filename_ext_len; i++) {
      if (in11[k_filename_base_len + i] == ' ') {
        break;
      }
      out12[j++] = (char)in11[k_filename_base_len + i];
    }
  }
  out12[j] = '\0';
}

/* =============================================================================
 * Directory walking
 * =============================================================================
 */

/**
 * @struct dir_walk_t
 * @brief Internal cursor used by the directory iterator.
 */
typedef struct {
  uint8_t  is_root_fixed;     /**< 1 = FAT12/16 fixed root dir region. */
  uint32_t fixed_remaining;   /**< Sectors left in fixed region.       */
  uint32_t cluster;           /**< Current cluster (FAT32 root case).  */
  uint32_t sector_in_cluster; /**< 0..SPC-1 inside cluster.            */
  uint32_t cur_lba;           /**< Currently loaded LBA.               */
  uint32_t entry_idx;         /**< Byte offset within the loaded sector. */
} dir_walk_t;

/**
 * @brief Initialise a walker that iterates the volume root directory.
 *
 * @details FAT12/16 use a fixed root region; FAT32 uses a cluster
 *          chain rooted at `m->root_cluster`.
 *
 * @param[in]  m Mount providing geometry and FAT type.
 * @param[out] w Walker cursor to initialise.
 *
 * @return None.
 * @retval None
 *
 * @pre `m` and `w` are non-NULL.
 * @pre `m` has been fully populated by `priv_compute_geometry`.
 * @post `w` points at the first sector of the root directory.
 * @post `w->entry_idx` is zero.
 *
 * @note Pure init -- does not touch the backend.
 *
 * @since 0.1.0
 */
static void priv_dir_walk_init_root(const ra_fs_mount_t* m, dir_walk_t* w)
{
  if (m->type == k_ra_fs_type_fat32) {
    w->is_root_fixed     = 0;
    w->fixed_remaining   = 0;
    w->cluster           = m->root_cluster;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    const uint32_t root_dir_sectors =
      ((m->root_entries * k_ra_fs_dir_entry_bytes) + (k_ra_fs_bytes_per_sector - 1U)) /
      k_ra_fs_bytes_per_sector;
    w->is_root_fixed     = 1;
    w->fixed_remaining   = root_dir_sectors;
    w->cluster           = 0;
    w->sector_in_cluster = 0;
    w->cur_lba           = m->first_root_lba;
  }
  w->entry_idx = 0;
}

/**
 * @brief Advance the walker to the next sector.
 *
 * @details For fixed-region roots simply increments the LBA. For
 *          cluster-chain roots advances within the cluster, then
 *          follows the FAT chain when the cluster is exhausted.
 *
 * @param[in]     m       Mount providing geometry and backend.
 * @param[in,out] w       Walker cursor to advance.
 * @param[out]    out_eod Set to 1 if end-of-directory reached, else 0.
 *
 * @return Error code.
 * @retval k_ra_ok    Walker advanced (or EOD signalled in `*out_eod`).
 * @retval k_ra_err_* Backend error from a FAT read.
 *
 * @pre `m`, `w`, and `out_eod` are non-NULL.
 * @pre Walker has been initialised by `priv_dir_walk_init_root`.
 * @post On success `w` either points at a new sector or `*out_eod` is 1.
 * @post `w->entry_idx` is reset to 0 on a successful advance.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_dir_walk_next_sector(const ra_fs_mount_t* m, dir_walk_t* w, uint8_t* out_eod)
{
  *out_eod = 0;
  if (w->is_root_fixed != 0U) {
    if (w->fixed_remaining <= 1U) {
      *out_eod = 1;
      return k_ra_ok;
    }
    w->fixed_remaining--;
    w->cur_lba++;
    w->entry_idx = 0;
    return k_ra_ok;
  }
  /* FAT32 cluster-chain root. */
  w->sector_in_cluster++;
  if (w->sector_in_cluster >= m->sectors_per_cluster) {
    uint32_t next = 0;
    ra_err_t err  = priv_fat_get(m, w->cluster, &next);
    if (err != k_ra_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      *out_eod = 1;
      return k_ra_ok;
    }
    w->cluster           = next;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    w->cur_lba++;
  }
  w->entry_idx = 0;
  return k_ra_ok;
}

/**
 * @brief Find a directory entry by 8.3 name.
 *
 * @details Walks the root directory and matches on the packed 11-byte
 *          name field. Skips LFN entries (attr 0x0F) and deleted slots.
 *
 * @param[in]  m             Mount providing geometry and backend.
 * @param[in]  name83        Packed 11-byte name.
 * @param[out] out_lba       Sector containing the entry.
 * @param[out] out_entry_off Byte offset within the sector.
 * @param[out] out_entry     32 bytes of the entry payload.
 *
 * @return Error code.
 * @retval k_ra_ok            Entry found; out parameters populated.
 * @retval k_ra_err_not_found End-of-directory reached without a match.
 * @retval k_ra_err_*         Backend error.
 *
 * @pre All output pointers are non-NULL.
 * @pre `name83` is non-NULL and points to 11 bytes.
 * @post On success, out parameters identify the on-disk entry.
 * @post On failure, out parameters are unspecified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_dir_find(const ra_fs_mount_t* m,
                              const uint8_t*       name83,
                              uint32_t*            out_lba,
                              uint32_t*            out_entry_off,
                              uint8_t              out_entry[k_ra_fs_dir_entry_bytes])
{
  dir_walk_t w = {};
  priv_dir_walk_init_root(m, &w);
  uint8_t eod                           = 0;
  uint8_t buf[k_ra_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra_ok) {
      return err;
    }
    for (uint32_t e = 0; e < k_dir_entries_per_sector; e++) {
      uint8_t* ent = &buf[(size_t)e * (size_t)k_ra_fs_dir_entry_bytes];
      if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
        return k_ra_err_not_found;
      }
      if (ent[k_dir_off_name] == k_dir_marker_free_used) {
        continue;
      }
      if (ent[k_dir_off_attr] == k_ra_fs_attr_lfn) {
        continue;
      }
      if (priv_byte_equal(ent, name83, k_dir_name_field_len) != 0U) {
        *out_lba       = w.cur_lba;
        *out_entry_off = e * (uint32_t)k_ra_fs_dir_entry_bytes;
        priv_byte_copy(out_entry, ent, k_ra_fs_dir_entry_bytes);
        return k_ra_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief Locate the first free entry slot in the root directory.
 *
 * @details Walks the root and returns the first entry whose name
 *          field is 0x00 (never used) or 0xE5 (deleted).
 *
 * @param[in]  m             Mount providing geometry and backend.
 * @param[out] out_lba       Sector containing the free entry.
 * @param[out] out_entry_off Byte offset within the sector.
 *
 * @return Error code.
 * @retval k_ra_ok          Free slot found; out parameters populated.
 * @retval k_ra_err_no_mem  Root directory has no free slot.
 * @retval k_ra_err_*       Backend error.
 *
 * @pre All output pointers are non-NULL.
 * @pre `m` is mounted with valid geometry.
 * @post On success, out parameters identify a writable slot.
 * @post On failure, out parameters are unspecified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t
priv_dir_find_free(const ra_fs_mount_t* m, uint32_t* out_lba, uint32_t* out_entry_off)
{
  dir_walk_t w = {};
  priv_dir_walk_init_root(m, &w);
  uint8_t eod                           = 0;
  uint8_t buf[k_ra_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra_ok) {
      return err;
    }
    for (uint32_t e = 0; e < k_dir_entries_per_sector; e++) {
      uint8_t* ent = &buf[(size_t)e * (size_t)k_ra_fs_dir_entry_bytes];
      if (ent[k_dir_off_name] == k_dir_marker_free_perm ||
          ent[k_dir_off_name] == k_dir_marker_free_used) {
        *out_lba       = w.cur_lba;
        *out_entry_off = e * (uint32_t)k_ra_fs_dir_entry_bytes;
        return k_ra_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_err_no_mem;
}

/**
 * @brief Free an entire cluster chain starting at `start`.
 *
 * @details Walks the chain via `priv_fat_get`, marking each cluster
 *          free. A guard counter bounds the loop against on-disk loops.
 *
 * @param[in] m     Mount providing FAT access.
 * @param[in] start First cluster of the chain.
 *
 * @return Error code.
 * @retval k_ra_ok                 All clusters freed.
 * @retval k_ra_err_protocol_error Loop detected in chain.
 * @retval k_ra_err_*              Backend error.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `start` is a valid cluster number or sentinel.
 * @post On success, every cluster in the chain has FAT entry = 0.
 * @post On failure, FAT may be partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t priv_free_chain(const ra_fs_mount_t* m, uint32_t start)
{
  uint32_t cur   = start;
  uint32_t guard = 0;
  while (cur >= k_cluster_first_data && (cur - k_cluster_first_data) < m->count_of_clusters) {
    uint32_t next = 0;
    ra_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra_ok) {
      return err;
    }
    err = priv_fat_set(m, cur, k_cluster_free);
    if (err != k_ra_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      break;
    }
    cur = next;
    /* Bounded loop -- can't visit more clusters than exist. */
    guard++;
    if (guard > m->count_of_clusters) {
      return k_ra_err_protocol_error;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * Slot allocation
 * =============================================================================
 */

/**
 * @brief Allocate a free entry from the mount table; returns NULL if full.
 *
 * @details Linear scan of `s_mounts` for an entry with `in_use == 0`.
 *
 * @return Pointer to a free mount slot, or NULL if all are busy.
 * @retval non-NULL Pointer to a `ra_fs_mount_t` with `in_use == 0`.
 * @retval NULL     Mount table is full.
 *
 * @pre Module is initialised.
 * @pre Caller serialises mount/unmount operations.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
static ra_fs_mount_t* priv_alloc_mount_slot(void)
{
  for (uint32_t i = 0; i < k_ra_fs_max_mounts; i++) {
    if (s_mounts[i].in_use == 0U) {
      return &s_mounts[i];
    }
  }
  return NULL;
}

/**
 * @brief Allocate a free entry from the file table; returns NULL if full.
 *
 * @details Linear scan of `s_files` for an entry with `in_use == 0`.
 *
 * @return Pointer to a free file slot, or NULL if all are busy.
 * @retval non-NULL Pointer to a `ra_fs_file_t` with `in_use == 0`.
 * @retval NULL     File table is full.
 *
 * @pre Module is initialised.
 * @pre Caller serialises open/close operations.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
static ra_fs_file_t* priv_alloc_file_slot(void)
{
  for (uint32_t i = 0; i < k_ra_fs_max_files; i++) {
    if (s_files[i].in_use == 0U) {
      return &s_files[i];
    }
  }
  return NULL;
}

/* =============================================================================
 * Public API: mount / unmount
 * =============================================================================
 */

/**
 * @brief Parse the BPB layout fields out of `s_scratch` into `m`.
 *
 * @details Validates the boot signature (0x55AA) and reads the BPB
 *          fields out of the boot sector scratch buffer.
 *
 * @param[in,out] m Mount to populate; backend already plugged in.
 *
 * @return Error code.
 * @retval k_ra_ok                     Fields parsed successfully.
 * @retval k_ra_err_validation_failed  Bad signature or sanity-check fail.
 *
 * @pre `m` is non-NULL.
 * @pre `s_scratch` holds the boot sector (LBA 0).
 * @post On success, the relevant `m->*` fields are populated.
 * @post On failure, `m` may be partially updated.
 *
 * @note Not thread-safe -- uses module-level scratch.
 *
 * @since 0.1.0
 */
static ra_err_t priv_parse_bpb_into_mount(ra_fs_mount_t* m)
{
  if (s_scratch[k_bpb_off_signature_lo] != k_bpb_sig_lo ||
      s_scratch[k_bpb_off_signature_hi] != k_bpb_sig_hi) {
    return k_ra_err_validation_failed;
  }
  m->bytes_per_sector    = priv_rd16(&s_scratch[k_bpb_off_bytes_per_sec]);
  m->sectors_per_cluster = (uint32_t)s_scratch[k_bpb_off_sec_per_clus];
  m->reserved_sectors    = priv_rd16(&s_scratch[k_bpb_off_rsvd_sec_cnt]);
  m->num_fats            = (uint32_t)s_scratch[k_bpb_off_num_fats];
  m->root_entries        = priv_rd16(&s_scratch[k_bpb_off_root_ent_cnt]);
  if (m->bytes_per_sector != k_ra_fs_bytes_per_sector || m->sectors_per_cluster == 0U ||
      m->num_fats == 0U) {
    return k_ra_err_validation_failed;
  }
  const uint32_t fat_sz_16  = priv_rd16(&s_scratch[k_bpb_off_fat_sz_16]);
  const uint32_t fat_sz_32  = priv_rd32(&s_scratch[k_bpb_off_fat_sz_32]);
  const uint32_t tot_sec_16 = priv_rd16(&s_scratch[k_bpb_off_tot_sec_16]);
  const uint32_t tot_sec_32 = priv_rd32(&s_scratch[k_bpb_off_tot_sec_32]);
  m->fat_size_sectors       = (fat_sz_16 != 0U) ? fat_sz_16 : fat_sz_32;
  m->total_sectors          = (tot_sec_16 != 0U) ? tot_sec_16 : tot_sec_32;
  m->root_cluster           = priv_rd32(&s_scratch[k_bpb_off_root_clus]);
  return k_ra_ok;
}

/**
 * @brief Compute first_fat_lba, first_root_lba, first_data_lba, count_of_clusters.
 *
 * @details Derives region LBAs from the BPB and chooses FAT type using
 *          the cluster-count thresholds in MS FAT spec sec 3.5.
 *
 * @param[in,out] m Mount with BPB fields already populated.
 *
 * @return Error code.
 * @retval k_ra_ok                     Geometry computed.
 * @retval k_ra_err_validation_failed  Total sectors smaller than data start.
 *
 * @pre `m` is non-NULL.
 * @pre `priv_parse_bpb_into_mount` has populated the BPB-derived fields.
 * @post On success, geometry fields and `m->type` are valid.
 * @post On failure, `m` is left in an inconsistent state.
 *
 * @note Pure computation; thread-safe vs other readers.
 *
 * @since 0.1.0
 */
static ra_err_t priv_compute_geometry(ra_fs_mount_t* m)
{
  m->first_fat_lba = m->reserved_sectors;
  const uint32_t root_dir_sectors =
    ((m->root_entries * k_ra_fs_dir_entry_bytes) + (k_ra_fs_bytes_per_sector - 1U)) /
    k_ra_fs_bytes_per_sector;
  m->first_root_lba = m->first_fat_lba + (m->num_fats * m->fat_size_sectors);
  m->first_data_lba = m->first_root_lba + root_dir_sectors;
  if (m->total_sectors < m->first_data_lba) {
    return k_ra_err_validation_failed;
  }
  const uint32_t data_sectors = m->total_sectors - m->first_data_lba;
  m->count_of_clusters        = data_sectors / m->sectors_per_cluster;
  if (m->count_of_clusters < k_cluster_count_fat12_max) {
    m->type = k_ra_fs_type_fat12;
  } else if (m->count_of_clusters < k_cluster_count_fat16_max) {
    m->type = k_ra_fs_type_fat16;
  } else {
    m->type = k_ra_fs_type_fat32;
  }
  return k_ra_ok;
}

/**
 * @brief Mount a FAT volume on the supplied block backend.
 *
 * @details Allocates a mount slot, reads the boot sector, parses the
 *          BPB, and computes the geometry.
 *
 * @param[in]  backend    Block-device backend to drive.
 * @param[out] out_handle On success, opaque mount handle.
 *
 * @return Error code.
 * @retval k_ra_ok                     Volume mounted.
 * @retval k_ra_err_null_ptr           NULL `backend` or `out_handle`.
 * @retval k_ra_err_invalid_arg        Backend missing required callbacks.
 * @retval k_ra_err_no_mem             Mount table is full.
 * @retval k_ra_err_validation_failed  Not a recognisable FAT volume.
 * @retval k_ra_err_*                  Backend read failure.
 *
 * @pre `backend` and `out_handle` are non-NULL.
 * @pre Backend's read/write/get_capacity callbacks are non-NULL.
 * @post On success, `*out_handle` is a valid mount.
 * @post On failure, no mount slot is marked in-use.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_mount(const ra_fs_backend_t* backend, ra_fs_mount_t** out_handle)
{
  if (backend == NULL || out_handle == NULL) {
    return k_ra_err_null_ptr;
  }
  if (backend->read_block == NULL || backend->write_block == NULL ||
      backend->get_capacity == NULL) {
    return k_ra_err_invalid_arg;
  }
  ra_fs_mount_t* m = priv_alloc_mount_slot();
  if (m == NULL) {
    return k_ra_err_no_mem;
  }
  m->backend   = *backend;
  ra_err_t err = priv_read_sector(m, 0, s_scratch);
  if (err != k_ra_ok) {
    return err;
  }
  err = priv_parse_bpb_into_mount(m);
  if (err != k_ra_ok) {
    return err;
  }
  err = priv_compute_geometry(m);
  if (err != k_ra_ok) {
    return err;
  }
  m->in_use   = 1;
  *out_handle = m;
  return k_ra_ok;
}

/**
 * @brief Release a previously mounted FAT volume.
 *
 * @details Marks the mount slot free; does not flush -- callers must
 *          close all files first.
 *
 * @param[in] handle Mount handle from `ra_fs_mount()`.
 *
 * @return Error code.
 * @retval k_ra_ok                Volume unmounted.
 * @retval k_ra_err_null_ptr      `handle` was NULL.
 * @retval k_ra_err_invalid_state `handle` is not currently mounted.
 *
 * @pre `handle` is non-NULL and currently in use.
 * @pre All files opened on this mount have been closed.
 * @post Mount slot is free for reuse.
 * @post `handle->type` is reset to `k_ra_fs_type_unknown`.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_unmount(ra_fs_mount_t* handle)
{
  if (handle == NULL) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  handle->in_use = 0;
  handle->type   = k_ra_fs_type_unknown;
  return k_ra_ok;
}

/* =============================================================================
 * Public API: open / close
 * =============================================================================
 */

/**
 * @brief Read the first cluster from a 32-byte directory entry.
 *
 * @details Combines the high and low cluster halves into a single
 *          32-bit value (FAT32 layout; high half is 0 on FAT12/16).
 *
 * @param[in] entry 32-byte directory entry.
 *
 * @return First cluster of the file.
 * @retval 0..UINT32_MAX  Cluster number.
 *
 * @pre `entry` is non-NULL and points to 32 readable bytes.
 * @pre Caller has already filtered LFN / deleted entries.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
static uint32_t priv_entry_first_cluster(const uint8_t* entry)
{
  const uint32_t hi = priv_rd16(&entry[k_dir_off_fst_clus_hi]);
  const uint32_t lo = priv_rd16(&entry[k_dir_off_fst_clus_lo]);
  return (hi << k_shift_two_bytes) | lo;
}

/**
 * @brief Patch first-cluster + size back into a 32-byte directory entry.
 *
 * @details Inverse of `priv_entry_first_cluster`; also writes file size.
 *
 * @param[in,out] entry   32-byte directory entry to update.
 * @param[in]     cluster New first cluster.
 * @param[in]     size    New file size in bytes.
 *
 * @return None.
 * @retval None
 *
 * @pre `entry` is non-NULL and points to 32 writable bytes.
 * @pre Caller has staged the entry in a sector buffer that will be
 *      written back to disk.
 * @post `entry` reflects the new first-cluster and size fields.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
static void priv_entry_set_cluster_size(uint8_t* entry, uint32_t cluster, uint32_t size)
{
  priv_wr16(&entry[k_dir_off_fst_clus_hi],
            (uint16_t)((cluster >> k_shift_two_bytes) & k_word_mask));
  priv_wr16(&entry[k_dir_off_fst_clus_lo], (uint16_t)(cluster & k_word_mask));
  priv_wr32(&entry[k_dir_off_file_size], size);
}

/**
 * @brief Truncate an existing file's chain and zero its dir-entry size.
 *
 * @details Frees the cluster chain, resets the in-memory file state,
 *          then writes a fresh dir entry with cluster=0 and size=0.
 *
 * @param[in,out] handle Mount providing FAT access.
 * @param[in,out] f      File state to reset.
 * @param[in]     lba    Sector LBA holding the directory entry.
 * @param[in]     off    Byte offset of the entry within the sector.
 *
 * @return Error code.
 * @retval k_ra_ok    File truncated successfully.
 * @retval k_ra_err_* Backend or FAT error.
 *
 * @pre All pointers are non-NULL; mount/file are in use.
 * @pre `lba`/`off` identify the file's directory entry.
 * @post On success, the file occupies zero clusters and its size is 0.
 * @post On failure, on-disk state may be partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t
priv_truncate_existing(ra_fs_mount_t* handle, ra_fs_file_t* f, uint32_t lba, uint32_t off)
{
  if (f->first_cluster >= k_cluster_first_data) {
    ra_err_t err = priv_free_chain(handle, f->first_cluster);
    if (err != k_ra_ok) {
      return err;
    }
  }
  f->first_cluster                       = 0;
  f->cur_cluster                         = 0;
  f->size_bytes                          = 0;
  f->offset                              = 0;
  uint8_t  buf[k_ra_fs_bytes_per_sector] = {};
  ra_err_t err                           = priv_read_sector(handle, lba, buf);
  if (err != k_ra_ok) {
    return err;
  }
  priv_entry_set_cluster_size(&buf[off], 0, 0);
  return priv_write_sector(handle, lba, buf);
}

/**
 * @brief Populate a fresh file handle from an existing on-disk dir entry.
 *
 * @details Allocates a file slot, copies the entry's first cluster /
 *          size into it, sets the requested mode, and applies
 *          truncate/append behaviour for write/append modes.
 *
 * @param[in,out] handle   Mount on which the file lives.
 * @param[in]     entry    32-byte directory entry already on disk.
 * @param[in]     lba      Sector LBA holding the directory entry.
 * @param[in]     off      Byte offset of the entry within the sector.
 * @param[in]     mode     Open mode (read / write / append).
 * @param[out]    out_file Receives the populated file handle.
 *
 * @return Error code.
 * @retval k_ra_ok          File handle ready.
 * @retval k_ra_err_no_mem  File table is full.
 * @retval k_ra_err_*       Backend error during truncation.
 *
 * @pre All pointers are non-NULL; mount is in use.
 * @pre `entry` came from a successful `priv_dir_find` for `lba`/`off`.
 * @post On success, `*out_file` is in use and configured for `mode`.
 * @post On failure, the file slot is marked free again.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
static ra_err_t priv_open_existing(ra_fs_mount_t* handle,
                                   const uint8_t* entry,
                                   uint32_t       lba,
                                   uint32_t       off,
                                   ra_fs_mode_t   mode,
                                   ra_fs_file_t** out_file)
{
  ra_fs_file_t* f = priv_alloc_file_slot();
  if (f == NULL) {
    return k_ra_err_no_mem;
  }
  f->mount         = handle;
  f->first_cluster = priv_entry_first_cluster(entry);
  f->cur_cluster   = f->first_cluster;
  f->size_bytes    = priv_rd32(&entry[k_dir_off_file_size]);
  f->dir_entry_lba = lba;
  f->dir_entry_idx = off;
  f->mode          = mode;
  f->in_use        = 1;
  if (mode == k_ra_fs_mode_write) {
    ra_err_t err = priv_truncate_existing(handle, f, lba, off);
    if (err != k_ra_ok) {
      f->in_use = 0;
      return err;
    }
  } else if (mode == k_ra_fs_mode_append) {
    f->offset = f->size_bytes;
  } else {
    f->offset = 0;
  }
  *out_file = f;
  return k_ra_ok;
}

/**
 * @brief Carve a fresh dir entry for `name83` and populate a file handle.
 *
 * @details Locates a free directory slot, writes the 8.3 name plus an
 *          archive attribute, and returns a file handle pointing at
 *          an empty file with no allocated clusters.
 *
 * @param[in,out] handle   Mount on which to create the file.
 * @param[in]     name83   Packed 11-byte 8.3 short name.
 * @param[in]     mode     Open mode used to record into the handle.
 * @param[out]    out_file Receives the populated file handle.
 *
 * @return Error code.
 * @retval k_ra_ok          New file created and opened.
 * @retval k_ra_err_no_mem  No free directory slot or file table full.
 * @retval k_ra_err_*       Backend error.
 *
 * @pre All pointers are non-NULL; mount is in use.
 * @pre `name83` is already validated by `priv_path_to_83`.
 * @post On success, `*out_file` is in use and the dir entry is on disk.
 * @post On failure, no dir entry is written.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
static ra_err_t priv_create_new(ra_fs_mount_t* handle,
                                const uint8_t* name83,
                                ra_fs_mode_t   mode,
                                ra_fs_file_t** out_file)
{
  uint32_t free_lba = 0;
  uint32_t free_off = 0;
  ra_err_t err      = priv_dir_find_free(handle, &free_lba, &free_off);
  if (err != k_ra_ok) {
    return err;
  }
  ra_fs_file_t* f = priv_alloc_file_slot();
  if (f == NULL) {
    return k_ra_err_no_mem;
  }
  uint8_t buf[k_ra_fs_bytes_per_sector] = {};
  err                                   = priv_read_sector(handle, free_lba, buf);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t* ent = &buf[free_off];
  for (uint32_t i = 0; i < (uint32_t)k_ra_fs_dir_entry_bytes; i++) {
    ent[i] = 0;
  }
  priv_byte_copy(&ent[k_dir_off_name], name83, k_dir_name_field_len);
  ent[k_dir_off_attr] = k_ra_fs_attr_archive;
  err                 = priv_write_sector(handle, free_lba, buf);
  if (err != k_ra_ok) {
    return err;
  }
  f->mount         = handle;
  f->first_cluster = 0;
  f->cur_cluster   = 0;
  f->size_bytes    = 0;
  f->offset        = 0;
  f->dir_entry_lba = free_lba;
  f->dir_entry_idx = free_off;
  f->mode          = mode;
  f->in_use        = 1;
  *out_file        = f;
  return k_ra_ok;
}

/**
 * @brief Open a file by path on a mounted FAT volume.
 *
 * @details Resolves the path to an 8.3 name, searches the root
 *          directory, and either opens an existing entry or creates a
 *          new one (write/append modes).
 *
 * @param[in]  handle   Mount handle.
 * @param[in]  path     NUL-terminated path; only flat root names supported.
 * @param[in]  mode     Open mode.
 * @param[out] out_file Receives the open file handle.
 *
 * @return Error code.
 * @retval k_ra_ok                File opened.
 * @retval k_ra_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra_err_invalid_state Mount is not currently in use.
 * @retval k_ra_err_invalid_arg   Path is not a valid 8.3 name.
 * @retval k_ra_err_not_found     Read-only open of a missing file.
 * @retval k_ra_err_no_mem        File or directory table full.
 * @retval k_ra_err_*             Backend error.
 *
 * @pre `handle`, `path`, and `out_file` are non-NULL.
 * @pre Mount is in use.
 * @post On success, `*out_file` is a valid open handle.
 * @post On failure, no file slot is marked in use.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t
ra_fs_open(ra_fs_mount_t* handle, const char* path, ra_fs_mode_t mode, ra_fs_file_t** out_file)
{
  if (handle == NULL || path == NULL || out_file == NULL) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  uint8_t name83[k_max_8_3_name] = {};
  if (priv_path_to_83(path, name83) == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t lba                            = 0;
  uint32_t off                            = 0;
  uint8_t  entry[k_ra_fs_dir_entry_bytes] = {};
  ra_err_t err                            = priv_dir_find(handle, name83, &lba, &off, entry);
  if (err == k_ra_ok) {
    return priv_open_existing(handle, entry, lba, off, mode, out_file);
  }
  if (err != k_ra_err_not_found) {
    return err;
  }
  if (mode == k_ra_fs_mode_read) {
    return k_ra_err_not_found;
  }
  return priv_create_new(handle, name83, mode, out_file);
}

/**
 * @brief Close an open file handle.
 *
 * @details Marks the slot free; the driver does not buffer writes so
 *          there is nothing to flush.
 *
 * @param[in] file Handle from `ra_fs_open()`.
 *
 * @return Error code.
 * @retval k_ra_ok           File closed.
 * @retval k_ra_err_null_ptr `file` was NULL.
 *
 * @pre `file` is non-NULL.
 * @pre All pending writes have already been issued.
 * @post File slot is marked free for reuse.
 * @post `file->mount` is reset to NULL.
 *
 * @note Idempotent on a freshly-closed handle.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_close(ra_fs_file_t* file)
{
  if (file == NULL) {
    return k_ra_err_null_ptr;
  }
  file->in_use = 0;
  file->mount  = NULL;
  return k_ra_ok;
}

/* =============================================================================
 * Public API: read / write / seek / tell / size
 * =============================================================================
 */

/**
 * @brief Walk `n` clusters forward from `start` along the FAT chain.
 */
static ra_err_t
priv_skip_clusters(const ra_fs_mount_t* m, uint32_t start, uint32_t n, uint32_t* out)
{
  uint32_t cur = start;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t next = 0;
    ra_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      *out = cur;
      return k_ra_err_invalid_state;
    }
    cur = next;
  }
  *out = cur;
  return k_ra_ok;
}

/**
 * @brief Read up to one sector's worth of bytes at the file's current offset.
 */
static ra_err_t
priv_read_one_chunk(ra_fs_file_t* file, uint8_t* buf, uint32_t remaining, uint32_t* out_take)
{
  const uint32_t cluster_bytes   = file->mount->sectors_per_cluster * k_ra_fs_bytes_per_sector;
  const uint32_t cluster_idx_now = file->offset / cluster_bytes;
  uint32_t       target          = 0;
  ra_err_t err = priv_skip_clusters(file->mount, file->first_cluster, cluster_idx_now, &target);
  if (err != k_ra_ok) {
    return err;
  }
  file->cur_cluster                = target;
  const uint32_t off_in_cluster    = file->offset % cluster_bytes;
  const uint32_t sector_in_cluster = off_in_cluster / k_ra_fs_bytes_per_sector;
  const uint32_t off_in_sector     = off_in_cluster % k_ra_fs_bytes_per_sector;
  const uint32_t lba = priv_cluster_to_lba(file->mount, file->cur_cluster) + sector_in_cluster;
  uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
  err                                          = priv_read_sector(file->mount, lba, sec);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t take = k_ra_fs_bytes_per_sector - off_in_sector;
  if (take > remaining) {
    take = remaining;
  }
  priv_byte_copy(buf, &sec[off_in_sector], take);
  *out_take = take;
  return k_ra_ok;
}

ra_err_t ra_fs_read(ra_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  if (file == NULL || buf == NULL || got_len == NULL) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  *got_len = 0;
  if (file->offset >= file->size_bytes || max_len == 0U) {
    return k_ra_ok;
  }
  uint32_t remaining = file->size_bytes - file->offset;
  if (remaining > max_len) {
    remaining = max_len;
  }
  uint32_t produced = 0;
  while (remaining > 0U) {
    uint32_t take = 0;
    ra_err_t err  = priv_read_one_chunk(file, &buf[produced], remaining, &take);
    if (err != k_ra_ok) {
      return err;
    }
    produced += take;
    file->offset += take;
    remaining -= take;
  }
  *got_len = produced;
  return k_ra_ok;
}

/**
 * @brief Allocate a fresh cluster, mark it EOC, and return its number.
 */
static ra_err_t priv_alloc_eoc_cluster(const ra_fs_mount_t* m, uint32_t* out_c)
{
  ra_err_t err = priv_alloc_cluster(m, out_c);
  if (err != k_ra_ok) {
    return err;
  }
  return priv_fat_set(m, *out_c, priv_eoc_write(m));
}

/**
 * @brief Walk to cluster index `idx` from `start`, growing the chain as needed.
 */
static ra_err_t
priv_walk_grow(const ra_fs_mount_t* m, uint32_t start, uint32_t idx, uint32_t* out_cluster)
{
  uint32_t cur = start;
  for (uint32_t i = 0; i < idx; i++) {
    uint32_t next = 0;
    ra_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      uint32_t newc = 0;
      err           = priv_alloc_eoc_cluster(m, &newc);
      if (err != k_ra_ok) {
        return err;
      }
      err = priv_fat_set(m, cur, newc);
      if (err != k_ra_ok) {
        return err;
      }
      next = newc;
    }
    cur = next;
  }
  *out_cluster = cur;
  return k_ra_ok;
}

/**
 * @brief Write `put` bytes into one sector at `lba` starting at `off_in_sector`.
 */
static ra_err_t priv_write_into_sector(const ra_fs_mount_t* m,
                                       uint32_t             lba,
                                       uint32_t             off_in_sector,
                                       const uint8_t*       src,
                                       uint32_t             put)
{
  uint8_t  sec[k_ra_fs_bytes_per_sector] = {};
  ra_err_t err                           = priv_read_sector(m, lba, sec);
  if (err != k_ra_ok) {
    return err;
  }
  priv_byte_copy(&sec[off_in_sector], src, put);
  return priv_write_sector(m, lba, sec);
}

/**
 * @brief Inner write loop: stream `len` bytes from `buf` into the file's chain.
 */
static ra_err_t priv_write_stream(ra_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  ra_fs_mount_t* m             = file->mount;
  const uint32_t cluster_bytes = m->sectors_per_cluster * k_ra_fs_bytes_per_sector;
  uint32_t       consumed      = 0;
  while (consumed < len) {
    if (file->first_cluster < k_cluster_first_data) {
      uint32_t c   = 0;
      ra_err_t err = priv_alloc_eoc_cluster(m, &c);
      if (err != k_ra_ok) {
        return err;
      }
      file->first_cluster = c;
      file->cur_cluster   = c;
    }
    const uint32_t cluster_idx_now = file->offset / cluster_bytes;
    uint32_t       cur             = 0;
    ra_err_t       err             = priv_walk_grow(m, file->first_cluster, cluster_idx_now, &cur);
    if (err != k_ra_ok) {
      return err;
    }
    file->cur_cluster                = cur;
    const uint32_t off_in_cluster    = file->offset % cluster_bytes;
    const uint32_t sector_in_cluster = off_in_cluster / k_ra_fs_bytes_per_sector;
    const uint32_t off_in_sector     = off_in_cluster % k_ra_fs_bytes_per_sector;
    const uint32_t lba = priv_cluster_to_lba(m, file->cur_cluster) + sector_in_cluster;
    uint32_t       put = k_ra_fs_bytes_per_sector - off_in_sector;
    if (put > (len - consumed)) {
      put = len - consumed;
    }
    err = priv_write_into_sector(m, lba, off_in_sector, &buf[consumed], put);
    if (err != k_ra_ok) {
      return err;
    }
    consumed += put;
    file->offset += put;
    if (file->offset > file->size_bytes) {
      file->size_bytes = file->offset;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_fs_write(ra_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  if (file == NULL || buf == NULL) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U || file->mode == k_ra_fs_mode_read) {
    return k_ra_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra_ok;
  }
  ra_err_t err = priv_write_stream(file, buf, len);
  if (err != k_ra_ok) {
    return err;
  }
  ra_fs_mount_t* m                                = file->mount;
  uint8_t        dirsec[k_ra_fs_bytes_per_sector] = {};
  err = priv_read_sector(m, file->dir_entry_lba, dirsec);
  if (err != k_ra_ok) {
    return err;
  }
  priv_entry_set_cluster_size(&dirsec[file->dir_entry_idx], file->first_cluster, file->size_bytes);
  return priv_write_sector(m, file->dir_entry_lba, dirsec);
}

ra_err_t ra_fs_seek(ra_fs_file_t* file, uint32_t offset_bytes)
{
  if (file == NULL) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  uint32_t target = offset_bytes;
  if (target > file->size_bytes) {
    target = file->size_bytes;
  }
  file->offset = target;
  return k_ra_ok;
}

ra_err_t ra_fs_tell(const ra_fs_file_t* file, uint32_t* out_offset)
{
  if (file == NULL || out_offset == NULL) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_offset = file->offset;
  return k_ra_ok;
}

ra_err_t ra_fs_size(const ra_fs_file_t* file, uint32_t* out_bytes)
{
  if (file == NULL || out_bytes == NULL) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_bytes = file->size_bytes;
  return k_ra_ok;
}

/* =============================================================================
 * Public API: listdir / unlink
 * =============================================================================
 */

/**
 * @brief Visit every visible entry in one already-loaded directory sector.
 *
 * @return 1 if end-of-directory marker hit (caller can stop), 0 otherwise.
 */
static uint8_t priv_listdir_visit_sector(const uint8_t* buf, ra_fs_listdir_cb_t cb, void* ctx)
{
  for (uint32_t e = 0; e < (uint32_t)k_dir_entries_per_sector; e++) {
    const uint8_t* ent = &buf[(size_t)e * (size_t)k_ra_fs_dir_entry_bytes];
    if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
      return 1U;
    }
    if (ent[k_dir_off_name] == k_dir_marker_free_used) {
      continue;
    }
    if (ent[k_dir_off_attr] == k_ra_fs_attr_lfn) {
      continue;
    }
    char name[k_ra_fs_short_name_len] = {};
    priv_83_to_str(&ent[k_dir_off_name], name);
    const uint32_t size = priv_rd32(&ent[k_dir_off_file_size]);
    cb(name, ent[k_dir_off_attr], size, ctx);
  }
  return 0U;
}

ra_err_t ra_fs_listdir(ra_fs_mount_t* handle, const char* path, ra_fs_listdir_cb_t cb, void* ctx)
{
  if (handle == NULL || cb == NULL || path == NULL) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  /* cppcheck-suppress redundantCondition -- explicit OR-chain documents intent. */
  if (path[0] != '/' || (path[0] == '/' && path[1] != '\0')) {
    return k_ra_err_not_supported;
  }
  dir_walk_t w = {};
  priv_dir_walk_init_root(handle, &w);
  uint8_t eod                           = 0;
  uint8_t buf[k_ra_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra_err_t err = priv_read_sector(handle, w.cur_lba, buf);
    if (err != k_ra_ok) {
      return err;
    }
    if (priv_listdir_visit_sector(buf, cb, ctx) != 0U) {
      return k_ra_ok;
    }
    err = priv_dir_walk_next_sector(handle, &w, &eod);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_fs_unlink(ra_fs_mount_t* handle, const char* path)
{
  if (handle == NULL || path == NULL) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  uint8_t name83[k_max_8_3_name] = {};
  if (priv_path_to_83(path, name83) == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t lba                            = 0;
  uint32_t off                            = 0;
  uint8_t  entry[k_ra_fs_dir_entry_bytes] = {};
  ra_err_t err                            = priv_dir_find(handle, name83, &lba, &off, entry);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t first_cluster = priv_entry_first_cluster(entry);
  if (first_cluster >= k_cluster_first_data) {
    err = priv_free_chain(handle, first_cluster);
    if (err != k_ra_ok) {
      return err;
    }
  }
  uint8_t buf[k_ra_fs_bytes_per_sector] = {};
  err                                   = priv_read_sector(handle, lba, buf);
  if (err != k_ra_ok) {
    return err;
  }
  buf[off + k_dir_off_name] = k_dir_marker_free_used;
  return priv_write_sector(handle, lba, buf);
}
