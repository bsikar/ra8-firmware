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

/** @brief Read a little-endian u32 from the RAM disk image. */
static uint32_t disk_rd32(uint32_t byte_off)
{
  return (uint32_t)s_disk.bytes[byte_off] |
         ((uint32_t)s_disk.bytes[byte_off + 1U] << (uint32_t)k_sp_shl_b1) |
         ((uint32_t)s_disk.bytes[byte_off + 2U] << (uint32_t)k_sp_shl_b2) |
         ((uint32_t)s_disk.bytes[byte_off + 3U] << (uint32_t)k_sp_shl_b3);
}

/** @brief Independently count free clusters by scanning the FAT in the image. */
static uint32_t indep_fat_free(const ra8_fs_mount_t* h)
{
  const uint32_t fat_byte = (h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_fmt_block_size;
  const uint32_t width =
    (h->type == k_ra8_fs_type_fat32) ? (uint32_t)k_sp_fat32_ent : (uint32_t)k_sp_fat16_ent;
  uint32_t free = 0U;
  for (uint32_t c = (uint32_t)k_sp_first_clus; c < (uint32_t)k_sp_first_clus + h->count_of_clusters;
       c++) {
    const uint32_t off = fat_byte + (c * width);
    uint32_t       v   = 0U;
    if (h->type == k_ra8_fs_type_fat32) {
      v = disk_rd32(off) & (uint32_t)k_sp_fat32_mask;
    } else {
      v = (uint32_t)s_disk.bytes[off] | ((uint32_t)s_disk.bytes[off + 1U] << 8);
    }
    if (v == 0U) {
      free++;
    }
  }
  return free;
}

/** @brief Absolute image byte offset of a cluster's first sector. */
static uint32_t clus_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t lba = h->partition_base_lba + h->first_data_lba +
                       ((uint64_t)(cluster - (uint32_t)k_sp_first_clus) * h->sectors_per_cluster);
  return lba * (uint32_t)k_fmt_block_size;
}

/** @brief Independently count free clusters by popcounting the exFAT bitmap. */
static uint32_t indep_exfat_free(const ra8_fs_mount_t* h)
{
  const uint32_t root     = clus_byte(h, h->root_cluster);
  uint32_t       bmp_clus = 0U;
  for (uint32_t e = 0U; e < (uint32_t)k_sp_entries_ps; e++) {
    const uint32_t eo = root + (e * (uint32_t)k_sp_entry_size);
    if (s_disk.bytes[eo] == (uint8_t)k_sp_bitmap_tag) {
      bmp_clus = disk_rd32(eo + (uint32_t)k_sp_de_clus);
      break;
    }
  }
  TEST_ASSERT(bmp_clus >= (uint32_t)k_sp_first_clus);
  const uint32_t bmp  = clus_byte(h, bmp_clus);
  uint32_t       used = 0U;
  for (uint32_t i = 0U; i < h->count_of_clusters; i++) {
    const uint8_t byte = s_disk.bytes[bmp + (i / (uint32_t)k_sp_bits_byte)];
    if (((byte >> (i % (uint32_t)k_sp_bits_byte)) & 1U) != 0U) {
      used++;
    }
  }
  return h->count_of_clusters - used;
}

/** @brief Independent free count for whichever filesystem @p h is. */
static uint32_t indep_free(const ra8_fs_mount_t* h)
{
  return (h->type == k_ra8_fs_type_exfat) ? indep_exfat_free(h) : indep_fat_free(h);
}

/** @brief Assert the struct's internal invariants hold. */
static void check_invariants(const ra8_fs_mount_t* h, const ra8_fs_space_t* sp)
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
 *          fresh-walk path and (on the second query) the cached path.
 */
static void space_cycle(uint32_t blocks, ra8_fs_type_t type, const char* label)
{
  alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_space_t before = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &before));
  check_invariants(h, &before);
  TEST_ASSERT_EQ(indep_free(h), before.free_clusters);
  TEST_ASSERT(before.free_clusters > 0U);

  /* Second query on the same mount: exercises the cached-count branch. */
  ra8_fs_space_t again = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &again));
  TEST_ASSERT_EQ(before.free_clusters, again.free_clusters);

  static uint8_t s_payload[k_sp_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_sp_payload; i++) {
    s_payload[i] = (uint8_t)((i * k_fs_pattern_stride) + k_fs_pattern_bias);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "DATA.BIN", s_payload, (uint32_t)k_sp_payload));

  ra8_fs_space_t after = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &after));
  check_invariants(h, &after);
  TEST_ASSERT(after.free_clusters < before.free_clusters);
  TEST_ASSERT_EQ(indep_free(h), after.free_clusters);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
}

/**
 * @test test_space_fat16
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT16 free-space happy path,
 * cross-checked against an independent FAT scan; the free-space null guard's
 * MC/DC is owned by test_space_null_guard)
 */
static void test_space_fat16(void)
{
  TEST_BEGIN("ra8_fs free_space: FAT16 vs an independent FAT scan");
  space_cycle((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "SPACE16");
  TEST_END("ra8_fs free_space: FAT16 vs an independent FAT scan");
}

/**
 * @test test_space_fat32
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT32 exercises the cached
 * FSInfo free count, cross-checked against an independent FAT scan)
 */
static void test_space_fat32(void)
{
  TEST_BEGIN("ra8_fs free_space: FAT32 (FSInfo cache) vs an independent FAT scan");
  space_cycle((uint32_t)k_fmt_blocks_fat32, k_ra8_fs_type_fat32, "SPACE32");
  TEST_END("ra8_fs free_space: FAT32 (FSInfo cache) vs an independent FAT scan");
}

/**
 * @test test_space_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT bitmap population count,
 * cross-checked against an independent bitmap popcount)
 */
static void test_space_exfat(void)
{
  TEST_BEGIN("ra8_fs free_space: exFAT vs an independent bitmap popcount");
  space_cycle((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "SPACEXF");
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
 * Also covers the not-in-use guard (an unmounted handle -> invalid_state).
 */
static void test_space_null_guard(void)
{
  TEST_BEGIN("ra8_fs free_space MC/DC: (handle||out) NULL pair + invalid_state");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
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
  free_volume();
  TEST_END("ra8_fs free_space MC/DC: (handle||out) NULL pair + invalid_state");
}

int32_t main(void)
{
  test_space_fat16();
  test_space_fat32();
  test_space_exfat();
  test_space_null_guard();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_space.c\n");
  return 0;
}
