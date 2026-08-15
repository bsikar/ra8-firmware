/**
 * @file test_ra8_fs_space.c
 * @brief Free / used / total space query (`ra8_fs_free_space()`, #678).
 *
 * @details
 * Formats a RAM card as FAT16, FAT32 and exFAT, mounts it, and checks that
 * ::ra8_fs_free_space agrees with an INDEPENDENT count read straight off the
 * disk image -- a linear FAT scan for the FAT variants, a bitmap population
 * count for exFAT -- both before and after a multi-cluster file is written. A
 * matching return code would prove nothing; the point of the feature is the
 * number, so the number is cross-checked against the on-disk structures the
 * driver claims to summarise. The struct invariants (`used + free == total`,
 * bytes = clusters * unit) are asserted on every result.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum space_probe_t
 * @brief On-disk offsets the independent counters read.
 */
typedef enum : uint32_t {
  k_sp_fat16_ent  = 2U,          /**< FAT16 entry width (bytes).           */
  k_sp_fat32_ent  = 4U,          /**< FAT32 entry width (bytes).           */
  k_sp_fat32_mask = 0x0FFFFFFFU, /**< FAT32 28-bit cluster value mask.     */
  k_sp_first_clus = 2U,          /**< First data cluster.                  */
  k_sp_entry_size = 32U,         /**< exFAT directory entry size.          */
  k_sp_entries_ps = 16U,         /**< 512 / 32 dir entries per sector.     */
  k_sp_bitmap_tag = 0x81U,       /**< exFAT allocation-bitmap entry type.  */
  k_sp_de_clus    = 20U,         /**< exFAT system entry FirstCluster off. */
  k_sp_bits_byte  = 8U,          /**< Bits per bitmap byte.                */
  k_sp_payload    = 1300U,       /**< Multi-cluster file payload (bytes).  */
  k_sp_shl_b1     = 8U,          /**< LE byte-1 shift.                     */
  k_sp_shl_b2     = 16U,         /**< LE byte-2 shift.                     */
  k_sp_shl_b3     = 24U,         /**< LE byte-3 shift.                     */
} space_probe_t;

/** @brief Read a little-endian u32 from the RAM disk image. @details Implements the bounded disk rd32 fixture step using caller-owned state. @param[in] byte_off Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_disk_rd32(uint32_t byte_off)
{
  return (uint32_t)s_disk.bytes[byte_off] |
         ((uint32_t)s_disk.bytes[byte_off + 1U] << (uint32_t)k_sp_shl_b1) |
         ((uint32_t)s_disk.bytes[byte_off + 2U] << (uint32_t)k_sp_shl_b2) |
         ((uint32_t)s_disk.bytes[byte_off + 3U] << (uint32_t)k_sp_shl_b3);
}

/** @brief Independently count free clusters by scanning the FAT in the image. @details Implements the bounded indep fat free fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_indep_fat_free(const ra8_fs_mount_t* h)
{
  const uint32_t internal_fat_byte =
    (h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_fmt_block_size;
  const uint32_t width =
    (h->type == k_ra8_fs_type_fat32) ? (uint32_t)k_sp_fat32_ent : (uint32_t)k_sp_fat16_ent;
  uint32_t free = 0U;
  for (uint32_t c = (uint32_t)k_sp_first_clus; c < (uint32_t)k_sp_first_clus + h->count_of_clusters;
       c++) {
    const uint32_t off = internal_fat_byte + (c * width);
    uint32_t       v   = 0U;
    if (h->type == k_ra8_fs_type_fat32) {
      v = internal_disk_rd32(off) & (uint32_t)k_sp_fat32_mask;
    } else {
      v = (uint32_t)s_disk.bytes[off] | ((uint32_t)s_disk.bytes[off + 1U] << 8);
    }
    if (v == 0U) {
      free++;
    }
  }
  return free;
}

/** @brief Absolute image byte offset of a cluster's first sector. @details Implements the bounded clus byte fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] cluster Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_clus_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t lba = h->partition_base_lba + h->first_data_lba +
                       ((uint64_t)(cluster - (uint32_t)k_sp_first_clus) * h->sectors_per_cluster);
  return lba * (uint32_t)k_fmt_block_size;
}

/** @brief Independently count free clusters by popcounting the exFAT bitmap. @details Implements the bounded indep exfat free fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_indep_exfat_free(const ra8_fs_mount_t* h)
{
  const uint32_t root     = internal_clus_byte(h, h->root_cluster);
  uint32_t       bmp_clus = 0U;
  for (uint32_t e = 0U; e < (uint32_t)k_sp_entries_ps; e++) {
    const uint32_t eo = root + (e * (uint32_t)k_sp_entry_size);
    if (s_disk.bytes[eo] == (uint8_t)k_sp_bitmap_tag) {
      bmp_clus = internal_disk_rd32(eo + (uint32_t)k_sp_de_clus);
      break;
    }
  }
  TEST_ASSERT(bmp_clus >= (uint32_t)k_sp_first_clus);
  const uint32_t bmp  = internal_clus_byte(h, bmp_clus);
  uint32_t       used = 0U;
  for (uint32_t i = 0U; i < h->count_of_clusters; i++) {
    const uint8_t byte = s_disk.bytes[bmp + (i / (uint32_t)k_sp_bits_byte)];
    if (((byte >> (i % (uint32_t)k_sp_bits_byte)) & 1U) != 0U) {
      used++;
    }
  }
  return h->count_of_clusters - used;
}

/** @brief Independent free count for whichever filesystem @p h is. @details Implements the bounded indep free fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_indep_free(const ra8_fs_mount_t* h)
{
  return (h->type == k_ra8_fs_type_exfat) ? internal_indep_exfat_free(h)
                                          : internal_indep_fat_free(h);
}

/** @brief Assert the struct's internal invariants hold. @details Implements the bounded check invariants fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] sp Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_invariants(const ra8_fs_mount_t* h,
                                                   const ra8_fs_space_t* sp)
{
  TEST_ASSERT_EQ(h->count_of_clusters, sp->total_clusters);
  TEST_ASSERT_EQ(sp->total_clusters, sp->used_clusters + sp->free_clusters);
  TEST_ASSERT_EQ(h->sectors_per_cluster * (uint32_t)k_fmt_block_size, sp->bytes_per_cluster);
  TEST_ASSERT_EQ(sp->total_clusters * sp->bytes_per_cluster, sp->total_bytes);
  TEST_ASSERT_EQ(sp->free_clusters * sp->bytes_per_cluster, sp->free_bytes);
  TEST_ASSERT_EQ(sp->used_clusters * sp->bytes_per_cluster, sp->used_bytes);
}

/**
 * @brief Format @p type, mount, and cross-check free space before/after a write.
 *
 * @details Proves the reported free count equals an independent on-disk walk on
 *          a fresh volume, then that writing a multi-cluster file drops the free
 *          count and the new count still matches the walk -- exercising both the
 *          fresh-walk path and (on the second query) the cached path. @param[in] blocks Value required by this filesystem vector. @param[in] type Value required by this filesystem vector. @param[in] label Validated fixture path or name value. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void
internal_space_cycle(uint32_t blocks, ra8_fs_type_t type, const char* label)
{
  internal_alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_space_t before = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &before));
  internal_check_invariants(h, &before);
  TEST_ASSERT_EQ(internal_indep_free(h), before.free_clusters);
  TEST_ASSERT(before.free_clusters > 0U);

  /* Second query on the same mount: exercises the cached-count branch. */
  ra8_fs_space_t again = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &again));
  TEST_ASSERT_EQ(before.free_clusters, again.free_clusters);

  static uint8_t payload[k_sp_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_sp_payload; i++) {
    payload[i] = (uint8_t)((i * k_fs_pattern_stride) + k_fs_pattern_bias);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "DATA.BIN", payload, (uint32_t)k_sp_payload));

  ra8_fs_space_t after = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &after));
  internal_check_invariants(h, &after);
  TEST_ASSERT(after.free_clusters < before.free_clusters);
  TEST_ASSERT_EQ(internal_indep_free(h), after.free_clusters);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
}

