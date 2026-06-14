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
 * @enum ra_fs_mbr_off_t
 * @brief Byte offsets inside an MBR partition table (when LBA 0 is not a BPB).
 *
 * @details A standard SD card is MBR-partitioned: LBA 0 holds a partition
 * table (four 16-byte entries starting at 0x1BE) and the FAT boot sector
 * lives at the partition's first LBA. Only partition 0 is consulted.
 */
typedef enum : uint16_t {
  k_mbr_off_part0_type = 0x1C2U, /**< Partition 0 type byte (0 = unused).      */
  k_mbr_off_part0_lba  = 0x1C6U, /**< Partition 0 first-LBA (4 bytes, LE).     */
} ra_fs_mbr_off_t;

/**
 * @enum ra_fs_gpt_t
 * @brief GPT on-disk constants (UEFI spec 2.10 ch 5 "GUID Partition Table").
 *
 * @details A GPT disk carries a protective MBR (partition 0 type 0xEE), the
 * GPT header at LBA 1 ("EFI PART"), and a partition entry array (128-byte
 * entries). FAT/exFAT data volumes live in entries whose type GUID is
 * Microsoft Basic Data; macOS additionally creates an EFI System Partition
 * first, which must be skipped to reach the user volume.
 */
typedef enum : uint16_t {
  k_gpt_part_type_protective = 0xEEU, /**< MBR type byte for a GPT disk.       */
  k_gpt_header_lba           = 1U,    /**< GPT header location.                */
  k_gpt_sig_len              = 8U,    /**< "EFI PART" signature length.        */
  k_gpt_off_entry_lba        = 0x48U, /**< Entry-array first LBA (8 bytes LE). */
  k_gpt_off_entry_count      = 0x50U, /**< Number of partition entries.        */
  k_gpt_off_entry_size       = 0x54U, /**< Bytes per partition entry.          */
  k_gpt_entry_bytes          = 128U,  /**< Standard entry size supported.      */
  k_gpt_entries_per_sector   = 4U,    /**< 512 / 128 entries per sector.       */
  k_gpt_entry_scan_max       = 128U,  /**< Bounded entry walk (UEFI minimum).  */
  k_gpt_entry_off_first_lba  = 0x20U, /**< Entry: first LBA (8 bytes LE).      */
  k_gpt_guid_len             = 16U,   /**< Type-GUID length in an entry.       */
  k_gpt_u64_hi_off           = 4U,    /**< High-word offset inside a u64.      */
} ra_fs_gpt_t;

/**
 * @enum ra_fs_exfat_off_t
 * @brief Byte offsets in the exFAT boot sector (VBR) and directory entries.
 *
 * @details exFAT (Microsoft exFAT spec) replaces the FAT BPB with a Volume
 * Boot Record carrying sector-relative region offsets and log2 geometry. The
 * directory uses 32-byte typed entry sets (File 0x85 + Stream 0xC0 + Name
 * 0xC1) with UTF-16LE names. Read-only support; fields are little-endian.
 */
typedef enum : uint16_t {
  k_exfat_off_fsname     = 3,    /**< "EXFAT   " filesystem-name field (8 chars). */
  k_exfat_off_fat_lba    = 0x50, /**< FatOffset (sectors from VBR).             */
  k_exfat_off_fat_len    = 0x54, /**< FatLength (sectors).                      */
  k_exfat_off_heap_lba   = 0x58, /**< ClusterHeapOffset (sectors from VBR).     */
  k_exfat_off_clus_count = 0x5C, /**< ClusterCount.                             */
  k_exfat_off_root_clus  = 0x60, /**< FirstClusterOfRootDirectory.              */
  k_exfat_off_bps_shift  = 0x6C, /**< BytesPerSectorShift (log2).               */
  k_exfat_off_spc_shift  = 0x6D, /**< SectorsPerClusterShift (log2).            */
  k_exfat_off_num_fats   = 0x6E, /**< NumberOfFats.                             */
  k_exfat_strm_off_flags = 1,    /**< Stream-ext GeneralSecondaryFlags.         */
  k_exfat_strm_off_nlen  = 3,    /**< Stream-ext NameLength (UTF-16 units).     */
  k_exfat_strm_off_clus  = 0x14, /**< Stream-ext FirstCluster.                  */
  k_exfat_strm_off_dlen  = 0x18, /**< Stream-ext DataLength (low 32 used).      */
  k_exfat_name_off       = 2,    /**< File-name entry character offset.         */
} ra_fs_exfat_off_t;

/**
 * @enum ra_fs_exfat_val_t
 * @brief exFAT entry-type tags + small magic values used by the reader.
 */
