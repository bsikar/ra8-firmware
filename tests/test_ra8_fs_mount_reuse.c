/**
 * @file test_ra8_fs_mount_reuse.c
 * @brief A reused mount slot carries no field of the previous volume (#684).
 *
 * @details
 * `k_ra8_fs_max_mounts` is 2 and the slots are reused across cards of different
 * filesystem types. `priv_mount_locked()` now zeroes the whole `ra8_fs_mount_t`
 * on claim, so a slot that served an exFAT volume cannot hand a later FAT mount
 * (or the reverse) a field the second parse does not populate. The FAT geometry
 * path and the exFAT parse populate DIFFERENT subsets of the struct, so this is
 * exactly where a stale value would hide.
 *
 * The tests reformat ONE 64 MiB RAM card between the two filesystem types and
 * mount it back into the same freed slot, asserting that every field reads the
 * new volume's value -- in particular that the fields one filesystem uses and
 * the other zeroes (FAT's `root_entries` / `first_root_lba` / `total_sectors`
 * vs exFAT's `root_cluster` / `count_of_clusters`) never survive the switch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum reuse_card_t
 * @brief The one card size that formats as both exFAT and FAT16.
 */
typedef enum : uint32_t {
  k_reuse_blocks = 131072U, /**< 64 MiB: exFAT-formattable and FAT16-formattable. */
} reuse_card_t;

/**
 * @brief Format @p type onto the shared card and mount it.
 *
 * @details Reformats the existing card in place (no realloc), so a mount here
 *          claims the slot the previous mount freed.
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_format_and_mount(ra8_fs_type_t type, const char* label)
{
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_NOT_NULL(h);
  TEST_ASSERT_EQ(type, h->type);
  return h;
}

/**
 * @test test_reuse_fat_then_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the reused-slot
 * zeroing end to end: a FAT mount that sets `root_entries` / `first_root_lba` /
 * `total_sectors` is unmounted, and a later exFAT mount into the same slot must
 * report those FAT-only fields as 0, not their FAT values) @brief Exercise the reuse fat then exfat filesystem operation. @details Runs the reuse fat then exfat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_reuse_fat_then_exfat(void)
{
  TEST_BEGIN("ra8_fs mount reuse: FAT16 slot reused for exFAT keeps no stale field");
  internal_alloc_garbage_card((uint32_t)k_reuse_blocks);

  ra8_fs_mount_t* fat = internal_format_and_mount(k_ra8_fs_type_fat16, "FIRSTFAT");
  /* FAT-only fields the exFAT parse does not use (it zeroes them). */
  TEST_ASSERT(fat->root_entries != 0U);
  TEST_ASSERT(fat->first_root_lba != 0U);
  TEST_ASSERT(fat->total_sectors != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(fat));

  ra8_fs_mount_t* exf = internal_format_and_mount(k_ra8_fs_type_exfat, "SECONDEXF");
  TEST_ASSERT_EQ(fat, exf); /* the very same slot was handed back */
  /* No FAT geometry survived: the exFAT parse leaves these zero, and the
   * slot-clear guarantees no earlier FAT value can leak through.
   * `total_sectors` is no longer zero on exFAT -- the parse records the VBR's
   * 64-bit VolumeLength (#683) -- so the stale-field proof for it is that the
   * value is the exFAT partition's own span, not the FAT card's total. */
  TEST_ASSERT_EQ(0U, exf->root_entries);
  TEST_ASSERT_EQ(0U, exf->first_root_lba);
  TEST_ASSERT_EQ(k_reuse_blocks - exf->partition_base_lba, exf->total_sectors);
  /* exFAT geometry is present and sane. */
  TEST_ASSERT(exf->root_cluster >= 2U);
  TEST_ASSERT(exf->count_of_clusters != 0U);
  TEST_ASSERT(exf->partition_base_lba != 0U); /* exFAT lives in an MBR partition */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(exf));

  internal_free_volume();
  TEST_END("ra8_fs mount reuse: FAT16 slot reused for exFAT keeps no stale field");
}

/**
 * @test test_reuse_exfat_then_fat
 * @par MC/DC:
 * (no compound decision unique to this case -- the reverse switch: an exFAT
 * mount that sets `root_cluster` / a non-zero `partition_base_lba` is unmounted,
 * and a later FAT16 mount into the same slot must report the FAT geometry, with
 * no exFAT partition base surviving) @brief Exercise the reuse exfat then fat filesystem operation. @details Runs the reuse exfat then fat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_reuse_exfat_then_fat(void)
{
  TEST_BEGIN("ra8_fs mount reuse: exFAT slot reused for FAT16 keeps no stale field");
  internal_alloc_garbage_card((uint32_t)k_reuse_blocks);

  ra8_fs_mount_t* exf      = internal_format_and_mount(k_ra8_fs_type_exfat, "EXFFIRST");
  const uint32_t  exf_root = exf->root_cluster;
  TEST_ASSERT(exf_root >= 2U);
  TEST_ASSERT(exf->partition_base_lba != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(exf));

  ra8_fs_mount_t* fat = internal_format_and_mount(k_ra8_fs_type_fat16, "FATSECOND");
  TEST_ASSERT_EQ(exf, fat); /* same slot */
  /* FAT16 is a superfloppy at LBA 0: the exFAT partition base must be gone. */
  TEST_ASSERT_EQ(0U, fat->partition_base_lba);
  /* FAT geometry present; the exFAT root cluster did not survive as geometry. */
  TEST_ASSERT(fat->root_entries != 0U);
  TEST_ASSERT(fat->first_root_lba != 0U);
  TEST_ASSERT(fat->total_sectors != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(fat));

  internal_free_volume();
  TEST_END("ra8_fs mount reuse: exFAT slot reused for FAT16 keeps no stale field");
}

int32_t main(void)
{
  internal_test_reuse_fat_then_exfat();
  internal_test_reuse_exfat_then_fat();
  return 0;
}
