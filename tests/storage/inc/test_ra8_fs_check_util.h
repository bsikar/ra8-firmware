/**
 * @file test_ra8_fs_check_util.h
 * @brief Shared scaffolding for the `ra8_fs_check()` test suites (#610).
 *
 * @details
 * `ra8_fs_check` is exercised by two test binaries that share a RAM-image
 * corruptor toolkit: `test_ra8_fs_check.c` (the detection suite -- a clean
 * volume reports nothing, each hand-injected corruption is caught) and
 * `test_ra8_fs_check_mcdc.c` (the MC/DC suite -- one `test_mcdc_*` per compound
 * decision, each citing the decision it drives). The constants, the independent
 * on-disk readers/writers, and the format/mount/check/teardown helpers live here
 * so both binaries drive corruptions through the identical, driver-independent
 * path rather than a copy of it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_fs_check.h"
#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum chk_probe_t
 * @brief On-disk offsets and constants the independent corruptors use.
 */
typedef enum : uint32_t {
  k_chk_fat16_ent      = 2U,      /**< FAT16 entry width (bytes).            */
  k_chk_first_clus     = 2U,      /**< First data cluster number.            */
  k_chk_dir_ent        = 32U,     /**< Directory-entry size (bytes).         */
  k_chk_dir_ents_ps    = 16U,     /**< 512 / 32 dir entries per sector.      */
  k_chk_off_attr       = 11U,     /**< FAT DIR_Attr offset.                  */
  k_chk_off_fst_hi     = 20U,     /**< FAT DIR_FstClusHI offset.             */
  k_chk_off_fst_lo     = 26U,     /**< FAT DIR_FstClusLO offset.             */
  k_chk_exfat_eod      = 0x00U,   /**< exFAT end-of-directory marker.        */
  k_chk_exfat_bitmap   = 0x81U,   /**< exFAT allocation-bitmap entry type.   */
  k_chk_exfat_upcase   = 0x82U,   /**< exFAT up-case-table entry type.       */
  k_chk_strm_off_nlen  = 3U,      /**< exFAT Stream entry NameLength offset. */
  k_chk_exfat_name_cap = 64U,     /**< Driver name cap, in UTF-16 units.     */
  k_chk_nlen_over_cap  = 100U,    /**< A NameLength past that cap.           */
  k_chk_exfat_file     = 0x85U,   /**< exFAT File entry type.                */
  k_chk_off_file_secnt = 1U,      /**< exFAT File entry SecondaryCount.      */
  k_chk_off_set_csum   = 2U,      /**< exFAT File entry SetChecksum offset.  */
  k_chk_strm_off_hash  = 4U,      /**< exFAT Stream entry NameHash offset.   */
  k_chk_strm_off_clus  = 0x14U,   /**< exFAT Stream/system FirstCluster off. */
  k_chk_strm_off_dlen  = 0x18U,   /**< exFAT Stream/system DataLength off.   */
  k_chk_csum_hi_bit    = 0x8000U, /**< SetChecksum rotate-in wrap bit.       */
  k_chk_set_max_bytes  = 608U,    /**< 19-entry set upper bound (bytes).     */
  k_chk_payload_mc     = 2600U,   /**< Multi-cluster FAT payload (bytes).    */
  k_chk_payload_ex     = 6000U,   /**< Multi-cluster exFAT payload (bytes).  */
  k_chk_eoc_fat16      = 0xFFFFU, /**< A FAT16 end-of-chain value.           */
  k_chk_bad_fat_val    = 0x0001U, /**< Reserved cluster value 1 (illegal).   */
  k_chk_huge_lo        = 0xFFFEU, /**< A far-out-of-range cluster low word.  */
  k_chk_huge_hi        = 0x0FFFU, /**< ... and its high word.                */
  k_chk_hash_flip      = 0xAAAAU, /**< XOR mask that corrupts a NameHash.    */
  k_chk_csum_flip      = 0xFFFFU, /**< XOR mask that corrupts a SetChecksum. */
  k_chk_byte_full      = 0xFFU,   /**< A fully-set bitmap byte.              */
  k_chk_byte_bits      = 8U,      /**< Bits per bitmap byte.                 */
  k_chk_shl_b1         = 8U,      /**< LE byte-1 shift.                      */
  k_chk_shl_b2         = 16U,     /**< LE byte-2 shift.                      */
  k_chk_shl_b3         = 24U,     /**< LE byte-3 shift.                      */
  k_chk_pattern_stride = 31U,     /**< Payload generator stride (prime).     */
  k_chk_pattern_bias   = 7U,      /**< Payload generator bias.               */
  k_chk_exfat_fat_ent  = 4U,      /**< exFAT FAT entry width (bytes).        */
  k_chk_bound_clusters = 4U,      /**< Synthetic exact-bound chain length.   */
} chk_probe_t;