typedef enum : uint32_t {
  k_exfat_entry_eod      = 0x00U,   /**< End-of-directory marker.              */
  k_exfat_entry_bitmap   = 0x81U,   /**< Allocation-bitmap directory entry.    */
  k_exfat_entry_file     = 0x85U,   /**< File directory entry.                 */
  k_exfat_entry_stream   = 0xC0U,   /**< Stream-extension entry.               */
  k_exfat_entry_name     = 0xC1U,   /**< File-name entry.                      */
  k_exfat_secflag_no_fat = 0x02U,   /**< GeneralSecondaryFlags: NoFatChain.    */
  k_exfat_secflag_alloc  = 0x03U,   /**< AllocationPossible | NoFatChain.      */
  k_exfat_attr_archive   = 0x20U,   /**< FileAttributes: archive.              */
  k_exfat_fsname_len     = 8U,      /**< "EXFAT   " field length.              */
  k_exfat_name_per_entry = 15U,     /**< UTF-16 units per file-name entry.     */
  k_exfat_entry_bytes    = 32U,     /**< Directory entry size.                 */
  k_exfat_bps_shift_512  = 9U,      /**< log2(512) -- the only sector size we do. */
  k_exfat_scan_limit     = 65536U,  /**< Max dir entries scanned (P10 bound).  */
  k_exfat_name_cap       = 64U,     /**< Longest path name we compare.         */
  k_exfat_ascii_hi_mask  = 0xFF00U, /**< UTF-16 unit is non-ASCII if set.     */
  k_exfat_csum_hi_bit    = 0x8000U, /**< Wrap bit for the rotate-add checksum. */
  k_exfat_off_file_secnt = 1U,      /**< File entry: SecondaryCount.           */
  k_exfat_off_file_csum  = 2U,      /**< File entry: SetChecksum (2 bytes).    */
  k_exfat_off_file_attr  = 4U,      /**< File entry: FileAttributes (2 bytes). */
  k_exfat_off_strm_hash  = 4U,      /**< Stream entry: NameHash (2 bytes).     */
  k_exfat_off_strm_valid = 8U,      /**< Stream entry: ValidDataLength (8 B).  */
  k_exfat_bit_mask       = 7U,      /**< Bit index within a bitmap byte.       */
  k_exfat_bit_shift      = 3U,      /**< log2(8): cluster index -> byte.       */
  k_exfat_inuse_bit      = 0x80U,   /**< Directory entry type bit 7 = in use.  */
  k_exfat_max_set_bytes  = 224U,    /**< (2 + ceil(64/15)) * 32 = 7 entries.   */
} ra_fs_exfat_val_t;

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
  return m->backend.read_block(m->backend.ctx, lba + m->partition_base_lba, 1, buf);
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
  return m->backend.write_block(m->backend.ctx, lba + m->partition_base_lba, 1, buf);
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
  if (path == nullptr || out11 == nullptr) {
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
  /* mcdc-deactivated: 3-condition AND on Shift-JIS kanji-escape directory entry; only reachable from a kanji-named FAT image, none of which exist in the test corpus. */
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
 * @pre Walker has been initialized by `priv_dir_walk_init_root`.
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
  /* mcdc-deactivated: loop bound; `cur < k_cluster_first_data` only on a corrupt FAT chain (the first-cluster reservation 0/1 is enforced at allocation). */
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
 * @pre Module is initialized.
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
  return nullptr;
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
 * @pre Module is initialized.
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
  return nullptr;
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
/**
 * @brief Return partition 0's first LBA from an MBR in @p buf, or 0.
 *
 * @details Used only after the LBA-0 BPB parse fails: a standard SD card is
 * MBR-partitioned, so the FAT boot sector lives at partition 0's start LBA,
 * not at LBA 0. Returns 0 when @p buf is not a usable MBR (no 0x55AA, or an
 * unused partition 0), which leaves the caller's original error in force.
 *
 * @param[in] buf Sector-0 contents (>= 512 bytes).
 * @return Partition 0 first LBA, or 0 if not an MBR with a live partition 0.
 * @retval 0 ``buf`` is not a usable MBR (no 0x55AA or an unused partition 0).
 * @retval non-zero Partition 0's first LBA.
 * @pre @p buf is non-NULL and holds at least one sector.
 * @pre @p buf is the contents of LBA 0.
 * @post No state modified.
 * @post @p buf is unmodified.
 * @note Not thread-safe (reads caller-owned memory only).
 * @since 0.1.0
 */
static uint32_t priv_mbr_part0_lba(const uint8_t* buf)
{
  if (buf[k_bpb_off_signature_lo] != (uint8_t)k_bpb_sig_lo) {
    return 0U;
  }
  if (buf[k_bpb_off_signature_hi] != (uint8_t)k_bpb_sig_hi) {
    return 0U;
  }
  if (buf[k_mbr_off_part0_type] == 0U) {
    return 0U;
  }
  return priv_rd32(&buf[k_mbr_off_part0_lba]);
}

/** @brief GPT header signature ("EFI PART", UEFI spec 2.10 table 5.5). */
static const uint8_t k_gpt_signature[k_gpt_sig_len] = {
  0x45U,
  0x46U,
  0x49U,
  0x20U,
  0x50U,
  0x41U,
  0x52U,
  0x54U,
};

/** @brief Microsoft Basic Data type GUID, on-disk byte order
 *         (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7). */
static const uint8_t k_gpt_guid_basic_data[k_gpt_guid_len] = {
  0xA2U,
  0xA0U,
  0xD0U,
  0xEBU,
  0xE5U,
  0xB9U,
  0x33U,
  0x44U,
  0x87U,
  0xC0U,
  0x68U,
  0xB6U,
  0xB7U,
  0x26U,
  0x99U,
  0xC7U,
};

/**
 * @brief Extract a usable first-LBA from one GPT partition entry.
 *
 * @details An entry is usable when its type GUID is non-zero (the slot is
 * allocated) and its 64-bit first LBA fits in 32 bits (this backend
 * addresses 512-byte blocks with 32-bit LBAs, i.e. disks up to 2 TiB).
 *
 * @param[in] entry One 128-byte partition entry.
 * @return The entry's first LBA, or 0 when the entry is unusable.
 * @retval 0 Unused slot, zero first-LBA, or first-LBA above 32 bits.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p entry came from the GPT partition entry array.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint32_t priv_gpt_entry_first_lba(const uint8_t* entry)
{
  uint32_t nonzero = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != 0U) {
      nonzero = 1U;
    }
  }
  if (nonzero == 0U) {
    return 0U;
  }
  if (priv_rd32(&entry[(uint32_t)k_gpt_entry_off_first_lba + (uint32_t)k_gpt_u64_hi_off]) != 0U) {
    return 0U;
  }
  return priv_rd32(&entry[k_gpt_entry_off_first_lba]);
}

/**
 * @brief Test whether a GPT entry's type GUID is Microsoft Basic Data.
 *
 * @details Basic Data is where FAT/exFAT user volumes live on a GPT disk;
 * other common entries (EFI System Partition, Microsoft Reserved) do not
 * carry the volume this layer should mount.
 *
 * @param[in] entry One 128-byte partition entry.
 * @return 1 when the type GUID matches, else 0.
 * @retval 1 The entry is a Basic Data partition.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p entry came from the GPT partition entry array.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint32_t priv_gpt_entry_is_basic_data(const uint8_t* entry)
{
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != k_gpt_guid_basic_data[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Fold one GPT entry into the candidate base-LBA bookkeeping.
 *
 * @details Prefers the first Microsoft Basic Data entry (the conventional
 * home of FAT/exFAT volumes); the first allocated entry of any other type
 * is kept as a fallback. Unusable entries (see
 * ::priv_gpt_entry_first_lba) are ignored.
 *
 * @param[in]     entry     One 128-byte partition entry.
 * @param[in,out] basic_lba First Basic Data candidate (0 = none yet).
 * @param[in,out] any_lba   First allocated-entry candidate (0 = none yet).
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p basic_lba and @p any_lba are non-NULL.
 * @post Candidates are updated only from 0 (first match wins).
 * @post @p entry is unmodified.
 * @note Pure bookkeeping; no I/O.
 * @since 0.1.0
 */
static void priv_gpt_note_entry(const uint8_t* entry, uint32_t* basic_lba, uint32_t* any_lba)
{
  const uint32_t first = priv_gpt_entry_first_lba(entry);
  if (first == 0U) {
    return;
  }
  if (*any_lba == 0U) {
    *any_lba = first;
  }
  if (*basic_lba == 0U) {
    if (priv_gpt_entry_is_basic_data(entry) != 0U) {
      *basic_lba = first;
    }
  }
}

/**
 * @brief Read the GPT entry array sector-by-sector and pick a base LBA.
 *
 * @details Reads each entry sector once and feeds every 128-byte entry to
 * ::priv_gpt_note_entry; the Basic Data candidate wins over the first
 * allocated entry of any other type.
 *
 * @param[in,out] m         Mount whose backend supplies the sectors.
 * @param[in]     entry_lba First LBA of the partition entry array.
 * @param[in]     count     Number of entries to scan (already clamped).
 * @param[out]    out_base  Receives the chosen partition's first LBA.
 * @return Error code.
 * @retval k_ra_ok            A candidate partition was found.
 * @retval k_ra_err_not_found No allocated entry was usable.
 * @retval k_ra_err_*         Backend read failure.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra_ok @p out_base holds a non-zero LBA.
 * @post ::s_scratch holds the last entry sector read.
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
static ra_err_t
priv_gpt_scan_entries(ra_fs_mount_t* m, uint32_t entry_lba, uint32_t count, uint32_t* out_base)
{
  uint32_t basic_lba = 0U;
  uint32_t any_lba   = 0U;
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t sector = i / (uint32_t)k_gpt_entries_per_sector;
    const uint32_t offset = (i % (uint32_t)k_gpt_entries_per_sector) * (uint32_t)k_gpt_entry_bytes;
    if (offset == 0U) {
      const ra_err_t err = priv_read_sector(m, entry_lba + sector, s_scratch);
      if (err != k_ra_ok) {
        return err;
      }
    }
    priv_gpt_note_entry(&s_scratch[offset], &basic_lba, &any_lba);
  }
  if (basic_lba != 0U) {
    *out_base = basic_lba;
    return k_ra_ok;
  }
  if (any_lba != 0U) {
    *out_base = any_lba;
    return k_ra_ok;
  }
  return k_ra_err_not_found;
}

/**
 * @brief Locate the mountable partition on a GPT disk.
 *
 * @details Validates the "EFI PART" header at LBA 1, bounds-checks the
 * entry-array geometry (128-byte entries, 32-bit array LBA), then scans the
 * entries via ::priv_gpt_scan_entries.
 *
 * @param[in,out] m        Mount whose backend supplies the sectors.
 * @param[out]    out_base Receives the chosen partition's first LBA.
 * @return Error code.
 * @retval k_ra_ok                    @p out_base holds the volume base.
 * @retval k_ra_err_validation_failed No "EFI PART" header at LBA 1.
 * @retval k_ra_err_not_supported     Non-standard entry size or array LBA.
 * @retval k_ra_err_*                 Backend read failure or no entry found.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra_ok @p out_base holds a non-zero LBA.
 * @post ::s_scratch is overwritten (callers must re-read their sector).
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
static ra_err_t priv_gpt_locate_volume(ra_fs_mount_t* m, uint32_t* out_base)
{
  ra_err_t err = priv_read_sector(m, (uint32_t)k_gpt_header_lba, s_scratch);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_sig_len; i++) {
    if (s_scratch[i] != k_gpt_signature[i]) {
      return k_ra_err_validation_failed;
    }
  }
  if (priv_rd32(&s_scratch[(uint32_t)k_gpt_off_entry_lba + (uint32_t)k_gpt_u64_hi_off]) != 0U) {
    return k_ra_err_not_supported;
  }
  const uint32_t entry_lba  = priv_rd32(&s_scratch[k_gpt_off_entry_lba]);
  const uint32_t entry_size = priv_rd32(&s_scratch[k_gpt_off_entry_size]);
  uint32_t       count      = priv_rd32(&s_scratch[k_gpt_off_entry_count]);
  if (entry_lba == 0U) {
    return k_ra_err_validation_failed;
  }
  if (entry_size != (uint32_t)k_gpt_entry_bytes) {
    return k_ra_err_not_supported;
  }
  if (count > (uint32_t)k_gpt_entry_scan_max) {
    count = (uint32_t)k_gpt_entry_scan_max;
  }
  return priv_gpt_scan_entries(m, entry_lba, count, out_base);
}

/* ===========================================================================
 * exFAT (read-only) support
 * ===========================================================================
 */

/**
 * @brief Length of a NUL-terminated string.
 *
 * @details Counts bytes up to the NUL terminator.
 *
 * @param[in] s NUL-terminated string.
 * @return Character count before the terminator.
 * @retval 0..UINT32_MAX String length.
 * @pre @p s is non-NULL.
 * @pre @p s is NUL-terminated.
 * @post No state modified.
 * @post @p s is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint32_t priv_strlen(const char* s)
{
  uint32_t n = 0U;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

/**
 * @brief Uppercase an ASCII character (others returned unchanged).
 *
 * @details Maps a-z to A-Z; any other byte is returned unchanged.
 *
 * @param[in] c Input character.
 * @return Uppercased character.
 * @retval c The (possibly) uppercased value.
 * @pre None.
 * @pre @p c is a byte value.
 * @post No state modified.
 * @post Result depends only on @p c.
 * @note Avoids compound conditions (MC/DC).
 * @since 0.1.0
 */
static char priv_ascii_upper(char c)
{
  if (c < 'a') {
    return c;
  }
  if (c > 'z') {
    return c;
  }
  return (char)(c - ('a' - 'A'));
}

/**
 * @brief Detect an exFAT volume from its boot sector.
 *
 * @details exFAT stamps the ASCII string "EXFAT   " (5 chars + 3 spaces) at
 * offset 3 of the VBR, where a FAT BPB carries OEM text.
 *
 * @param[in] buf Sector-0 (VBR) contents (>= 512 bytes).
 * @return 1 if @p buf is an exFAT VBR, else 0.
 * @retval 1 exFAT signature present.
 * @retval 0 Not exFAT.
 * @pre @p buf is non-NULL.
 * @pre @p buf holds at least one sector.
 * @post No state modified.
 * @post @p buf is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t priv_exfat_is_volume(const uint8_t* buf)
{
  static const char sig[k_exfat_fsname_len] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_fsname_len; i++) {
    if (buf[(uint32_t)k_exfat_off_fsname + i] != (uint8_t)sig[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Parse an exFAT VBR into the mount's geometry fields.
 *
 * @details Maps exFAT's sector-relative region offsets + log2 geometry onto
 * the shared FAT fields so ::priv_cluster_to_lba / ::priv_fat_get (FAT32 path)
 * serve the data + FAT regions unchanged. Only 512-byte sectors are supported.
 *
 * @param[in,out] m   Mount with backend bound and base LBA set.
 * @param[in]     buf VBR contents (sector 0 at the volume base).
 * @return Error code.
 * @retval k_ra_ok                exFAT geometry stored; ``m->type`` set.
 * @retval k_ra_err_not_supported BytesPerSectorShift is not 9 (512 B).
 * @pre @p m and @p buf are non-NULL.
 * @pre @p buf is a validated exFAT VBR (::priv_exfat_is_volume true).
 * @post On success ``m`` carries the exFAT region geometry.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe; serialize mount operations.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_parse(ra_fs_mount_t* m, const uint8_t* buf)
{
  if (buf[k_exfat_off_bps_shift] != (uint8_t)k_exfat_bps_shift_512) {
    return k_ra_err_not_supported;
  }
  m->type                = k_ra_fs_type_exfat;
  m->bytes_per_sector    = k_ra_fs_bytes_per_sector;
  m->sectors_per_cluster = 1U << buf[k_exfat_off_spc_shift];
  m->num_fats            = (uint32_t)buf[k_exfat_off_num_fats];
  m->fat_size_sectors    = priv_rd32(&buf[k_exfat_off_fat_len]);
  m->first_fat_lba       = priv_rd32(&buf[k_exfat_off_fat_lba]);
  m->first_data_lba      = priv_rd32(&buf[k_exfat_off_heap_lba]);
  m->root_cluster        = priv_rd32(&buf[k_exfat_off_root_clus]);
  m->count_of_clusters   = priv_rd32(&buf[k_exfat_off_clus_count]);
  m->reserved_sectors    = 0U;
  m->root_entries        = 0U;
  m->first_root_lba      = 0U;
  m->total_sectors       = 0U;
  return k_ra_ok;
}

/**
 * @brief Parse the volume at the current base: exFAT first, then FAT BPB.
 *
 * @details Dispatches to the exFAT parser when the VBR carries the exFAT
 * signature, else to the FAT BPB parser.
 *
 * @param[in,out] m Mount with sector 0 already read into ::s_scratch.
 * @return Error code from the chosen parser.
 * @retval k_ra_ok    Volume parsed (FAT or exFAT).
 * @retval k_ra_err_* No recognizable volume at this base.
 * @pre @p m is non-NULL and ::s_scratch holds the base sector 0.
 * @pre ``m->backend`` is bound.
 * @post On success ``m`` holds the volume geometry + type.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_parse_volume(ra_fs_mount_t* m)
{
  if (priv_exfat_is_volume(s_scratch) != 0U) {
    return priv_exfat_parse(m, s_scratch);
  }
  return priv_parse_bpb_into_mount(m);
}

/**
 * @struct exfat_cursor_t
 * @brief Linear cursor over a directory's 32-byte entries.
 */
typedef struct {
  uint32_t cluster;          /**< Current directory cluster.            */
  uint32_t entry_in_cluster; /**< Next entry index within the cluster.  */
  uint32_t scanned;          /**< Total entries read (P10 bound).       */
} exfat_cursor_t;

/**
 * @brief Fetch the next 32-byte directory entry, following the cluster chain.
 *
 * @details Advances across sectors and (via the FAT) clusters. Reports
 * end-of-directory as ::k_ra_err_not_found when the chain reaches EOC.
 *
 * @param[in]     m   Mounted exFAT volume.
 * @param[in,out] cur Cursor; advanced by one entry on success.
 * @param[out]    out Receives the 32-byte entry.
 * @return Error code.
 * @retval k_ra_ok            ``out`` holds the next entry.
 * @retval k_ra_err_not_found The directory chain ended (EOC).
 * @retval k_ra_err_*         Backend or FAT read failure.
 * @pre @p m, @p cur, and @p out are non-NULL.
 * @pre ``cur->cluster`` is a valid directory cluster.
 * @post On success ``cur`` points at the following entry.
 * @post On failure ``out`` is undefined.
 * @note Re-reads the sector per entry (simple; dir scans are short).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_next_entry(const ra_fs_mount_t* m, exfat_cursor_t* cur, uint8_t* out)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  if (cur->entry_in_cluster >= per_cluster) {
    uint32_t next = 0U;
    ra_err_t e    = priv_fat_get(m, cur->cluster, &next);
    if (e != k_ra_ok) {
      return e;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra_err_not_found;
    }
    cur->cluster          = next;
    cur->entry_in_cluster = 0U;
  }
  const uint32_t byte_off = cur->entry_in_cluster * (uint32_t)k_exfat_entry_bytes;
  const uint32_t lba = priv_cluster_to_lba(m, cur->cluster) + (byte_off / k_ra_fs_bytes_per_sector);
  uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
  ra_err_t       e                             = priv_read_sector(m, lba, sec);
  if (e != k_ra_ok) {
    return e;
  }
  priv_byte_copy(out, &sec[byte_off % k_ra_fs_bytes_per_sector], (uint32_t)k_exfat_entry_bytes);
  cur->entry_in_cluster++;
  cur->scanned++;
  return k_ra_ok;
}

/**
 * @brief Compare one file-name entry's 15 UTF-16 units against an ASCII path.
 *
 * @details Case-insensitive ASCII match; any non-ASCII unit fails the match.
 * Positions at/after @p nlen are treated as already matched (tail padding).
 *
 * @param[in] entry 32-byte file-name (0xC1) entry.
 * @param[in] path  Target path (ASCII).
 * @param[in] pos   Index of the first name unit this entry covers.
 * @param[in] nlen  Total name length in UTF-16 units.
 * @return 1 if this slice matches, else 0.
 * @retval 1 Slice matches.
 * @retval 0 Mismatch or non-ASCII unit.
 * @pre @p entry and @p path are non-NULL.
 * @pre ``priv_strlen(path) == nlen``.
 * @post No state modified.
 * @post Inputs are unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t
priv_exfat_name_chunk_eq(const uint8_t* entry, const char* path, uint32_t pos, uint32_t nlen)
{
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_name_per_entry; i++) {
    if ((pos + i) >= nlen) {
      return 1U;
    }
    const uint32_t b = (uint32_t)k_exfat_name_off + (i * 2U);
    if (entry[b + 1U] != 0U) {
      return 0U;
    }
    if (priv_ascii_upper((char)entry[b]) != priv_ascii_upper(path[pos + i])) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Read a stream-ext + name set and test it against @p path.
 *
 * @details Called with @p cur positioned just after a 0x85 File entry. Reads
 * the 0xC0 stream-extension entry and the following 0xC1 name entries; on a
 * full match fills the file's location fields.
 *
 * @param[in]     m         Mounted exFAT volume.
 * @param[in,out] cur       Cursor (advanced past the consumed entries).
 * @param[in]     path      Target path (ASCII, flat root name).
 * @param[out]    out_first First cluster of the matched file.
 * @param[out]    out_size  File length in bytes (low 32 bits).
 * @param[out]    out_nofat 1 if the file is contiguous (NoFatChain).
 * @return Error code.
 * @retval k_ra_ok            Match; outputs populated.
 * @retval k_ra_err_not_found This set is not @p path.
 * @retval k_ra_err_*         Backend read failure.
 * @pre All pointers are non-NULL; @p cur follows a 0x85 entry.
 * @pre @p path is a flat (root-level) name.
 * @post On match the out-params describe the file.
 * @post On non-match the out-params are untouched.
 * @note Leftover name entries self-heal in ::priv_exfat_find.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_match_set(const ra_fs_mount_t* m,
                                     exfat_cursor_t*      cur,
                                     const char*          path,
                                     uint32_t*            out_first,
                                     uint32_t*            out_size,
                                     uint8_t*             out_nofat)
{
  uint8_t  strm[k_exfat_entry_bytes] = {};
  ra_err_t e                         = priv_exfat_next_entry(m, cur, strm);
  if (e != k_ra_ok) {
    return e;
  }
  if (strm[0] != (uint8_t)k_exfat_entry_stream) {
    return k_ra_err_not_found;
  }
  const uint32_t nlen = (uint32_t)strm[k_exfat_strm_off_nlen];
  if (nlen != priv_strlen(path)) {
    return k_ra_err_not_found;
  }
  for (uint32_t pos = 0U; pos < nlen; pos += (uint32_t)k_exfat_name_per_entry) {
    uint8_t nm[k_exfat_entry_bytes] = {};
    e                               = priv_exfat_next_entry(m, cur, nm);
    if (e != k_ra_ok) {
      return e;
    }
    if (nm[0] != (uint8_t)k_exfat_entry_name) {
      return k_ra_err_not_found;
    }
    if (priv_exfat_name_chunk_eq(nm, path, pos, nlen) == 0U) {
      return k_ra_err_not_found;
    }
  }
  *out_first = priv_rd32(&strm[k_exfat_strm_off_clus]);
  *out_size  = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  *out_nofat = ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  return k_ra_ok;
}

/**
 * @brief Find a flat root-directory file by name on an exFAT volume.
 *
 * @details Streams the root directory entries, matching each File entry set
 * against @p path; stops at end-of-directory or the scan bound.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      Target path (ASCII, root-level name).
 * @param[out] out_first First cluster of the file.
 * @param[out] out_size  File length in bytes (low 32 bits).
 * @param[out] out_nofat 1 if the file is contiguous (NoFatChain).
 * @return Error code.
 * @retval k_ra_ok            File found; outputs populated.
 * @retval k_ra_err_not_found No matching entry in the root directory.
 * @retval k_ra_err_*         Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre ``m->root_cluster`` is valid.
 * @post On success the out-params describe the file.
 * @post Scan is bounded by ::k_exfat_scan_limit entries.
 * @note Only the root directory is searched (flat namespace).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_find(const ra_fs_mount_t* m,
                                const char*          path,
                                uint32_t*            out_first,
                                uint32_t*            out_size,
                                uint8_t*             out_nofat)
{
  /* Leading slashes are not part of the name; match FAT's priv_path_to_83
   * behavior so ra_fs_open("/name") resolves on exFAT too (#93). */
  while (*path == '/') {
    path++;
  }
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t  entry[k_exfat_entry_bytes] = {};
    ra_err_t e                          = priv_exfat_next_entry(m, &cur, entry);
    if (e != k_ra_ok) {
      return e;
    }
    if (entry[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra_err_not_found;
    }
    if (entry[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    e = priv_exfat_match_set(m, &cur, path, out_first, out_size, out_nofat);
    if (e == k_ra_ok) {
      return k_ra_ok;
    }
    if (e != k_ra_err_not_found) {
      return e;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief Open a file (read-only) on a mounted exFAT volume.
 *
 * @details Resolves @p path in the root directory and populates a read handle;
 * write/append modes are rejected (exFAT is read-only here).
 *
 * @param[in]  handle   Mounted exFAT volume.
 * @param[in]  path     Flat root-level file name (ASCII).
 * @param[in]  mode     Open mode; only ::k_ra_fs_mode_read is supported.
 * @param[out] out_file Receives the open handle.
 * @return Error code.
 * @retval k_ra_ok                File opened.
 * @retval k_ra_err_not_supported Write/append requested (exFAT is read-only).
 * @retval k_ra_err_not_found     No such file.
 * @retval k_ra_err_no_mem        File table full.
 * @pre @p handle, @p path, @p out_file are non-NULL; mount is exFAT.
 * @pre @p handle is in use.
 * @post On success ``*out_file`` is an in-use read handle.
 * @post On failure no file slot is consumed.
 * @note Not thread-safe; callers serialize.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_open(ra_fs_mount_t* handle, const char* path, ra_fs_mode_t mode, ra_fs_file_t** out_file)
{
  if (mode != k_ra_fs_mode_read) {
    return k_ra_err_not_supported;
  }
  uint32_t first = 0U;
  uint32_t size  = 0U;
  uint8_t  nofat = 0U;
  ra_err_t e     = priv_exfat_find(handle, path, &first, &size, &nofat);
  if (e != k_ra_ok) {
    return e;
  }
  ra_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra_err_no_mem;
  }
  f->mount         = handle;
  f->first_cluster = first;
  f->cur_cluster   = first;
  f->size_bytes    = size;
  f->offset        = 0U;
  f->dir_entry_lba = 0U;
  f->dir_entry_idx = 0U;
  f->mode          = mode;
  f->no_fat_chain  = nofat;
  f->in_use        = 1U;
  *out_file        = f;
  return k_ra_ok;
}

/* ---- exFAT one-shot file writer (provisioning) -------------------------- */

/**
 * @brief One step of exFAT's rotate-right-then-add 16-bit checksum.
 *
 * @details Used for both the directory SetChecksum and the name hash.
 * @param[in] cs Running checksum.
 * @param[in] b  Next byte.
 * @return Updated checksum.
 * @retval 0..0xFFFF The folded value.
 * @pre None.
 * @pre @p cs is the prior running value.
 * @post No state modified.
 * @post Result depends only on inputs.
 * @note Pure function.
 * @since 0.1.0
 */
static uint16_t priv_exfat_csum_add(uint16_t cs, uint8_t b)
{
  uint16_t hi = ((cs & 1U) != 0U) ? (uint16_t)k_exfat_csum_hi_bit : (uint16_t)0U;
  return (uint16_t)(hi + (uint16_t)(cs >> 1) + (uint16_t)b);
}

/**
 * @brief Compute the exFAT NameHash for an ASCII path.
 *
 * @details Hashes the up-cased UTF-16LE name (low then high byte per unit).
 * @param[in] path File name (ASCII).
 * @param[in] nlen Name length in characters.
 * @return 16-bit name hash.
 * @retval 0..0xFFFF The hash value.
 * @pre @p path is non-NULL.
 * @pre ``priv_strlen(path) == nlen``.
 * @post No state modified.
 * @post Inputs unmodified.
 * @note ASCII up-casing matches the standard up-case table for a-z.
 * @since 0.1.0
 */
static uint16_t priv_exfat_name_hash(const char* path, uint32_t nlen)
{
  uint16_t h = 0U;
  for (uint32_t i = 0U; i < nlen; i++) {
    h = priv_exfat_csum_add(h, (uint8_t)priv_ascii_upper(path[i]));
    h = priv_exfat_csum_add(h, 0U);
  }
  return h;
}

/**
 * @brief Compute the SetChecksum over a built directory entry set.
 *
 * @details Folds every byte except the File entry's checksum field (bytes 2-3).
 * @param[in] set   Contiguous entry-set bytes (File + Stream + Name entries).
 * @param[in] bytes Total byte count of the set.
 * @return 16-bit SetChecksum.
 * @retval 0..0xFFFF The checksum.
 * @pre @p set is non-NULL and at least @p bytes long.
 * @pre @p bytes is a multiple of the entry size.
 * @post No state modified.
 * @post @p set is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
static uint16_t priv_exfat_set_checksum(const uint8_t* set, uint32_t bytes)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < bytes; i++) {
    if (i == (uint32_t)k_exfat_off_file_csum) {
      continue;
    }
    if (i == ((uint32_t)k_exfat_off_file_csum + 1U)) {
      continue;
    }
    cs = priv_exfat_csum_add(cs, set[i]);
  }
  return cs;
}

/**
 * @brief Locate the allocation-bitmap entry in the exFAT root directory.
 *
 * @details Streams the root directory for the 0x81 entry and returns its data run.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[out] out_clus  First cluster of the allocation bitmap.
 * @param[out] out_len   Bitmap length in bytes.
 * @return Error code.
 * @retval k_ra_ok            Bitmap located.
 * @retval k_ra_err_not_found No allocation-bitmap entry.
 * @retval k_ra_err_*         Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre ``m->root_cluster`` is valid.
 * @post On success the bitmap location is returned.
 * @post No volume state modified.
 * @note Reads only the directory chain.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_find_bitmap(const ra_fs_mount_t* m, uint32_t* out_clus, uint32_t* out_len)
{
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t  e[k_exfat_entry_bytes] = {};
    ra_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra_err_not_found;
    }
    if (e[0] == (uint8_t)k_exfat_entry_bitmap) {
      *out_clus = priv_rd32(&e[k_exfat_strm_off_clus]);
      *out_len  = priv_rd32(&e[k_exfat_strm_off_dlen]);
      return k_ra_ok;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief Scan the allocation bitmap for a contiguous run of free clusters.
 *
 * @details Assumes a contiguous bitmap (true for a freshly formatted volume).
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  bmp_lba  First LBA (volume-relative) of the bitmap.
 * @param[in]  need     Number of contiguous free clusters required.
 * @param[out] out_clus First cluster of the found run.
 * @return Error code.
 * @retval k_ra_ok         A run of @p need free clusters was found.
 * @retval k_ra_err_no_mem No such run (volume full / too fragmented).
 * @retval k_ra_err_*      Backend read failure.
 * @pre @p m and @p out_clus are non-NULL; @p need >= 1.
 * @pre The bitmap region is contiguous on disk.
 * @post On success ``*out_clus`` is the run's first cluster number.
 * @post No volume state modified.
 * @note O(count_of_clusters); bounded by the volume size.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_bitmap_scan(const ra_fs_mount_t* m, uint32_t bmp_lba, uint32_t need, uint32_t* out_clus)
{
  uint32_t run                           = 0U;
  uint32_t start                         = 0U;
  uint32_t loaded                        = UINT32_MAX;
  uint8_t  sec[k_ra_fs_bytes_per_sector] = {};
  for (uint32_t idx = 0U; idx < m->count_of_clusters; idx++) {
    const uint32_t lba = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra_fs_bytes_per_sector);
    if (lba != loaded) {
      ra_err_t e = priv_read_sector(m, lba, sec);
      if (e != k_ra_ok) {
        return e;
      }
      loaded = lba;
    }
    const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra_fs_bytes_per_sector;
    const uint32_t bit  = idx & k_exfat_bit_mask;
    if (((sec[byte] >> bit) & 1U) != 0U) {
      run = 0U;
      continue;
    }
    if (run == 0U) {
      start = idx;
    }
    run++;
    if (run >= need) {
      *out_clus = start + k_cluster_first_data;
      return k_ra_ok;
    }
  }
  return k_ra_err_no_mem;
}

/**
 * @brief Flush the cached bitmap sector and load @p lba if it changed.
 *
 * @details Writes the dirty cached sector before reading the newly requested one.
 *
 * @param[in]     m      Mounted exFAT volume.
 * @param[in]     lba    Bitmap sector wanted next.
 * @param[in,out] loaded Currently-cached LBA (UINT32_MAX if none).
 * @param[in,out] sec    Cached sector buffer.
 * @return Error code.
 * @retval k_ra_ok    @p sec now holds @p lba.
 * @retval k_ra_err_* Backend read/write failure.
 * @pre All pointers are non-NULL.
 * @pre @p sec matches @p loaded on entry.
 * @post @p sec holds @p lba; the previous sector was written if dirty.
 * @post @p loaded == @p lba.
 * @note Keeps the caller's loop nesting shallow.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_bmp_switch(const ra_fs_mount_t* m, uint32_t lba, uint32_t* loaded, uint8_t* sec)
{
  if (lba == *loaded) {
    return k_ra_ok;
  }
  if (*loaded != UINT32_MAX) {
    ra_err_t we = priv_write_sector(m, *loaded, sec);
    if (we != k_ra_ok) {
      return we;
    }
  }
  ra_err_t e = priv_read_sector(m, lba, sec);
  if (e != k_ra_ok) {
    return e;
  }
  *loaded = lba;
  return k_ra_ok;
}

/**
 * @brief Mark a contiguous cluster run as allocated in the bitmap.
 *
 * @details Sets one bit per cluster, batching read-modify-write per bitmap sector.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark used.
 * @return Error code.
 * @retval k_ra_ok    All bits set + written.
 * @retval k_ra_err_* Backend read/write failure.
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre The bitmap region is contiguous on disk.
 * @post The @p count bits for the run read as 1.
 * @post Only the affected bitmap sectors are rewritten.
 * @note Read-modify-write, one sector at a time.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_bitmap_mark(const ra_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count)
{
  uint32_t loaded                        = UINT32_MAX;
  uint8_t  sec[k_ra_fs_bytes_per_sector] = {};
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t idx  = (clus - k_cluster_first_data) + k;
    const uint32_t lba  = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra_fs_bytes_per_sector);
    const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra_fs_bytes_per_sector;
    const uint32_t bit  = idx & k_exfat_bit_mask;
    ra_err_t       e    = priv_exfat_bmp_switch(m, lba, &loaded, sec);
    if (e != k_ra_ok) {
      return e;
    }
    sec[byte] = (uint8_t)(sec[byte] | (uint8_t)(1U << bit));
  }
  return priv_write_sector(m, loaded, sec);
}

/**
 * @brief Write file data into a contiguous cluster run (zero-padded).
 *
 * @details Copies the bytes sector by sector, zero-filling the final partial sector.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] clus First cluster of the run.
 * @param[in] data File bytes.
 * @param[in] len  Byte count (> 0).
 * @return Error code.
 * @retval k_ra_ok    All sectors written.
 * @retval k_ra_err_* Backend write failure.
 * @pre @p m and @p data are non-NULL; @p len > 0.
 * @pre The run has enough clusters for @p len.
 * @post The run holds @p data, last sector zero-padded.
 * @post No directory/bitmap state modified here.
 * @note Writes whole sectors.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_write_data(const ra_fs_mount_t* m, uint32_t clus, const uint8_t* data, uint32_t len)
{
  const uint32_t base    = priv_cluster_to_lba(m, clus);
  const uint32_t sectors = (len + k_ra_fs_bytes_per_sector - 1U) / k_ra_fs_bytes_per_sector;
  for (uint32_t s = 0U; s < sectors; s++) {
    uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
    const uint32_t off                           = s * k_ra_fs_bytes_per_sector;
    uint32_t       n                             = (off < len) ? (len - off) : 0U;
    if (n > k_ra_fs_bytes_per_sector) {
      n = k_ra_fs_bytes_per_sector;
    }
    if (n > 0U) {
      priv_byte_copy(sec, &data[off], n);
    }
    ra_err_t e = priv_write_sector(m, base + s, sec);
    if (e != k_ra_ok) {
      return e;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Read one 32-byte directory entry by index within a cluster.
 *
 * @details Reads the containing sector and copies out the addressed entry.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[in]  cluster Directory cluster.
 * @param[in]  idx     Entry index within the cluster.
 * @param[out] out     Receives the entry.
 * @return Error code.
 * @retval k_ra_ok    Entry read.
 * @retval k_ra_err_* Backend read failure.
 * @pre All pointers are non-NULL.
 * @pre @p idx is within the cluster's entry capacity.
 * @post ``out`` holds the entry on success.
 * @post No state modified.
 * @note Reads the containing sector.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_read_entry(const ra_fs_mount_t* m, uint32_t cluster, uint32_t idx, uint8_t* out)
{
  const uint32_t byte_off = idx * (uint32_t)k_exfat_entry_bytes;
  const uint32_t lba      = priv_cluster_to_lba(m, cluster) + (byte_off / k_ra_fs_bytes_per_sector);
  uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
  ra_err_t       e                             = priv_read_sector(m, lba, sec);
  if (e != k_ra_ok) {
    return e;
  }
  priv_byte_copy(out, &sec[byte_off % k_ra_fs_bytes_per_sector], (uint32_t)k_exfat_entry_bytes);
  return k_ra_ok;
}

/**
 * @brief True if a directory entry slot is free (end-of-dir or deleted).
 *
 * @details A slot is reusable when it is the end-of-directory marker or has bit 7 clear.
 *
 * @param[in] type_byte Entry type byte (entry[0]).
 * @return 1 if the slot is available for reuse, else 0.
 * @retval 1 Slot is free.
 * @retval 0 Slot is in use.
 * @pre None.
 * @pre @p type_byte is entry[0].
 * @post No state modified.
 * @post Result depends only on @p type_byte.
 * @note exFAT marks an in-use entry with bit 7 set.
 * @since 0.1.0
 */
static uint8_t priv_exfat_slot_free(uint8_t type_byte)
{
  if (type_byte == (uint8_t)k_exfat_entry_eod) {
    return 1U;
  }
  if ((type_byte & (uint8_t)k_exfat_inuse_bit) == 0U) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Find @p need consecutive free entries within one root-dir cluster.
 *
 * @details Scans each directory cluster for a run of free slots large enough for the set.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  need     Number of consecutive free entries required.
 * @param[out] out_clus Cluster holding the run.
 * @param[out] out_idx  Entry index of the run start within that cluster.
 * @return Error code.
 * @retval k_ra_ok         A run was found.
 * @retval k_ra_err_no_mem No run of @p need entries in the root directory.
 * @retval k_ra_err_*      Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre @p need >= 1.
 * @post On success the run location is returned.
 * @post No volume state modified.
 * @note The set is kept within a single cluster (no chain spanning).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_find_dir_space(const ra_fs_mount_t* m,
                                          uint32_t             need,
                                          uint32_t*            out_clus,
                                          uint32_t*            out_idx)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  uint32_t cluster = m->root_cluster;
  uint32_t guard   = 0U;
  while (guard < (uint32_t)k_exfat_scan_limit) {
    uint32_t run = 0U;
    for (uint32_t i = 0U; i < per_cluster; i++) {
      uint8_t  e[k_exfat_entry_bytes] = {};
      ra_err_t r                      = priv_exfat_read_entry(m, cluster, i, e);
      if (r != k_ra_ok) {
        return r;
      }
      if (priv_exfat_slot_free(e[0]) == 0U) {
        run = 0U;
        continue;
      }
      if (run == 0U) {
        *out_idx = i;
      }
      run++;
      if (run >= need) {
        *out_clus = cluster;
        return k_ra_ok;
      }
    }
    uint32_t next = 0U;
    ra_err_t fe   = priv_fat_get(m, cluster, &next);
    if (fe != k_ra_ok) {
      return fe;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra_err_no_mem;
    }
    cluster = next;
    guard++;
  }
  return k_ra_err_no_mem;
}

/**
 * @brief Build the File + Stream + Name entry set into @p set.
 *
 * @details Fills the typed entries, the name hash, and the trailing SetChecksum.
 *
 * @param[out] set        Buffer (>= set_len * 32 bytes).
 * @param[in]  path       File name (ASCII).
 * @param[in]  nlen       Name length in characters.
 * @param[in]  first_clus First data cluster of the file.
 * @param[in]  len        File length in bytes.
 * @return The total byte length of the built set.
 * @retval >0 Number of bytes written into @p set.
 * @pre @p set and @p path are non-NULL; @p set is large enough.
 * @pre ``priv_strlen(path) == nlen``.
 * @post @p set holds a complete entry set with a valid SetChecksum.
 * @post No volume state modified.
 * @note NoFatChain (contiguous) is recorded in the stream flags.
 * @since 0.1.0
 */
static uint32_t priv_exfat_build_set(uint8_t*    set,
                                     const char* path,
                                     uint32_t    nlen,
                                     uint32_t    first_clus,
                                     uint32_t    len)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t sec_count = 1U + name_entries;
  const uint32_t total     = (1U + sec_count) * (uint32_t)k_exfat_entry_bytes;
  for (uint32_t i = 0U; i < total; i++) {
    set[i] = 0U;
  }
  set[0]                      = (uint8_t)k_exfat_entry_file;
  set[k_exfat_off_file_secnt] = (uint8_t)sec_count;
  priv_wr16(&set[k_exfat_off_file_attr], (uint16_t)k_exfat_attr_archive);
  uint8_t* strm                = &set[k_exfat_entry_bytes];
  strm[0]                      = (uint8_t)k_exfat_entry_stream;
  strm[k_exfat_strm_off_flags] = (uint8_t)k_exfat_secflag_alloc;
  strm[k_exfat_strm_off_nlen]  = (uint8_t)nlen;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(path, nlen));
  priv_wr32(&strm[k_exfat_off_strm_valid], len);
  priv_wr32(&strm[k_exfat_strm_off_clus], first_clus);
  priv_wr32(&strm[k_exfat_strm_off_dlen], len);
  for (uint32_t n = 0U; n < name_entries; n++) {
    uint8_t* ne = &set[(size_t)(2U + n) * (size_t)k_exfat_entry_bytes];
    ne[0]       = (uint8_t)k_exfat_entry_name;
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t pos = (n * (uint32_t)k_exfat_name_per_entry) + c;
      if (pos < nlen) {
        ne[k_exfat_name_off + (c * 2U)] = (uint8_t)path[pos];
      }
    }
  }
  priv_wr16(&set[k_exfat_off_file_csum], priv_exfat_set_checksum(set, total));
  return total;
}

/**
 * @brief Write a pre-built entry set into consecutive directory entries.
 *
 * @details Read-modify-writes each entry slot so neighbouring entries are preserved.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] cluster Directory cluster holding the run.
 * @param[in] idx     Entry index of the run start.
 * @param[in] set     Built entry-set bytes.
 * @param[in] bytes   Total size of the set.
 * @return Error code.
 * @retval k_ra_ok    Entries written.
 * @retval k_ra_err_* Backend read/write failure.
 * @pre @p m and @p set are non-NULL; the run fits in the cluster.
 * @pre @p bytes is a multiple of the entry size.
 * @post The directory holds the new entry set.
 * @post Read-modify-write preserves neighbouring entries.
 * @note Writes one entry (sector RMW) at a time.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_write_dir_set(const ra_fs_mount_t* m,
                                         uint32_t             cluster,
                                         uint32_t             idx,
                                         const uint8_t*       set,
                                         uint32_t             bytes)
{
  const uint32_t count = bytes / (uint32_t)k_exfat_entry_bytes;
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t byte_off = (idx + k) * (uint32_t)k_exfat_entry_bytes;
    const uint32_t lba = priv_cluster_to_lba(m, cluster) + (byte_off / k_ra_fs_bytes_per_sector);
    uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
    ra_err_t       e                             = priv_read_sector(m, lba, sec);
    if (e != k_ra_ok) {
      return e;
    }
    priv_byte_copy(&sec[byte_off % k_ra_fs_bytes_per_sector],
                   &set[(size_t)k * (size_t)k_exfat_entry_bytes],
                   (uint32_t)k_exfat_entry_bytes);
    e = priv_write_sector(m, lba, sec);
    if (e != k_ra_ok) {
      return e;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Allocate a contiguous run, write the data, and mark the bitmap.
 *
 * @details Combines bitmap scan, data write, and bitmap marking into one step.
 *
 * @param[in]  m     Mounted exFAT volume.
 * @param[in]  data  File bytes.
 * @param[in]  len   Byte count (> 0).
 * @param[in]  nclus Clusters required.
 * @param[out] out_start First cluster of the allocated run.
 * @return Error code.
 * @retval k_ra_ok         Data written; run marked used.
 * @retval k_ra_err_no_mem No contiguous run of @p nclus clusters.
 * @retval k_ra_err_*      Bitmap or backend failure.
 * @pre All pointers are non-NULL; @p nclus >= 1.
 * @pre ``m->type`` is exFAT.
 * @post On success the run holds the data and is marked allocated.
 * @post On failure an unlinked run may remain allocated.
 * @note Contiguous (NoFatChain) allocation only.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_alloc_write(ra_fs_mount_t* m,
                                       const uint8_t* data,
                                       uint32_t       len,
                                       uint32_t       nclus,
                                       uint32_t*      out_start)
{
  uint32_t bclus = 0U;
  uint32_t blen  = 0U;
  ra_err_t e     = priv_exfat_find_bitmap(m, &bclus, &blen);
  if (e != k_ra_ok) {
    return e;
  }
  const uint32_t bmp_lba = priv_cluster_to_lba(m, bclus);
  e                      = priv_exfat_bitmap_scan(m, bmp_lba, nclus, out_start);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_exfat_write_data(m, *out_start, data, len);
  if (e != k_ra_ok) {
    return e;
  }
  // NOLINTNEXTLINE(readability-suspicious-call-argument) -- (cluster, count) order is correct
  return priv_exfat_bitmap_mark(m, bmp_lba, *out_start, nclus);
}

/**
 * @brief Append a File/Stream/Name entry set linking a written run.
 *
 * @details Finds directory space, builds the entry set, and writes it.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] path  File name (ASCII).
 * @param[in] nlen  Name length in characters.
 * @param[in] start First data cluster of the file.
 * @param[in] len   File length in bytes.
 * @return Error code.
 * @retval k_ra_ok         Entry set written.
 * @retval k_ra_err_no_mem No directory space.
 * @retval k_ra_err_*      Backend failure.
 * @pre @p m and @p path are non-NULL; ``m->type`` is exFAT.
 * @pre ``priv_strlen(path) == nlen``.
 * @post On success the root directory references the file.
 * @post No data clusters are modified here.
 * @note Keeps the entry set within one directory cluster.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_link(ra_fs_mount_t* m, const char* path, uint32_t nlen, uint32_t start, uint32_t len)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t need  = 2U + name_entries;
  uint32_t       dclus = 0U;
  uint32_t       didx  = 0U;
  ra_err_t       e     = priv_exfat_find_dir_space(m, need, &dclus, &didx);
  if (e != k_ra_ok) {
    return e;
  }
  uint8_t        set[k_exfat_max_set_bytes] = {};
  const uint32_t bytes                      = priv_exfat_build_set(set, path, nlen, start, len);
  return priv_exfat_write_dir_set(m, dclus, didx, set, bytes);
}

/**
 * @brief Create a contiguous file on an exFAT volume and write its contents.
 *
 * @details Allocates a contiguous run from the allocation bitmap, writes the
 * data, then appends a File/Stream/Name entry set (NoFatChain) to the root
 * directory. One-shot provisioning helper; does not overwrite an existing file.
 *
 * @param[in,out] m    Mounted exFAT volume.
 * @param[in]     path Flat root-level file name (ASCII).
 * @param[in]     data File bytes.
 * @param[in]     len  Byte count (> 0).
 * @return Error code.
 * @retval k_ra_ok              File created.
 * @retval k_ra_err_invalid_arg Empty/oversized name or zero length.
 * @retval k_ra_err_no_mem      No contiguous space or directory slots.
 * @retval k_ra_err_*           Backend or bitmap failure.
 * @pre @p m, @p path, @p data are non-NULL; ``m->type`` is exFAT.
 * @pre @p path does not already exist (not checked here).
 * @post On success the file is allocated, written, and linked.
 * @post On failure the volume may hold an orphaned (unlinked) run.
 * @note Contiguous allocation only (NoFatChain).
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_create(ra_fs_mount_t* m, const char* path, const uint8_t* data, uint32_t len)
{
  /* Strip leading slashes so the stored name matches what the matchers
   * search for (#93); otherwise the file is created but cannot be reopened. */
  while (*path == '/') {
    path++;
  }
  const uint32_t nlen = priv_strlen(path);
  if (nlen == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (nlen > (uint32_t)k_exfat_name_cap) {
    return k_ra_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t cbytes = m->sectors_per_cluster * k_ra_fs_bytes_per_sector;
  const uint32_t nclus  = (len + cbytes - 1U) / cbytes;
  uint32_t       start  = 0U;
  ra_err_t       e      = priv_exfat_alloc_write(m, data, len, nclus, &start);
  if (e != k_ra_ok) {
    return e;
  }
  return priv_exfat_link(m, path, nlen, start, len);
}

/* ---- exFAT mutation helpers (unlink / rename / listdir) ------------------ */

/**
 * @enum exfat_mutate_const_t
 * @brief Sizing constants for the exFAT mutation helpers.
 */
typedef enum : uint32_t {
  k_exfat_set_max_entries = 19U, /**< 1 File + 1 Stream + 17 Name entries.    */
  k_exfat_list_name_cap   = 64U, /**< Listdir name buffer (truncated + NUL).  */
  k_exfat_rename_entries  = 3U,  /**< In-place rename set: File+Stream+Name.  */
  k_exfat_rename_bytes    = 96U, /**< Three 32-byte entries.                  */
} exfat_mutate_const_t;

/**
 * @struct exfat_setpos_t
 * @brief Directory position (cluster + entry index) of one 32-byte entry.
 */
typedef struct {
  uint32_t cluster; /**< Directory cluster holding the entry.          */
  uint32_t index;   /**< Entry index within that cluster (0-based).    */
} exfat_setpos_t;

/**
 * @brief Clear a contiguous cluster run in the allocation bitmap.
 *
 * @details Mirror of ::priv_exfat_bitmap_mark: clears one bit per cluster,
 * batching read-modify-write per bitmap sector. Per the exFAT spec the
 * bitmap alone is authoritative for allocation state, so freeing does not
 * require FAT edits.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark free.
 * @return Error code.
 * @retval k_ra_ok    All bits cleared + written.
 * @retval k_ra_err_* Backend read/write failure.
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre The bitmap region is contiguous on disk.
 * @post The @p count bits for the run read as 0.
 * @post Only the affected bitmap sectors are rewritten.
 * @note Read-modify-write, one sector at a time.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_bitmap_clear(const ra_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count)
{
  uint32_t loaded                        = UINT32_MAX;
  uint8_t  sec[k_ra_fs_bytes_per_sector] = {};
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t idx  = (clus - k_cluster_first_data) + k;
    const uint32_t lba  = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra_fs_bytes_per_sector);
    const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra_fs_bytes_per_sector;
    const uint32_t bit  = idx & k_exfat_bit_mask;
    ra_err_t       e    = priv_exfat_bmp_switch(m, lba, &loaded, sec);
    if (e != k_ra_ok) {
      return e;
    }
    sec[byte] = (uint8_t)(sec[byte] & (uint8_t)~(uint8_t)(1U << bit));
  }
  return priv_write_sector(m, loaded, sec);
}

/**
 * @brief Consume a set's secondary entries, recording positions + matching.
 *
 * @details Reads the @p sc secondaries following a File entry, snapshots
 * each entry's (cluster, index) into @p pos starting at slot 1, and checks
 * the set against @p path: the first secondary must be a Stream entry with
 * the right NameLength, the rest must be Name entries whose UTF-16 chunks
 * equal the path. The cursor always consumes all @p sc entries so the
 * caller's walk stays aligned.
 *
 * @param[in]     m         Mounted exFAT volume.
 * @param[in,out] cur       Directory cursor (just past the File entry).
 * @param[in]     path      Target name (ASCII, root-level).
 * @param[in]     nlen      Length of @p path.
 * @param[in]     sc        SecondaryCount from the File entry.
 * @param[out]    pos       Position array (slot 0 already holds the File).
 * @param[out]    strm_copy Receives the 32-byte Stream entry when matched.
 * @param[out]    out_match Receives 1 when the whole set matches @p path.
 * @return Error code.
 * @retval k_ra_ok    All @p sc secondaries were consumed.
 * @retval k_ra_err_* Backend read failure mid-set.
 * @pre @p cur sits immediately after the set's File entry.
 * @pre @p pos has at least 1 + @p sc slots.
 * @post @p cur sits immediately after the set's last secondary.
 * @post On k_ra_ok @p out_match is 0 or 1.
 * @note Helper of ::priv_exfat_find_set (complexity split).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_take_set(const ra_fs_mount_t* m,
                                    exfat_cursor_t*      cur,
                                    const char*          path,
                                    uint32_t             nlen,
                                    uint32_t             sc,
                                    exfat_setpos_t*      pos,
                                    uint8_t*             strm_copy,
                                    uint8_t*             out_match)
{
  uint8_t matched = 1U;
  for (uint32_t k = 0U; k < sc; k++) {
    const exfat_setpos_t sp = {.cluster = cur->cluster, .index = cur->entry_in_cluster};
    uint8_t              se[k_exfat_entry_bytes] = {};
    const ra_err_t       r                       = priv_exfat_next_entry(m, cur, se);
    if (r != k_ra_ok) {
      return r;
    }
    pos[1U + k] = sp;
    if (k == 0U) {
      if (se[0] != (uint8_t)k_exfat_entry_stream) {
        matched = 0U;
        continue;
      }
      if ((uint32_t)se[k_exfat_strm_off_nlen] != nlen) {
        matched = 0U;
        continue;
      }
      priv_byte_copy(strm_copy, se, (uint32_t)k_exfat_entry_bytes);
      continue;
    }
    if (matched == 0U) {
      continue;
    }
    if (se[0] != (uint8_t)k_exfat_entry_name) {
      matched = 0U;
      continue;
    }
    const uint32_t cpos = (k - 1U) * (uint32_t)k_exfat_name_per_entry;
    if (priv_exfat_name_chunk_eq(se, path, cpos, nlen) == 0U) {
      matched = 0U;
    }
  }
  *out_match = matched;
  return k_ra_ok;
}

/**
 * @brief Locate a file's full directory-entry set with per-entry positions.
 *
 * @details Walks the root directory like ::priv_exfat_find but records the
 * (cluster, index) of every entry in the matched set -- the File entry plus
 * all its secondaries -- so callers can rewrite them in place. Non-matching
 * sets are skipped entry-by-entry, which keeps the cursor aligned.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      Target name (ASCII, root-level).
 * @param[out] pos       Receives the positions (File entry first).
 * @param[in]  max_pos   Capacity of @p pos.
 * @param[out] out_count Receives the entry count (1 + SecondaryCount).
 * @param[out] file_copy Receives the 32-byte File entry.
 * @param[out] strm_copy Receives the 32-byte Stream-extension entry.
 * @return Error code.
 * @retval k_ra_ok            Set found; outputs populated.
 * @retval k_ra_err_not_found No matching set in the root directory.
 * @retval k_ra_err_no_mem    The set has more entries than @p max_pos.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre @p path is a flat root-level name.
 * @post On success @p pos holds @p *out_count valid positions.
 * @post On failure the outputs are unspecified.
 * @note Only the root directory is searched (flat namespace).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_find_set(const ra_fs_mount_t* m,
                                    const char*          path,
                                    exfat_setpos_t*      pos,
                                    uint32_t             max_pos,
                                    uint32_t*            out_count,
                                    uint8_t*             file_copy,
                                    uint8_t*             strm_copy)
{
  /* Strip leading slashes so a "/name" path matches (#93), as FAT does. */
  while (*path == '/') {
    path++;
  }
  exfat_cursor_t cur  = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  const uint32_t nlen = priv_strlen(path);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    const exfat_setpos_t at = {.cluster = cur.cluster, .index = cur.entry_in_cluster};
    uint8_t              e[k_exfat_entry_bytes] = {};
    ra_err_t             r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra_err_not_found;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    const uint32_t sc    = (uint32_t)e[k_exfat_off_file_secnt];
    const uint32_t total = 1U + sc;
    if (total > max_pos) {
      return k_ra_err_no_mem;
    }
    pos[0] = at;
    priv_byte_copy(file_copy, e, (uint32_t)k_exfat_entry_bytes);
    uint8_t matched = 0U;
    r               = priv_exfat_take_set(m, &cur, path, nlen, sc, pos, strm_copy, &matched);
    if (r != k_ra_ok) {
      return r;
    }
    if (matched == 1U) {
      *out_count = total;
      return k_ra_ok;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief Rewrite one 32-byte directory entry at a recorded position.
 *
 * @details Thin wrapper over ::priv_exfat_write_dir_set for a single entry.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] where Entry position from ::priv_exfat_find_set.
 * @param[in] entry The 32 bytes to write.
 * @return Error code.
 * @retval k_ra_ok    Entry rewritten.
 * @retval k_ra_err_* Backend read/write failure.
 * @pre @p m and @p entry are non-NULL.
 * @pre @p where came from ::priv_exfat_find_set.
 * @post The on-disk entry equals @p entry.
 * @post No other directory bytes change.
 * @note Read-modify-write of one sector.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_put_entry(const ra_fs_mount_t* m, const exfat_setpos_t* where, const uint8_t* entry)
{
  return priv_exfat_write_dir_set(m,
                                  where->cluster,
                                  where->index,
                                  entry,
                                  (uint32_t)k_exfat_entry_bytes);
}

/**
 * @brief Free the cluster run / chain referenced by a Stream entry.
 *
 * @details Contiguous (NoFatChain) files free ceil(size / cluster bytes)
 * bitmap bits from the first cluster; FAT-chained files walk the FAT and
 * free each visited cluster. Per the exFAT spec the bitmap alone is
 * authoritative, so the FAT entries themselves are left untouched.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] strm The file's 32-byte Stream-extension entry.
 * @return Error code.
 * @retval k_ra_ok    Clusters freed (or the file had none).
 * @retval k_ra_err_* Bitmap or backend failure.
 * @pre @p m and @p strm are non-NULL.
 * @pre The directory entries were already marked deleted.
 * @post The file's clusters read as free in the bitmap.
 * @post FAT contents are unchanged.
 * @note Chain walk is bounded by the volume's cluster count.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_free_clusters(const ra_fs_mount_t* m, const uint8_t* strm)
{
  const uint32_t first = priv_rd32(&strm[k_exfat_strm_off_clus]);
  const uint32_t size  = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  if (first < k_cluster_first_data) {
    return k_ra_ok;
  }
  if (size == 0U) {
    return k_ra_ok;
  }
  uint32_t bmp_clus = 0U;
  uint32_t bmp_len  = 0U;
  ra_err_t e        = priv_exfat_find_bitmap(m, &bmp_clus, &bmp_len);
  if (e != k_ra_ok) {
    return e;
  }
  const uint32_t bmp_lba       = priv_cluster_to_lba(m, bmp_clus);
  const uint32_t cluster_bytes = m->sectors_per_cluster * k_ra_fs_bytes_per_sector;
  const uint8_t  nofat =
    ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  if (nofat == 1U) {
    const uint32_t count = (size + cluster_bytes - 1U) / cluster_bytes;
    return priv_exfat_bitmap_clear(m, bmp_lba, first, count);
  }
  uint32_t clus = first;
  for (uint32_t guard = 0U; guard < m->count_of_clusters; guard++) {
    e = priv_exfat_bitmap_clear(m, bmp_lba, clus, 1U);
    if (e != k_ra_ok) {
      return e;
    }
    uint32_t next = 0U;
    e             = priv_fat_get(m, clus, &next);
    if (e != k_ra_ok) {
      return e;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra_ok;
    }
    clus = next;
  }
  return k_ra_ok;
}

/**
 * @brief Delete a root-level file on an exFAT volume.
 *
 * @details Locates the file's directory-entry set, clears the in-use bit
 * (bit 7 of the entry type) on every entry in the set, and frees the
 * file's clusters in the allocation bitmap.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] path Flat root-level file name (ASCII).
 * @return Error code.
 * @retval k_ra_ok            File unlinked.
 * @retval k_ra_err_not_found No such file.
 * @retval k_ra_err_*         Directory or bitmap write failure.
 * @pre @p m and @p path are non-NULL; mount is exFAT.
 * @pre The file is not open.
 * @post The name no longer resolves; its clusters are free.
 * @post Other directory entries are untouched.
 * @note Root-directory namespace only.
 * @since 0.1.0
 */
static ra_err_t priv_exfat_unlink(const ra_fs_mount_t* m, const char* path)
{
  exfat_setpos_t pos[k_exfat_set_max_entries] = {};
  uint32_t       count                        = 0U;
  uint8_t        file_e[k_exfat_entry_bytes]  = {};
  uint8_t        strm_e[k_exfat_entry_bytes]  = {};
  ra_err_t       e =
    priv_exfat_find_set(m, path, pos, (uint32_t)k_exfat_set_max_entries, &count, file_e, strm_e);
  if (e != k_ra_ok) {
    return e;
  }
  for (uint32_t k = 0U; k < count; k++) {
    exfat_cursor_t one                        = {.cluster          = pos[k].cluster,
                                                 .entry_in_cluster = pos[k].index,
                                                 .scanned          = 0U};
    uint8_t        entry[k_exfat_entry_bytes] = {};
    e                                         = priv_exfat_next_entry(m, &one, entry);
    if (e != k_ra_ok) {
      return e;
    }
    entry[0] = (uint8_t)(entry[0] & (uint8_t)~(uint8_t)k_exfat_inuse_bit);
    e        = priv_exfat_put_entry(m, &pos[k], entry);
    if (e != k_ra_ok) {
      return e;
    }
  }
  return priv_exfat_free_clusters(m, strm_e);
}

/**
 * @brief Patch a 3-entry set for a new name and rewrite it in place.
 *
 * @details Updates the Stream entry's NameLength + NameHash, rebuilds the
 * single Name entry from @p new_path, recomputes the File entry's
 * SetChecksum over the whole set, and writes all three entries back at
 * their recorded positions.
 *
 * @param[in]     m        Mounted exFAT volume.
 * @param[in]     pos      Positions of the set's three entries.
 * @param[in,out] set      The 96-byte set (File + Stream + Name).
 * @param[in]     new_path Replacement name (<= 15 characters).
 * @param[in]     new_len  Length of @p new_path.
 * @return Error code.
 * @retval k_ra_ok    All three entries rewritten.
 * @retval k_ra_err_* Backend write failure.
 * @pre @p set holds the file's current File + Stream entries.
 * @pre @p new_len fits one Name entry.
 * @post On k_ra_ok the on-disk set carries the new name + checksum.
 * @post @p set mirrors the on-disk bytes.
 * @note Helper of ::priv_exfat_rename (complexity split).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_apply_rename(const ra_fs_mount_t*  m,
                                        const exfat_setpos_t* pos,
                                        uint8_t*              set,
                                        const char*           new_path,
                                        uint32_t              new_len)
{
  uint8_t* strm               = &set[k_exfat_entry_bytes];
  uint8_t* name               = &set[(size_t)2U * (size_t)k_exfat_entry_bytes];
  strm[k_exfat_strm_off_nlen] = (uint8_t)new_len;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(new_path, new_len));
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_entry_bytes; i++) {
    name[i] = 0U;
  }
  name[0] = (uint8_t)k_exfat_entry_name;
  for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
    if (c < new_len) {
      name[k_exfat_name_off + (c * 2U)] = (uint8_t)new_path[c];
    }
  }
  priv_wr16(&set[k_exfat_off_file_csum],
            priv_exfat_set_checksum(set, (uint32_t)k_exfat_rename_bytes));
  for (uint32_t k = 0U; k < (uint32_t)k_exfat_rename_entries; k++) {
    const ra_err_t e =
      priv_exfat_put_entry(m, &pos[k], &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (e != k_ra_ok) {
      return e;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Rename a root-level file on an exFAT volume (in place).
 *
 * @details Supported when both names fit a single Name entry (<= 15
 * characters), which keeps the entry-set length unchanged: the Stream
 * entry's NameLength + NameHash are patched, the Name entry is rebuilt,
 * and the File entry's SetChecksum is recomputed; all three entries are
 * rewritten at their original positions.
 *
 * @param[in] m        Mounted exFAT volume.
 * @param[in] old_path Existing root-level name.
 * @param[in] new_path Replacement name (must not exist).
 * @return Error code.
 * @retval k_ra_ok                File renamed.
 * @retval k_ra_err_not_found     @p old_path does not exist.
 * @retval k_ra_err_exists       @p new_path already resolves.
 * @retval k_ra_err_not_supported A name needs more than one Name entry.
 * @pre @p m and both paths are non-NULL; mount is exFAT.
 * @pre The file is not open.
 * @post @p new_path resolves to the same data; @p old_path is gone.
 * @post File attributes, size, and clusters are unchanged.
 * @note Root-directory namespace only.
 * @since 0.1.0
 */
static ra_err_t
priv_exfat_rename(const ra_fs_mount_t* m, const char* old_path, const char* new_path)
{
  /* Strip leading slashes on both paths so the old name matches and the new
   * name is stored without a slash (#93), consistent with create/find. */
  while (*old_path == '/') {
    old_path++;
  }
  while (*new_path == '/') {
    new_path++;
  }
  const uint32_t old_len = priv_strlen(old_path);
  const uint32_t new_len = priv_strlen(new_path);
  if (old_len > (uint32_t)k_exfat_name_per_entry) {
    return k_ra_err_not_supported;
  }
  if (new_len > (uint32_t)k_exfat_name_per_entry) {
    return k_ra_err_not_supported;
  }
  uint32_t e_first = 0U;
  uint32_t e_size  = 0U;
  uint8_t  e_nofat = 0U;
  if (priv_exfat_find(m, new_path, &e_first, &e_size, &e_nofat) == k_ra_ok) {
    return k_ra_err_exists;
  }
  exfat_setpos_t pos[k_exfat_set_max_entries] = {};
  uint32_t       count                        = 0U;
  uint8_t        set[k_exfat_rename_bytes]    = {};
  const ra_err_t e = priv_exfat_find_set(m,
                                         old_path,
                                         pos,
                                         (uint32_t)k_exfat_set_max_entries,
                                         &count,
                                         &set[0],
                                         &set[k_exfat_entry_bytes]);
  if (e != k_ra_ok) {
    return e;
  }
  if (count != (uint32_t)k_exfat_rename_entries) {
    return k_ra_err_not_supported;
  }
  return priv_exfat_apply_rename(m, pos, set, new_path, new_len);
}

/**
 * @brief Consume a set's Name entries and assemble the ASCII name.
 *
 * @details Reads the @p sc - 1 secondaries that follow the Stream entry,
 * copying the low byte of each UTF-16 unit into @p name until @p nlen
 * characters (or the buffer cap) are gathered. Non-Name secondaries are
 * consumed and skipped so the caller's cursor stays aligned.
 *
 * @param[in]     m    Mounted exFAT volume.
 * @param[in,out] cur  Directory cursor (just past the Stream entry).
 * @param[in]     sc   SecondaryCount from the File entry.
 * @param[in]     nlen NameLength from the Stream entry.
 * @param[out]    name Receives the NUL-terminated ASCII name.
 * @param[in]     cap  Capacity of @p name in bytes.
 * @return Error code.
 * @retval k_ra_ok    All secondaries consumed; @p name terminated.
 * @retval k_ra_err_* Backend read failure mid-set.
 * @pre @p cur sits immediately after the set's Stream entry.
 * @pre @p cap is at least 1.
 * @post @p cur sits immediately after the set's last secondary.
 * @post @p name is NUL-terminated (possibly truncated).
 * @note Helper of ::priv_exfat_listdir (complexity split).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_gather_name(const ra_fs_mount_t* m,
                                       exfat_cursor_t*      cur,
                                       uint32_t             sc,
                                       uint32_t             nlen,
                                       char*                name,
                                       uint32_t             cap)
{
  uint32_t written = 0U;
  for (uint32_t k = 1U; k < sc; k++) {
    uint8_t        ne[k_exfat_entry_bytes] = {};
    const ra_err_t r                       = priv_exfat_next_entry(m, cur, ne);
    if (r != k_ra_ok) {
      return r;
    }
    if (ne[0] != (uint8_t)k_exfat_entry_name) {
      continue;
    }
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t p = ((k - 1U) * (uint32_t)k_exfat_name_per_entry) + c;
      if (p >= nlen) {
        break;
      }
      if (written < (cap - 1U)) {
        name[written] = (char)ne[k_exfat_name_off + (c * 2U)];
        written++;
      }
    }
  }
  name[written] = '\0';
  return k_ra_ok;
}

/**
 * @brief Enumerate the root directory of an exFAT volume.
 *
 * @details Walks the directory entry stream; every in-use File entry set
 * yields one callback with the ASCII name (truncated to the local buffer,
 * NUL-terminated), the low attribute byte, and the file size. Deleted
 * entries and non-file sets (bitmap, up-case table, label) are skipped.
 *
 * @param[in] m   Mounted exFAT volume.
 * @param[in] cb  Callback invoked once per visible file.
 * @param[in] ctx Cookie forwarded to the callback.
 * @return Error code.
 * @retval k_ra_ok    Enumeration completed (EOD reached).
 * @retval k_ra_err_* Backend read failure.
 * @pre @p m and @p cb are non-NULL; mount is exFAT.
 * @pre The volume is mounted.
 * @post @p cb ran once per in-use file set.
 * @post No volume state modified.
 * @note Names longer than the buffer are truncated (still NUL-terminated).
 * @since 0.1.0
 */
static ra_err_t priv_exfat_listdir(const ra_fs_mount_t* m, ra_fs_listdir_cb_t cb, void* ctx)
{
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t  e[k_exfat_entry_bytes] = {};
    ra_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra_ok;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    const uint32_t sc                        = (uint32_t)e[k_exfat_off_file_secnt];
    const uint8_t  attr                      = e[k_exfat_off_file_attr];
    uint8_t        strm[k_exfat_entry_bytes] = {};
    r                                        = priv_exfat_next_entry(m, &cur, strm);
    if (r != k_ra_ok) {
      return r;
    }
    if (strm[0] != (uint8_t)k_exfat_entry_stream) {
      continue;
    }
    const uint32_t size                        = priv_rd32(&strm[k_exfat_strm_off_dlen]);
    const uint32_t nlen                        = (uint32_t)strm[k_exfat_strm_off_nlen];
    char           name[k_exfat_list_name_cap] = {};
    r = priv_exfat_gather_name(m, &cur, sc, nlen, name, (uint32_t)k_exfat_list_name_cap);
    if (r != k_ra_ok) {
      return r;
    }
    cb(name, attr, size, ctx);
  }
  return k_ra_ok;
}

/**
 * @brief Read + parse the boot sector, transparently following MBR or GPT.
 *
 * @details Tries the BPB/VBR at the current base (LBA 0 for a superfloppy).
 * If that parse fails and sector 0 is an MBR, retargets the mount: for a
 * plain MBR to partition 0's start LBA, for a protective MBR (type 0xEE) to
 * the partition chosen from the GPT entry array (Basic Data preferred), and
 * re-parses. Leaves the original error in force when sector 0 is neither a
 * volume, an MBR, nor a usable GPT.
 *
 * @param[in,out] m Mount with backend bound and ``partition_base_lba == 0``.
 * @return Error code from the (re-)parse.
 * @retval k_ra_ok ``m`` holds the volume's BPB fields and base LBA.
 * @retval k_ra_err_* The backend read failed or no volume was found.
 * @pre ``m->partition_base_lba`` is 0 on entry.
 * @pre ``m->backend`` is bound with a valid ``read_block``.
 * @post On success ``m`` holds the volume's BPB fields and base LBA.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe; serialize mount operations.
 * @since 0.1.0
 */
static ra_err_t priv_read_boot_sector(ra_fs_mount_t* m)
{
  ra_err_t err = priv_read_sector(m, 0, s_scratch);
  if (err != k_ra_ok) {
    return err;
  }
  err = priv_parse_volume(m);
  if (err == k_ra_ok) {
    return k_ra_ok;
  }
  uint32_t base = priv_mbr_part0_lba(s_scratch);
  if (base == 0U) {
    return err;
  }
  if (s_scratch[k_mbr_off_part0_type] == (uint8_t)k_gpt_part_type_protective) {
    const ra_err_t gpt_err = priv_gpt_locate_volume(m, &base);
    if (gpt_err != k_ra_ok) {
      return gpt_err;
    }
  }
  m->partition_base_lba = base;
  err                   = priv_read_sector(m, 0, s_scratch);
  if (err != k_ra_ok) {
    return err;
  }
  return priv_parse_volume(m);
}

ra_err_t ra_fs_mount(const ra_fs_backend_t* backend, ra_fs_mount_t** out_handle)
{
  if (backend == nullptr || out_handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (backend->read_block == nullptr || backend->write_block == nullptr ||
      backend->get_capacity == nullptr) {
    return k_ra_err_invalid_arg;
  }
  ra_fs_mount_t* m = priv_alloc_mount_slot();
  if (m == nullptr) {
    return k_ra_err_no_mem;
  }
  m->backend            = *backend;
  m->partition_base_lba = 0U;
  ra_err_t err          = priv_read_boot_sector(m);
  if (err != k_ra_ok) {
    return err;
  }
  /* exFAT geometry is parsed directly in priv_exfat_parse; the FAT-BPB
   * geometry computation applies only to FAT12/16/32. */
  if (m->type != k_ra_fs_type_exfat) {
    err = priv_compute_geometry(m);
    if (err != k_ra_ok) {
      return err;
    }
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
  if (handle == nullptr) {
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
  if (f == nullptr) {
    return k_ra_err_no_mem;
  }
  f->mount         = handle;
  f->first_cluster = priv_entry_first_cluster(entry);
  f->cur_cluster   = f->first_cluster;
  f->size_bytes    = priv_rd32(&entry[k_dir_off_file_size]);
  f->dir_entry_lba = lba;
  f->dir_entry_idx = off;
  f->mode          = mode;
  f->no_fat_chain  = 0U;
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
  if (f == nullptr) {
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
  f->no_fat_chain  = 0U;
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
  if (handle == nullptr || path == nullptr || out_file == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  if (handle->type == k_ra_fs_type_exfat) {
    return priv_exfat_open(handle, path, mode, out_file);
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
  if (file == nullptr) {
    return k_ra_err_null_ptr;
  }
  file->in_use = 0;
  file->mount  = nullptr;
  return k_ra_ok;
}

/* =============================================================================
 * Public API: read / write / seek / tell / size
 * =============================================================================
 */

/**
 * @brief Walk `n` clusters forward from `start` along the FAT chain.
 *
 * @details Used by the read path to position to the cluster covering
 *          `file->offset`. Stops early on EOC.
 *
 * @param[in]  m     Mount providing FAT access.
 * @param[in]  start First cluster of the chain.
 * @param[in]  n     Number of clusters to walk.
 * @param[out] out   Receives the cluster reached (or last before EOC).
 *
 * @return Error code.
 * @retval k_ra_ok                Walked exactly `n` clusters.
 * @retval k_ra_err_invalid_state Hit EOC before walking `n` clusters.
 * @retval k_ra_err_*             Backend error.
 *
 * @pre `m` and `out` are non-NULL.
 * @pre `start` is a valid cluster number.
 * @post On success, `*out` is the destination cluster.
 * @post On EOC failure, `*out` is the last valid cluster reached.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
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
 *
 * @details Resolves the cluster covering `file->offset`, reads the
 *          containing sector, and copies the relevant slice into `buf`.
 *
 * @param[in,out] file      File handle providing offset and chain root.
 * @param[out]    buf       Destination of the byte slice.
 * @param[in]     remaining Maximum bytes the caller can accept.
 * @param[out]    out_take  Number of bytes actually copied.
 *
 * @return Error code.
 * @retval k_ra_ok    Slice copied.
 * @retval k_ra_err_* Backend or FAT error.
 *
 * @pre All pointers are non-NULL; file is in use.
 * @pre `file->offset < file->size_bytes`.
 * @post On success, `*out_take > 0` and `buf[0..*out_take]` populated.
 * @post `file->offset` is NOT advanced -- caller does that.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
static ra_err_t
priv_read_one_chunk(ra_fs_file_t* file, uint8_t* buf, uint32_t remaining, uint32_t* out_take)
{
  const uint32_t cluster_bytes   = file->mount->sectors_per_cluster * k_ra_fs_bytes_per_sector;
  const uint32_t cluster_idx_now = file->offset / cluster_bytes;
  uint32_t       target          = 0;
  /* exFAT contiguous files (NoFatChain) have no valid FAT chain: clusters are
   * sequential from the first. FAT files (no_fat_chain == 0) walk the chain. */
  if (file->no_fat_chain != 0U) {
    target = file->first_cluster + cluster_idx_now;
  } else {
    ra_err_t werr = priv_skip_clusters(file->mount, file->first_cluster, cluster_idx_now, &target);
    if (werr != k_ra_ok) {
      return werr;
    }
  }
  file->cur_cluster                = target;
  const uint32_t off_in_cluster    = file->offset % cluster_bytes;
  const uint32_t sector_in_cluster = off_in_cluster / k_ra_fs_bytes_per_sector;
  const uint32_t off_in_sector     = off_in_cluster % k_ra_fs_bytes_per_sector;
  const uint32_t lba = priv_cluster_to_lba(file->mount, file->cur_cluster) + sector_in_cluster;
  uint8_t        sec[k_ra_fs_bytes_per_sector] = {};
  const ra_err_t err                           = priv_read_sector(file->mount, lba, sec);
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

/**
 * @brief Read bytes from an open file.
 *
 * @details Loops on `priv_read_one_chunk`, advancing `file->offset`
 *          after each chunk. Stops at EOF or when `max_len` met.
 *
 * @param[in,out] file    Open file handle.
 * @param[out]    buf     Destination buffer.
 * @param[in]     max_len Maximum bytes to read.
 * @param[out]    got_len Bytes actually read.
 *
 * @return Error code.
 * @retval k_ra_ok                Read completed (possibly short at EOF).
 * @retval k_ra_err_null_ptr      Any pointer was NULL.
 * @retval k_ra_err_invalid_state File is not open.
 * @retval k_ra_err_*             Backend error.
 *
 * @pre `file`, `buf`, and `got_len` are non-NULL.
 * @pre File is in use.
 * @post On success, `*got_len` bytes were placed in `buf` and the
 *       file offset advanced by the same amount.
 * @post On failure, `*got_len` is 0.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_read(ra_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  if (file == nullptr || buf == nullptr || got_len == nullptr) {
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
 *
 * @details Combines `priv_alloc_cluster` with a `priv_fat_set` to the
 *          canonical EOC value. Used by the write path when the file
 *          chain needs to grow.
 *
 * @param[in]  m     Mount providing FAT access.
 * @param[out] out_c Receives the allocated cluster.
 *
 * @return Error code.
 * @retval k_ra_ok    Cluster allocated and marked EOC.
 * @retval k_ra_err_* Backend or FAT error.
 *
 * @pre `m` and `out_c` are non-NULL.
 * @pre Volume has free clusters.
 * @post On success, `*out_c` is in range and FAT entry = EOC.
 * @post On failure, FAT may have been partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
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
 *
 * @details Like `priv_skip_clusters` but extends the chain (with new
 *          EOC clusters) when EOC is encountered before reaching `idx`.
 *
 * @param[in]  m           Mount providing FAT access.
 * @param[in]  start       First cluster of the chain.
 * @param[in]  idx         Index to walk to.
 * @param[out] out_cluster Receives the cluster at index `idx`.
 *
 * @return Error code.
 * @retval k_ra_ok    Reached / created cluster at `idx`.
 * @retval k_ra_err_* Backend, FAT, or no-mem error.
 *
 * @pre `m` and `out_cluster` are non-NULL.
 * @pre `start` is a valid cluster.
 * @post On success, `*out_cluster` is the cluster at offset `idx`.
 * @post On failure, FAT may have been partially extended.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
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
 *
 * @details Read-modify-write of a single sector. Used by the write
 *          path so partial-sector updates do not destroy neighbouring
 *          file data.
 *
 * @param[in] m             Mount providing the backend.
 * @param[in] lba           Sector to update.
 * @param[in] off_in_sector Byte offset within the sector.
 * @param[in] src           Source bytes.
 * @param[in] put           Number of bytes to write.
 *
 * @return Error code.
 * @retval k_ra_ok    Sector updated.
 * @retval k_ra_err_* Backend read or write failure.
 *
 * @pre `m` and `src` are non-NULL.
 * @pre `off_in_sector + put <= k_ra_fs_bytes_per_sector`.
 * @post On success, the sector reflects the merged content.
 * @post On failure, the sector content is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
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
 *
 * @details Allocates the first cluster on demand, then walks/grows
 *          the chain as needed and forwards each sector slice through
 *          `priv_write_into_sector`.
 *
 * @param[in,out] file File handle providing chain root and offset.
 * @param[in]     buf  Source buffer.
 * @param[in]     len  Number of bytes to write.
 *
 * @return Error code.
 * @retval k_ra_ok    All `len` bytes written.
 * @retval k_ra_err_* Backend, FAT, or no-mem error.
 *
 * @pre All pointers are non-NULL; file is open for writing.
 * @pre `len > 0`.
 * @post On success, `file->offset` advanced by `len`; size grew if needed.
 * @post On failure, partial bytes may already be on disk.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
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

/**
 * @brief Write bytes to an open file.
 *
 * @details Forwards to `priv_write_stream`, then patches the on-disk
 *          dir entry with the updated first-cluster and file size.
 *
 * @param[in,out] file File handle.
 * @param[in]     buf  Source buffer.
 * @param[in]     len  Bytes to write.
 *
 * @return Error code.
 * @retval k_ra_ok                All bytes written.
 * @retval k_ra_err_null_ptr      `file` or `buf` was NULL.
 * @retval k_ra_err_invalid_state File not open or opened read-only.
 * @retval k_ra_err_*             Backend, FAT, or no-mem error.
 *
 * @pre `file` and `buf` are non-NULL.
 * @pre `file->mode != k_ra_fs_mode_read`.
 * @post On success, the file's dir entry on disk reflects new size.
 * @post On failure, partial bytes may already be on disk.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_write(ra_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  if (file == nullptr || buf == nullptr) {
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

ra_err_t
ra_fs_write_file(ra_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len)
{
  if (handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (data == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  if (handle->type == k_ra_fs_type_exfat) {
    return priv_exfat_create(handle, path, data, len);
  }
  ra_fs_file_t* f = nullptr;
  ra_err_t      e = ra_fs_open(handle, path, k_ra_fs_mode_write, &f);
  if (e != k_ra_ok) {
    return e;
  }
  e                   = ra_fs_write(f, data, len);
  const ra_err_t cerr = ra_fs_close(f);
  if (e != k_ra_ok) {
    return e;
  }
  return cerr;
}

/**
 * @brief Move the file's read/write cursor.
 *
 * @details Clamps the requested offset to the current file size; the
 *          driver does not implement sparse files.
 *
 * @param[in,out] file         Open file handle.
 * @param[in]     offset_bytes Desired cursor position.
 *
 * @return Error code.
 * @retval k_ra_ok                Cursor moved (possibly clamped).
 * @retval k_ra_err_null_ptr      `file` was NULL.
 * @retval k_ra_err_invalid_state File not open.
 *
 * @pre `file` is non-NULL.
 * @pre File is in use.
 * @post `file->offset == min(offset_bytes, file->size_bytes)`.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_seek(ra_fs_file_t* file, uint32_t offset_bytes)
{
  if (file == nullptr) {
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

/**
 * @brief Report the file's current cursor position.
 *
 * @details Reads `file->offset`.
 *
 * @param[in]  file       Open file handle.
 * @param[out] out_offset Receives the current offset in bytes.
 *
 * @return Error code.
 * @retval k_ra_ok                Position returned.
 * @retval k_ra_err_null_ptr      Any pointer was NULL.
 * @retval k_ra_err_invalid_state File not open.
 *
 * @pre `file` and `out_offset` are non-NULL.
 * @pre File is in use.
 * @post `*out_offset == file->offset`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_tell(const ra_fs_file_t* file, uint32_t* out_offset)
{
  if (file == nullptr || out_offset == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_offset = file->offset;
  return k_ra_ok;
}

/**
 * @brief Report the file's size in bytes.
 *
 * @details Reads `file->size_bytes`.
 *
 * @param[in]  file      Open file handle.
 * @param[out] out_bytes Receives the file size in bytes.
 *
 * @return Error code.
 * @retval k_ra_ok                Size returned.
 * @retval k_ra_err_null_ptr      Any pointer was NULL.
 * @retval k_ra_err_invalid_state File not open.
 *
 * @pre `file` and `out_bytes` are non-NULL.
 * @pre File is in use.
 * @post `*out_bytes == file->size_bytes`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_size(const ra_fs_file_t* file, uint32_t* out_bytes)
{
  if (file == nullptr || out_bytes == nullptr) {
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
 * @details Skips deleted (0xE5) and LFN (attr 0x0F) entries. Stops on
 *          the end-of-directory marker (0x00).
 *
 * @param[in] buf 512-byte sector buffer holding directory entries.
 * @param[in] cb  Caller-supplied per-entry callback.
 * @param[in] ctx Opaque pointer forwarded to `cb`.
 *
 * @return 1 if end-of-directory marker hit (caller can stop), 0 otherwise.
 * @retval 1  End-of-directory reached; caller should stop.
 * @retval 0  Sector exhausted without end-of-directory.
 *
 * @pre `buf` and `cb` are non-NULL.
 * @pre `buf` holds a sector loaded from disk.
 * @post No state modified by this function (callback may modify `ctx`).
 * @post `cb` invoked once per visible entry.
 *
 * @note Thread-safety inherited from `cb`.
 *
 * @since 0.1.0
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

/**
 * @brief Enumerate the entries in the volume root directory.
 *
 * @details Only the root path `"/"` is currently supported -- subdir
 *          traversal is not implemented.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   Must be `"/"`.
 * @param[in]     cb     Per-entry callback.
 * @param[in]     ctx    Opaque pointer forwarded to `cb`.
 *
 * @return Error code.
 * @retval k_ra_ok                  Directory walked successfully.
 * @retval k_ra_err_null_ptr        Any required pointer was NULL.
 * @retval k_ra_err_invalid_state   Mount not in use.
 * @retval k_ra_err_not_supported   `path` is not `"/"`.
 * @retval k_ra_err_*               Backend error.
 *
 * @pre `handle`, `path`, and `cb` are non-NULL.
 * @pre Mount is in use.
 * @post `cb` invoked once per visible root-directory entry.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_listdir(ra_fs_mount_t* handle, const char* path, ra_fs_listdir_cb_t cb, void* ctx)
{
  if (handle == nullptr || cb == nullptr || path == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  /* cppcheck-suppress redundantCondition -- explicit OR-chain documents intent. */
  if (path[0] != '/' || (path[0] == '/' && path[1] != '\0')) {
    return k_ra_err_not_supported;
  }
  if (handle->type == k_ra_fs_type_exfat) {
    return priv_exfat_listdir(handle, cb, ctx);
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

/**
 * @brief Delete a file from the root directory.
 *
 * @details Frees the cluster chain (if any) and marks the dir entry
 *          deleted by writing 0xE5 to the first byte of the name field.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated path; only flat root names supported.
 *
 * @return Error code.
 * @retval k_ra_ok                File deleted.
 * @retval k_ra_err_null_ptr      Any pointer was NULL.
 * @retval k_ra_err_invalid_state Mount not in use.
 * @retval k_ra_err_invalid_arg   Path is not a valid 8.3 name.
 * @retval k_ra_err_not_found     No such file.
 * @retval k_ra_err_*             Backend or FAT error.
 *
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use; no file handle currently references this entry.
 * @post On success, file's clusters are free and dir entry is deleted.
 * @post On failure, on-disk state may be partially updated.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra_err_t ra_fs_unlink(ra_fs_mount_t* handle, const char* path)
{
  if (handle == nullptr || path == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  if (handle->type == k_ra_fs_type_exfat) {
    return priv_exfat_unlink(handle, path);
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

/**
 * @brief Rename a root-level file on a FAT12/16/32 volume (in place).
 *
 * @details Rewrites the 11-byte packed 8.3 name inside the existing
 * directory entry; clusters, size, and attributes are untouched.
 *
 * @param[in] handle   Mounted FAT volume.
 * @param[in] old_path Existing 8.3 name.
 * @param[in] new_path Replacement 8.3 name (must not exist).
 * @return Error code.
 * @retval k_ra_ok                File renamed.
 * @retval k_ra_err_invalid_arg   A path does not convert to 8.3.
 * @retval k_ra_err_not_found     @p old_path does not exist.
 * @retval k_ra_err_exists        @p new_path already resolves.
 * @pre @p handle and both paths are non-NULL; mount is FAT.
 * @pre The file is not open.
 * @post @p new_path resolves to the same entry; @p old_path is gone.
 * @post No cluster or FAT state changes.
 * @note Root-directory namespace only (matches open/unlink).
 * @since 0.1.0
 */
static ra_err_t
priv_fat_rename(const ra_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  uint8_t old83[k_max_8_3_name] = {};
  uint8_t new83[k_max_8_3_name] = {};
  if (priv_path_to_83(old_path, old83) == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (priv_path_to_83(new_path, new83) == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t dup_lba                      = 0U;
  uint32_t dup_off                      = 0U;
  uint8_t  dup[k_ra_fs_dir_entry_bytes] = {};
  if (priv_dir_find(handle, new83, &dup_lba, &dup_off, dup) == k_ra_ok) {
    return k_ra_err_exists;
  }
  uint32_t lba                            = 0U;
  uint32_t off                            = 0U;
  uint8_t  entry[k_ra_fs_dir_entry_bytes] = {};
  ra_err_t err                            = priv_dir_find(handle, old83, &lba, &off, entry);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t sec[k_ra_fs_bytes_per_sector] = {};
  err                                   = priv_read_sector(handle, lba, sec);
  if (err != k_ra_ok) {
    return err;
  }
  priv_byte_copy(&sec[off], new83, (uint32_t)k_max_8_3_name);
  return priv_write_sector(handle, lba, sec);
}

/**
 * @brief Implementation of `ra_fs_rename()`.
 * @details See the public header for the documented contract; dispatches
 *          to the in-place FAT 8.3 rewrite or the exFAT entry-set rewrite.
 * @param[in] handle See header.
 * @param[in] old_path See header.
 * @param[in] new_path See header.
 * @return Result code.
 * @retval k_ra_ok File renamed.
 * @pre Module state is consistent.
 * @pre The volume is mounted.
 * @post On success the new name resolves to the same data.
 * @post On failure the directory is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_fs_rename(ra_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  if (handle == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (old_path == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (new_path == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra_err_invalid_state;
  }
  if (handle->type == k_ra_fs_type_exfat) {
    return priv_exfat_rename(handle, old_path, new_path);
  }
  return priv_fat_rename(handle, old_path, new_path);
}