/**
 * @test test_space_fat16
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT16 free-space happy path,
 * cross-checked against an independent FAT scan; the free-space null guard's
 * MC/DC is owned by test_space_null_guard) @brief Exercise the space fat16 filesystem operation. @details Runs the space fat16 vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_space_fat16(void)
{
  TEST_BEGIN("ra8_fs free_space: FAT16 vs an independent FAT scan");
  internal_space_cycle((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "SPACE16");
  TEST_END("ra8_fs free_space: FAT16 vs an independent FAT scan");
}

/**
 * @test test_space_fat32
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT32 exercises the cached
 * FSInfo free count, cross-checked against an independent FAT scan) @brief Exercise the space fat32 filesystem operation. @details Runs the space fat32 vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_space_fat32(void)
{
  TEST_BEGIN("ra8_fs free_space: FAT32 (FSInfo cache) vs an independent FAT scan");
  internal_space_cycle((uint32_t)k_fmt_blocks_fat32, k_ra8_fs_type_fat32, "SPACE32");
  TEST_END("ra8_fs free_space: FAT32 (FSInfo cache) vs an independent FAT scan");
}

/**
 * @test test_space_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT bitmap population count,
 * cross-checked against an independent bitmap popcount) @brief Exercise the space exfat filesystem operation. @details Runs the space exfat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_space_exfat(void)
{
  TEST_BEGIN("ra8_fs free_space: exFAT vs an independent bitmap popcount");
  internal_space_cycle((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "SPACEXF");
  TEST_END("ra8_fs free_space: exFAT vs an independent bitmap popcount");
}

/**
 * @test test_space_null_guard
 * @par MC/DC:
 * Decision: `if (handle == nullptr || out == nullptr)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_space.c@priv_space_locked`.
 * - V1 handle=valid, out=valid -> F (control: both false; the query runs).
 * - V2 handle=NULL,  out=valid -> C1=T -> T (varies handle only).
 * - V3 handle=valid, out=NULL  -> C1=F, C2=T -> T (varies out only).
 * V1+V2 prove handle independently flips the outcome; V1+V3 prove out does.
 * Also covers the not-in-use guard (an unmounted handle -> invalid_state). @brief Exercise the space null guard filesystem operation. @details Runs the space null guard vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_space_null_guard(void)
{
  TEST_BEGIN("ra8_fs free_space MC/DC: (handle||out) NULL pair + invalid_state");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "GUARD";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_space_t sp = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &sp));                 /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_free_space(nullptr, &sp)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_free_space(h, nullptr));   /* V3 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_free_space(h, &sp)); /* unmounted */
  internal_free_volume();
  TEST_END("ra8_fs free_space MC/DC: (handle||out) NULL pair + invalid_state");
}

int32_t main(void)
{
  internal_test_space_fat16();
  internal_test_space_fat32();
  internal_test_space_exfat();
  internal_test_space_null_guard();
  return 0;
}