/**
 * @enum chk_probe2_t
 * @brief Further constants for the extended-coverage / MC/DC corruptors.
 */
typedef enum : uint32_t {
  k_chk_small          = 64U,         /**< Small single-cluster payload (bytes).    */
  k_chk_many           = 20U,         /**< Files that overflow one root cluster.    */
  k_chk_cluster_ex     = 4096U,       /**< One 4 KiB exFAT cluster (bytes).         */
  k_chk_fsi_off_num    = 48U,         /**< FAT32 BPB_FSInfo sector-number offset.   */
  k_chk_fsi_off_free   = 488U,        /**< FSInfo FSI_Free_Count offset.            */
  k_chk_defective      = 0xFFF7U,     /**< FAT16 defective-cluster marker.          */
  k_chk_huge_dlen      = 0x08000000U, /**< A DataLength that overruns the volume.   */
  k_chk_name_cap       = 16U,         /**< Scratch capacity for a generated name.   */
  k_chk_fsi_off_lead   = 0U,          /**< FSInfo FSI_LeadSig offset.               */
  k_chk_fsi_off_struct = 484U,        /**< FSInfo FSI_StrucSig offset.              */
  k_chk_fsi_off_trail  = 508U,        /**< FSInfo FSI_TrailSig offset.              */
  k_chk_wreck          = 0xDEADBEEFU, /**< A value that is no valid signature.      */
  k_chk_fsi_unknown    = 0xFFFFFFFFU, /**< FSInfo FSI_Free_Count "unknown".         */
  k_chk_fsi_far        = 999U,        /**< An FSInfo sector past the reserved area. */
  k_chk_secnt_over     = 200U,        /**< A SecondaryCount over the set maximum.   */
  k_chk_bmp_bytes      = 16384U,      /**< Scratch visited-bitmap size (bytes).     */
  k_chk_bit_mask       = 7U,          /**< Cluster index -> bit within its byte.    */
  k_chk_byte_shift     = 3U,          /**< log2(8): cluster index -> bitmap byte.   */
  k_chk_inuse_clear    = 0x7FU,       /**< Mask that clears the exFAT in-use bit.   */
  k_chk_exfat_deleted  = 0x01U,       /**< Deleted non-EOD directory entry type.    */
  k_chk_fat_deleted    = 0xE5U,       /**< Deleted FAT directory-entry lead byte.   */
  k_chk_scan_limit     = 65536U,      /**< Checker directory-entry ceiling.         */
  k_chk_report_fill    = 0xA5U,       /**< Sentinel byte for transactional output.  */
  k_chk_exfat_fat_eoc  = 0xFFFFFFFFU, /**< exFAT end-of-chain marker.               */
  k_chk_overflow_clus  = 123U,        /**< Sentinel cluster for worklist overflow.  */
} chk_probe2_t;

/** @brief One bit per data cluster of the largest test volume; ra8_fs_check scratch. */
static uint8_t s_chk_bitmap[k_chk_bmp_bytes];

/** @brief The multi-cluster payload written into each test volume. */
static uint8_t s_chk_payload[k_chk_payload_ex];

/* --------------------------------------------------------------------------
 * Independent RAM-image readers/writers (byte-addressed, absolute LBA*512).
 * -------------------------------------------------------------------------- */

/** @brief Read a little-endian u16 from the RAM disk image. @details Implements the bounded disk rd16 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_disk_rd16(uint32_t off)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  return (uint16_t)((uint16_t)s_disk.bytes[off] |
                    (uint16_t)((uint16_t)s_disk.bytes[off + 1U] << (uint16_t)k_chk_shl_b1));
}

/** @brief Write a little-endian u16 into the RAM disk image. @details Implements the bounded disk wr16 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @param[in] v Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_disk_wr16(uint32_t off, uint16_t v)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  s_disk.bytes[off]      = (uint8_t)(v & (uint16_t)k_chk_byte_full);
  s_disk.bytes[off + 1U] = (uint8_t)((v >> (uint16_t)k_chk_shl_b1) & (uint16_t)k_chk_byte_full);
}

/** @brief Read a little-endian u32 from the RAM disk image. @details Implements the bounded disk rd32 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_disk_rd32(uint32_t off)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  return (uint32_t)s_disk.bytes[off] |
         ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_chk_shl_b1) |
         ((uint32_t)s_disk.bytes[off + 2U] << (uint32_t)k_chk_shl_b2) |
         ((uint32_t)s_disk.bytes[off + 3U] << (uint32_t)k_chk_shl_b3);
}

/** @brief Write a little-endian u32 into the RAM disk image. @details Implements the bounded disk wr32 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @param[in] v Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_disk_wr32(uint32_t off, uint32_t v)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  s_disk.bytes[off]      = (uint8_t)(v & (uint32_t)k_chk_byte_full);
  s_disk.bytes[off + 1U] = (uint8_t)((v >> (uint32_t)k_chk_shl_b1) & (uint32_t)k_chk_byte_full);
  s_disk.bytes[off + 2U] = (uint8_t)((v >> (uint32_t)k_chk_shl_b2) & (uint32_t)k_chk_byte_full);
  s_disk.bytes[off + 3U] = (uint8_t)((v >> (uint32_t)k_chk_shl_b3) & (uint32_t)k_chk_byte_full);
}

/** @brief Absolute byte offset of a FAT16 entry in the (first) FAT. @details Implements the bounded fat16 off fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] clus Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_fat16_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  return ((h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_fmt_block_size) +
         ((uint64_t)clus * (uint32_t)k_chk_fat16_ent);
}

/** @brief Absolute byte offset of the root-dir entry named @p n11, or 0. @details Implements the bounded fat root entry fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] n11 Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_fat_root_entry(const ra8_fs_mount_t* h, const char* n11)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t base = (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_fmt_block_size;
  const uint32_t ents =
    (h->root_entries / (uint32_t)k_chk_dir_ents_ps) * (uint32_t)k_chk_dir_ents_ps;
  for (uint32_t i = 0U; i < ents; i++) {
    const uint32_t off = base + (i * (uint32_t)k_chk_dir_ent);
    if (s_disk.bytes[off] == 0U) {
      return 0U;
    }
    if (memcmp(&s_disk.bytes[off], n11, (size_t)k_chk_off_attr) == 0) {
      return off;
    }
  }
  return 0U;
}

/** @brief Absolute byte offset of an exFAT cluster's first sector. @details Implements the bounded exfat clus off fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] clus Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_exfat_clus_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  const uint32_t vlba =
    h->first_data_lba + ((uint64_t)(clus - (uint32_t)k_chk_first_clus) * h->sectors_per_cluster);
  return (h->partition_base_lba + vlba) * (uint32_t)k_fmt_block_size;
}

/** @brief Absolute byte offset of the first File(0x85) entry in the exFAT root, or 0. @details Implements the bounded exfat first file fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
[[maybe_unused]] RA8_INTERNAL static uint32_t internal_exfat_first_file(const ra8_fs_mount_t* h)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t base = internal_exfat_clus_off(h, h->root_cluster);
  const uint32_t nb   = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t o = 0U; o < nb; o += (uint32_t)k_chk_dir_ent) {
    const uint8_t t = s_disk.bytes[base + o];
    if (t == (uint8_t)k_chk_exfat_eod) {
      return 0U;
    }
    if (t == (uint8_t)k_chk_exfat_file) {
      return base + o;
    }
  }
  return 0U;
}

/** @brief First cluster of the exFAT allocation bitmap (the 0x81 root entry). @details Implements the bounded exfat bitmap clus fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_exfat_bitmap_clus(const ra8_fs_mount_t* h)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t base = internal_exfat_clus_off(h, h->root_cluster);
  const uint32_t nb   = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t o = 0U; o < nb; o += (uint32_t)k_chk_dir_ent) {
    if (s_disk.bytes[base + o] == (uint8_t)k_chk_exfat_bitmap) {
      return internal_disk_rd32(base + o + (uint32_t)k_chk_strm_off_clus);
    }
  }
  return 0U;
}

/** @brief exFAT SetChecksum, re-implemented independently of the driver. @details Implements the bounded chk set checksum fixture step using caller-owned state. @param[in] set Value required by this filesystem vector. @param[in] bytes Caller-supplied bounded extent or quantity. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_chk_set_checksum(const uint8_t* set, uint32_t bytes)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < bytes; i++) {
    if ((i == (uint32_t)k_chk_off_set_csum) || (i == (uint32_t)k_chk_off_set_csum + 1U)) {
      continue;
    }
    const uint16_t hi = ((cs & 1U) != 0U) ? (uint16_t)k_chk_csum_hi_bit : (uint16_t)0U;
    cs                = (uint16_t)(hi + (uint16_t)(cs >> 1) + (uint16_t)set[i]);
  }
  return cs;
}

/** @brief Re-seal an exFAT entry set's SetChecksum after an independent field edit. @details Implements the bounded exfat reseal fixture step using caller-owned state. @param[in] fe Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
[[maybe_unused]] RA8_INTERNAL static void internal_exfat_reseal(uint32_t fe)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t count = 1U + (uint32_t)s_disk.bytes[fe + (uint32_t)k_chk_off_file_secnt];
  uint8_t        set[k_chk_set_max_bytes] = {};
  const uint32_t bytes                    = count * (uint32_t)k_chk_dir_ent;
  memcpy(set, &s_disk.bytes[fe], (size_t)bytes);
  internal_disk_wr16(fe + (uint32_t)k_chk_off_set_csum, internal_chk_set_checksum(set, bytes));
}

/** @brief Set the first clear bit in the exFAT allocation bitmap (fabricates a lost cluster). @details Implements the bounded exfat set spare bit fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
[[maybe_unused]] RA8_INTERNAL static void internal_exfat_set_spare_bit(const ra8_fs_mount_t* h)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t bmp = internal_exfat_clus_off(h, internal_exfat_bitmap_clus(h));
  const uint32_t nb  = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  for (uint32_t by = 0U; by < nb; by++) {
    const uint8_t b = s_disk.bytes[bmp + by];
    if (b == (uint8_t)k_chk_byte_full) {
      continue;
    }
    for (uint32_t bit = 0U; bit < (uint32_t)k_chk_byte_bits; bit++) {
      if (((b >> bit) & 1U) == 0U) {
        s_disk.bytes[bmp + by] = (uint8_t)(b | (uint8_t)(1U << bit));
        return;
      }
    }
  }
}

/** @brief Clear the exFAT allocation-bitmap bit for @p cluster (fabricates a bitmap mismatch). @details Implements the bounded exfat clear ref bit fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] cluster Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
[[maybe_unused]] RA8_INTERNAL static void internal_exfat_clear_ref_bit(const ra8_fs_mount_t* h,
                                                                       uint32_t cluster)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t bmp = internal_exfat_clus_off(h, internal_exfat_bitmap_clus(h));
  const uint32_t idx = cluster - (uint32_t)k_chk_first_clus;
  s_disk.bytes[bmp + (idx >> (uint32_t)k_chk_byte_shift)] &=
    (uint8_t)~(uint8_t)(1U << (idx & (uint32_t)k_chk_bit_mask));
}

/**
 * @brief Compute an exFAT entry's absolute byte offset in FAT copy zero.
 * @param[in] h       Mounted exFAT volume.
 * @param[in] cluster Cluster whose FAT entry is requested.
 * @return Absolute byte offset in ::s_disk.
 * @retval 0..UINT32_MAX FAT entry offset.
 * @pre @p h is non-NULL and describes the 64 MiB test volume.
 * @pre @p cluster is within the mounted volume.
 * @post No state is modified.
 * @post The result addresses FAT copy zero.
 * @note Pure; trivially thread-safe within one test executable. @details Implements the bounded exfat fat off fixture step using caller-owned state. @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_exfat_fat_off(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint64_t offset =
    ((h->partition_base_lba + h->first_fat_lba) * (uint64_t)k_fmt_block_size) +
    ((uint64_t)cluster * (uint64_t)k_chk_exfat_fat_ent);
  return (uint32_t)offset;
}

/**
 * @brief Read one cluster's allocation-bitmap bit.
 * @param[in] h       Mounted exFAT volume.
 * @param[in] cluster Cluster whose allocation state is requested.
 * @return Whether the cluster is allocated.
 * @retval true  The allocation bit is set.
 * @retval false The allocation bit is clear.
 * @pre @p h is non-NULL and has an allocation-bitmap entry.
 * @pre @p cluster is in the data-cluster range.
 * @post No state is modified.
 * @post The result reflects ::s_disk.
 * @note Pure relative to the RAM image; not thread-safe with concurrent writes. @details Implements the bounded exfat cluster allocated fixture step using caller-owned state. @since 0.1.0
 */
RA8_INTERNAL
static bool internal_exfat_cluster_allocated(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t bmp = internal_exfat_clus_off(h, internal_exfat_bitmap_clus(h));
  const uint32_t idx = cluster - (uint32_t)k_chk_first_clus;
  return (s_disk.bytes[bmp + (idx >> (uint32_t)k_chk_byte_shift)] &
          (uint8_t)(1U << (idx & (uint32_t)k_chk_bit_mask))) != 0U;
}

/**
 * @brief Set one cluster's allocation-bitmap bit.
 * @param[in] h       Mounted exFAT volume.
 * @param[in] cluster Cluster to mark allocated.
 * @return Nothing.
 * @pre @p h is non-NULL and has an allocation-bitmap entry.
 * @pre @p cluster is in the data-cluster range.
 * @post The cluster's allocation bit is set.
 * @post No other allocation bit changes.
 * @note Not thread-safe; tests own ::s_disk exclusively. @details Implements the bounded exfat set allocated fixture step using caller-owned state. @since 0.1.0
 */
RA8_INTERNAL
static void internal_exfat_set_allocated(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t bmp = internal_exfat_clus_off(h, internal_exfat_bitmap_clus(h));
  const uint32_t idx = cluster - (uint32_t)k_chk_first_clus;
  s_disk.bytes[bmp + (idx >> (uint32_t)k_chk_byte_shift)] |=
    (uint8_t)(1U << (idx & (uint32_t)k_chk_bit_mask));
}

/**
 * @brief Find a contiguous clear allocation-bitmap run.
 * @param[in] h    Mounted exFAT volume.
 * @param[in] need Required consecutive clear clusters.
 * @return First cluster of the run.
 * @retval 2..UINT32_MAX A run of @p need clusters was found.
 * @pre @p h is non-NULL and @p need is non-zero.
 * @pre The 64 MiB fixture has at least @p need free clusters.
 * @post No state is modified.
 * @post Every cluster in the returned run is clear.
 * @note Bounded by `h->count_of_clusters`; not thread-safe with disk mutation. @details Implements the bounded exfat find free run fixture step using caller-owned state. @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_exfat_find_free_run(const ra8_fs_mount_t* h, uint32_t need)
{
  uint32_t start = 0U;
  uint32_t run   = 0U;
  for (uint32_t idx = 0U; idx < h->count_of_clusters; idx++) {
    const uint32_t cluster = idx + (uint32_t)k_chk_first_clus;
    if (internal_exfat_cluster_allocated(h, cluster)) {
      run = 0U;
      continue;
    }
    if (run == 0U) {
      start = cluster;
    }
    run++;
    if (run >= need) {
      return start;
    }
  }
  TEST_ASSERT(false);
  return 0U;
}

/**
 * @brief Extend the exFAT root to an exact count of non-EOD clusters.
 * @param[in] h        Mounted exFAT volume whose RAM image is mutated.
 * @param[in] clusters Total root-directory chain clusters to construct.
 * @return Nothing.
 * @pre @p h is non-NULL and @p clusters is at least one.
 * @pre The volume has `clusters - 1` consecutive free clusters.
 * @post The root FAT chain contains exactly @p clusters clusters and then EOC.
 * @post Every directory slot in that chain is non-EOD.
 * @note Not thread-safe; unmount before asking production code to observe edits. @details Implements the bounded exfat build bound directory fixture step using caller-owned state. @since 0.1.0
 */
RA8_INTERNAL
[[maybe_unused]] static void internal_exfat_build_bound_directory(const ra8_fs_mount_t* h,
                                                                  uint32_t              clusters)
{
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_fmt_block_size;
  const uint32_t root   = internal_exfat_clus_off(h, h->root_cluster);
  uint32_t       eod    = cbytes;
  for (uint32_t off = 0U; off < cbytes; off += (uint32_t)k_chk_dir_ent) {
    if (s_disk.bytes[root + off] == (uint8_t)k_chk_exfat_eod) {
      eod = off;
      break;
    }
  }
  TEST_ASSERT(eod < cbytes);
  for (uint32_t off = eod; off < cbytes; off += (uint32_t)k_chk_dir_ent) {
    memset(&s_disk.bytes[root + off], 0, (size_t)k_chk_dir_ent);
    s_disk.bytes[root + off] = (uint8_t)k_chk_exfat_deleted;
  }

  const uint32_t extras = clusters - 1U;
  if (extras == 0U) {
    internal_disk_wr32(internal_exfat_fat_off(h, h->root_cluster), (uint32_t)k_chk_exfat_fat_eoc);
    return;
  }
  const uint32_t first = internal_exfat_find_free_run(h, extras);
  internal_disk_wr32(internal_exfat_fat_off(h, h->root_cluster), first);
  for (uint32_t i = 0U; i < extras; i++) {
    const uint32_t cluster = first + i;
    const uint32_t next    = (i + 1U < extras) ? (cluster + 1U) : (uint32_t)k_chk_exfat_fat_eoc;
    internal_disk_wr32(internal_exfat_fat_off(h, cluster), next);
    internal_exfat_set_allocated(h, cluster);
    const uint32_t base = internal_exfat_clus_off(h, cluster);
    for (uint32_t off = 0U; off < cbytes; off += (uint32_t)k_chk_dir_ent) {
      memset(&s_disk.bytes[base + off], 0, (size_t)k_chk_dir_ent);
      s_disk.bytes[base + off] = (uint8_t)k_chk_exfat_deleted;
    }
  }
}

/* --------------------------------------------------------------------------
 * Format / mount / check / teardown.
 * -------------------------------------------------------------------------- */

/** @brief Format + mount a fresh card, writing a multi-cluster file when named. */
RA8_INTERNAL static ra8_fs_mount_t*
internal_chk_setup(uint32_t blocks, ra8_fs_type_t t, const char* name, uint32_t len)
{
  internal_alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = t;
  opts.label                = "CHK";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  if (name != nullptr) {
    for (uint32_t i = 0U; i < len; i++) {
      s_chk_payload[i] =
        (uint8_t)((i * (uint32_t)k_chk_pattern_stride) + (uint32_t)k_chk_pattern_bias);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, name, s_chk_payload, len));
  }
  return h;
}

/** @brief Run the check with the scratch bitmap into @p r. @details Implements the bounded chk run fixture step using caller-owned state. @param[in,out] h Value required by this filesystem vector. @param[in,out] r Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_chk_run(ra8_fs_mount_t* h, ra8_fs_check_report_t* r)
{
  *r = (ra8_fs_check_report_t){};
  return ra8_fs_check(h, s_chk_bitmap, (uint32_t)sizeof(s_chk_bitmap), r);
}

/** @brief Unmount and release the RAM card. @details Implements the bounded chk teardown fixture step using caller-owned state. @param[in,out] h Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_chk_teardown(ra8_fs_mount_t* h)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
}

/** @brief Populate a mounted exFAT volume with a subdir and a fragmented file. @details Implements the bounded exfat build rich fixture step using caller-owned state. @param[in,out] h Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
[[maybe_unused]] RA8_INTERNAL static void internal_exfat_build_rich(ra8_fs_mount_t* h)
{
  for (uint32_t i = 0U; i < (uint32_t)k_chk_cluster_ex; i++) {
    s_chk_payload[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "T.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ra8_fs_file_t* fa = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fa, s_chk_payload, (uint32_t)k_chk_cluster_ex));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "B.BIN", s_chk_payload, (uint32_t)k_chk_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fa, s_chk_payload, (uint32_t)k_chk_cluster_ex));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
}
