/**
 * @file test_ra8_fs_rmdir.c
 * @brief `ra8_fs_rmdir()` plus the FAT directory guards on unlink / open (#604).
 *
 * @details
 * Two halves of one defect, so one test file.
 *
 * The GUARD half: nothing in `ra8_fs` looked at `ATTR_DIRECTORY` outside path
 * traversal, so `ra8_fs_unlink("/LOGS")` freed the directory's own cluster chain
 * and orphaned every file inside it -- their clusters left allocated in the FAT
 * with nothing referencing them, which `fsck.fat` reports as lost clusters --
 * and `ra8_fs_open("/LOGS", write)` did the same damage while leaving the
 * directory entry in place. Read mode handed back a bogus zero-byte handle. All
 * three are now refused, and the tests check the volume afterwards rather than
 * only the return code: a guard that returns the right error while still
 * mutating the volume would pass a return-code-only test.
 *
 * The VERB half: `ra8_fs_rmdir()` itself -- empty and non-empty directories,
 * nested paths, the root, a file, a missing name, a non-8.3 name, a directory
 * entry with no cluster, and the reuse of the freed cluster afterwards. The
 * emptiness scan's three verdicts each get a case, including the two that no
 * ordinary API call can produce: a sector holding only deleted slots, and a
 * long-name remnant of the kind `ra8_fs_unlink()` leaves behind (it clears only
 * the 8.3 entry). Those are written into the directory cluster directly through
 * the fixture's RAM disk.
 *
 * I/O failures are swept rather than pinned to one injection count: the budget
 * is raised from zero until the call succeeds, which walks every read (and every
 * write) on the path and asserts each one is reported instead of ignored.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat_dir_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum rmdir_dir_off_t
 * @brief Byte offsets inside a 32-byte FAT directory entry.
 *
 * @details The names are the FAT specification's own (section 6, "Directory
 *          Entry"), so the raw patching below can be checked against the
 *          document instead of decoded from constants.
 *
 * @invariant Every offset is inside one 32-byte entry.
 * @see poke_dir_slot()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rd_off_name    = 0U,  /**< DIR_Name (11 bytes).               */
  k_rd_off_attr    = 11U, /**< DIR_Attr.                          */
  k_rd_off_clus_hi = 20U, /**< DIR_FstClusHI (u16).               */
  k_rd_off_clus_lo = 26U, /**< DIR_FstClusLO (u16).               */
  k_rd_name_len    = 11U, /**< DIR_Name field length.             */
  k_rd_entry_bytes = 32U, /**< Directory entry size.              */
  k_rd_per_sector  = 16U, /**< 512 / 32.                          */
  k_rd_dot_slots   = 2U,  /**< "." and ".." occupy slots 0 and 1. */
} rmdir_dir_off_t;

/**
 * @enum rmdir_val_t
 * @brief Marker bytes, payload sizes and loop bounds used by these tests.
 *
 * @invariant k_rd_io_budget_cap bounds every injection sweep (NASA Rule 2).
 * @see test_rmdir_read_failures()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rd_marker_deleted = 0xE5U, /**< DIR_Name[0] of a deleted slot.          */
  k_rd_attr_lfn       = 0x0FU, /**< Long-name entry attribute.              */
  k_rd_lfn_seq        = 0x41U, /**< Plausible LFN sequence byte (last, #1). */
  k_rd_cluster_first  = 2U,    /**< FAT cluster numbering starts at 2.      */
  k_rd_payload        = 64U,   /**< Bytes written into the test files.      */
  k_rd_seed           = 0x33U, /**< Fill seed for those payloads.           */
  k_rd_seed_stride    = 7U,    /**< Stride of the fill generator.           */
  k_rd_shift_16       = 16U,   /**< Shift folding FstClusHI into a cluster. */
  k_rd_io_budget_cap  = 64U,   /**< Upper bound on the I/O injection sweep. */
  k_rd_entries_none   = 0U,    /**< Expected listing size of an empty dir.  */
  k_rd_entries_one    = 1U,    /**< Expected listing size with one file.    */
} rmdir_val_t;

/* ---- fixture helpers ---------------------------------------------------- */

/**
 * @brief Fill @p buf with a deterministic pattern.
 *
 * @param[out] buf Destination buffer of at least @p len bytes.
 * @param[in]  len Number of bytes to write.
 *
 * @pre @p buf is non-NULL and addresses @p len writable bytes.
 * @pre @p len is the exact buffer length.
 * @post Every byte of @p buf[0..len-1] is written.
 * @post No other state is modified.
 *
 * @note Trivially thread-safe (writes only through @p buf).
 * @since 0.1.0 @details Implements the bounded fill payload fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_fill_payload(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_rd_seed_stride) + (uint32_t)k_rd_seed);
  }
}

/**
 * @brief Byte offset in the RAM disk of the FAT16 fixed root directory.
 *
 * @param[in] h Mounted FAT16 volume.
 *
 * @return Offset of the root directory's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The root region's start.
 *
 * @pre @p h is non-NULL and mounted as FAT16.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state is modified.
 * @post The result addresses the root region.
 *
 * @note Pure function of the mount geometry.
 * @since 0.1.0 @details Implements the bounded root dir byte fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_root_dir_byte(const ra8_fs_mount_t* h)
{
  /* Partition-adjusted: `priv_read_sector()` adds `partition_base_lba`, so a
   * test poking `s_disk.bytes` directly has to add it too or it lands in the
   * pre-partition gap (#568). It is 0 for this hand-built superfloppy, but
   * writing it out keeps the helper correct if the fixture ever grows an MBR. */
  return (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Byte offset in the RAM disk of a data cluster's first sector.
 *
 * @param[in] h       Mounted FAT16 volume.
 * @param[in] cluster Cluster number (>= 2).
 *
 * @return Offset of the cluster's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The cluster's start.
 *
 * @pre @p h is non-NULL and mounted; @p cluster >= 2.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state is modified.
 * @post The result addresses the data region.
 *
 * @note Pure function of the mount geometry.
 * @since 0.1.0 @details Implements the bounded cluster byte fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_cluster_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t lba =
    h->partition_base_lba + h->first_data_lba +
    ((uint64_t)(cluster - (uint32_t)k_rd_cluster_first) * h->sectors_per_cluster);
  return lba * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Find the root-directory slot holding the packed 8.3 name @p name11.
 *
 * @details Scans the fixed root region for an exact 11-byte name match rather
 *          than assuming which cluster the allocator handed out, so the raw
 *          patching below stays correct if allocation order ever changes.
 *
 * @param[in] h      Mounted FAT16 volume.
 * @param[in] name11 Packed 8.3 name, exactly 11 characters, space padded.
 *
 * @return Byte offset of the matching entry in `s_disk.bytes`.
 * @retval 0..byte_count The entry's offset (the test fails if absent).
 *
 * @pre @p h is non-NULL and mounted; @p name11 is 11 characters.
 * @pre The named entry exists in the root directory.
 * @post No state is modified.
 * @post A missing name fails the test rather than returning a bogus offset.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_root_entry_byte(const ra8_fs_mount_t* h, const char* name11)
{
  const uint32_t base = internal_root_dir_byte(h);
  for (uint32_t slot = 0U; slot < (uint32_t)k_rd_per_sector; slot++) {
    const uint32_t off = base + (slot * (uint32_t)k_rd_entry_bytes);
    if (memcmp(&s_disk.bytes[off + (uint32_t)k_rd_off_name], name11, (size_t)k_rd_name_len) == 0) {
      return off;
    }
  }
  TEST_FAIL_FMT("root entry %s not found", name11);
  return 0U;
}

/**
 * @brief Read the first cluster recorded in a directory entry.
 *
 * @param[in] entry_off Byte offset of the entry in `s_disk.bytes`.
 *
 * @return The entry's first cluster.
 * @retval 0..UINT32_MAX The folded FstClusHI:FstClusLO pair.
 *
 * @pre @p entry_off addresses a 32-byte directory entry.
 * @pre The fixture's RAM disk is allocated.
 * @post No state is modified.
 * @post Result depends only on the stored bytes.
 *
 * @note Pure read of the RAM disk.
 * @since 0.1.0 @details Implements the bounded entry cluster fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_entry_cluster(uint32_t entry_off)
{
  const uint8_t* e = &s_disk.bytes[entry_off];
  const uint32_t hi =
    (uint32_t)e[k_rd_off_clus_hi] | ((uint32_t)e[(uint32_t)k_rd_off_clus_hi + 1U] << 8U);
  const uint32_t lo =
    (uint32_t)e[k_rd_off_clus_lo] | ((uint32_t)e[(uint32_t)k_rd_off_clus_lo + 1U] << 8U);
  return (hi << (uint32_t)k_rd_shift_16) | lo;
}

/**
 * @brief Overwrite a slot in a directory cluster with raw bytes.
 *
 * @param[in] h       Mounted FAT16 volume.
 * @param[in] cluster The directory's first cluster.
 * @param[in] slot    Entry index within that cluster.
 * @param[in] name0   Byte to store in DIR_Name[0].
 * @param[in] attr    Byte to store in DIR_Attr.
 *
 * @pre @p h is non-NULL and mounted; @p cluster >= 2; @p slot < 16.
 * @pre The fixture's RAM disk backs @p h.
 * @post The addressed slot carries @p name0 and @p attr.
 * @post No other slot is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded poke dir slot fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_poke_dir_slot(const ra8_fs_mount_t* h,
                                                uint32_t              cluster,
                                                uint32_t              slot,
                                                uint8_t               name0,
                                                uint8_t               attr)
{
  const uint32_t off = internal_cluster_byte(h, cluster) + (slot * (uint32_t)k_rd_entry_bytes);
  s_disk.bytes[off + (uint32_t)k_rd_off_name] = name0;
  s_disk.bytes[off + (uint32_t)k_rd_off_attr] = attr;
}

/**
 * @brief Count the entries `ra8_fs_listdir()` reports for @p path.
 *
 * @param[in] h    Mounted volume.
 * @param[in] path Directory to enumerate.
 *
 * @return Number of entries reported.
 * @retval 0..UINT32_MAX The entry count.
 *
 * @pre @p h and @p path are non-NULL; the mount is in use.
 * @pre The listing succeeds (asserted).
 * @post No on-disk state is modified.
 * @post Synthetic "." / ".." entries are not counted.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded listing size fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_listing_size(ra8_fs_mount_t* h, const char* path)
{
  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, path, internal_count_cb, &count));
  return count;
}

/* ---- the verb ----------------------------------------------------------- */

/**
 * @test test_rmdir_basic
 * @brief An empty directory is removed and its cluster becomes reusable.
 *
 * @details The round trip that proves the removal was complete rather than
 *          cosmetic: the name stops resolving, the root listing loses it, and a
 *          second `mkdir` of the same name succeeds -- which it could not if the
 *          cluster were still marked in use.
 *
 * @par MC/DC:
 * Decision: `if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) == 0U)` in
 * `priv_rmdir_locate()` -- 1 condition.
 * - V1: attr = 0x10 -> F -> proceed with the removal (THIS test).
 * - V2: attr = 0x20 -> T -> k_ra8_err_invalid_arg (test_rmdir_refuses_a_file).
 *
 * @pre A hand-built FAT16 volume is mounted.
 * @post "/LOGS" no longer resolves and can be created again.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_basic(void)
{
  TEST_BEGIN("rmdir: an empty directory is removed and its cluster reused");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/LOGS"));
  TEST_ASSERT_EQ(k_rd_entries_none, internal_listing_size(h, "/"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/LOGS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: an empty directory is removed and its cluster reused");
}

/**
 * @test test_rmdir_nested
 * @brief A directory two levels deep is removed, then its parent.
 *
 * @details Exercises the parent-resolution path: the leaf is looked up inside a
 *          subdirectory rather than the root, and removing the child is what
 *          makes the parent removable in turn.
 *
 * @par MC/DC:
 * (no compound decision under test -- this varies the PATH DEPTH through the
 * already-covered single-condition guards)
 *
 * @pre A hand-built FAT16 volume is mounted.
 * @post Neither directory resolves and the root listing is empty.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_nested(void)
{
  TEST_BEGIN("rmdir: nested directory, then its parent");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS/SCIFI"));

  /* the parent is not empty while the child exists */
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/BOOKS/SCIFI"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_rd_entries_none, internal_listing_size(h, "/"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: nested directory, then its parent");
}

/**
 * @test test_rmdir_not_empty
 * @brief A directory holding a file is refused, and the file survives intact.
 *
 * @details The refusal must cost the volume nothing, so the file inside is read
 *          back afterwards. Unlinking it then makes the directory removable --
 *          which also covers the deleted-slot (0xE5) arm of the emptiness scan,
 *          since `ra8_fs_unlink()` leaves exactly that behind.
 *
 * @par MC/DC:
 * Decision: `if (empty == 0U)` in `priv_fat_rmdir()` -- 1 condition.
 * - V1: empty = 0 -> T -> k_ra8_err_not_empty (THIS test, first call).
 * - V2: empty = 1 -> F -> the chain is freed (THIS test, after the unlink).
 * Both arms are exercised on the same directory, so the only difference between
 * the two outcomes is the contents.
 *
 * @pre A hand-built FAT16 volume is mounted with "/LOGS/A.TXT" present.
 * @post The refused removal left the file readable; the retry succeeded.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_not_empty(void)
{
  TEST_BEGIN("rmdir: a non-empty directory is refused and left intact");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  uint8_t data[k_rd_payload] = {};
  internal_fill_payload(data, (uint32_t)k_rd_payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/LOGS/A.TXT", data, (uint32_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/LOGS"));

  /* the refusal changed nothing: the file is still there and still readable */
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/LOGS"));
  ra8_fs_file_t* f                 = nullptr;
  uint8_t        got[k_rd_payload] = {};
  uint32_t       got_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/LOGS/A.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)k_rd_payload, &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_rd_payload, got_len);
  TEST_ASSERT_EQ(0, memcmp(data, got, (size_t)k_rd_payload));

  /* emptying it makes it removable -- and leaves a 0xE5 slot behind */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/LOGS/A.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/LOGS"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: a non-empty directory is refused and left intact");
}

/**
 * @test test_rmdir_skips_long_name_remnant
 * @brief A stranded long-name slot does not make a directory un-removable.
 *
 * @details `ra8_fs_unlink()` marks only the 8.3 entry deleted, so a removed file
 *          with a long name leaves its whole 0x0F chain in place. Counting those
 *          as contents would make an emptied directory permanently unremovable,
 *          so the scan skips them. The remnant is written directly, because the
 *          driver has no long-name WRITE path to produce one.
 *
 * @par MC/DC:
 * Decision: `if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn)` in
 * `priv_rmdir_scan_sector()` -- 1 condition.
 * - V1: attr = 0x0F -> T -> slot skipped, scan continues (THIS test).
 * - V2: attr = 0x20 -> F -> slot counts as an occupant (test_rmdir_not_empty).
 *
 * @pre A hand-built FAT16 volume is mounted with an empty "/LOGS".
 * @post The directory is removed despite the remnant.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_skips_long_name_remnant(void)
{
  TEST_BEGIN("rmdir: a stranded long-name slot is not contents");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  const uint32_t cluster = internal_entry_cluster(internal_root_entry_byte(h, "LOGS       "));
  internal_poke_dir_slot(h,
                         cluster,
                         (uint32_t)k_rd_dot_slots,
                         (uint8_t)k_rd_lfn_seq,
                         (uint8_t)k_rd_attr_lfn);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/LOGS"));
  TEST_ASSERT_EQ(k_rd_entries_none, internal_listing_size(h, "/"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: a stranded long-name slot is not contents");
}

/**
 * @test test_rmdir_full_sector_of_deleted_slots
 * @brief A directory sector with no free marker left still reads as empty.
 *
 * @details Every slot in the cluster is either a dot entry or a deleted one, so
 *          the scan never meets the 0x00 end-of-directory marker and must fall
 *          through to the next sector -- which does not exist, because a
 *          `mkdir` directory owns exactly one cluster. That is the walk's
 *          chain-exhausted verdict, unreachable through any ordinary sequence of
 *          API calls.
 *
 * @par MC/DC:
 * Decision: `if (verdict == k_dir_scan_empty)` in `priv_dir_is_empty()` --
 * 1 condition.
 * - V1: verdict = k_dir_scan_more -> F -> advance to the next sector, chain
 *   ends, `*out_empty = 1` via the post-loop path (THIS test).
 * - V2: verdict = k_dir_scan_empty -> T -> report empty immediately
 *   (test_rmdir_basic).
 *
 * @pre A hand-built FAT16 volume is mounted with an empty "/LOGS".
 * @post The directory is removed.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_full_sector_of_deleted_slots(void)
{
  TEST_BEGIN("rmdir: a sector of deleted slots with no free marker reads as empty");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  const uint32_t cluster = internal_entry_cluster(internal_root_entry_byte(h, "LOGS       "));
  for (uint32_t slot = (uint32_t)k_rd_dot_slots; slot < (uint32_t)k_rd_per_sector; slot++) {
    internal_poke_dir_slot(h, cluster, slot, (uint8_t)k_rd_marker_deleted, 0U);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/LOGS"));
  TEST_ASSERT_EQ(k_rd_entries_none, internal_listing_size(h, "/"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: a sector of deleted slots with no free marker reads as empty");
}

/**
 * @test test_rmdir_refuses_a_file
 * @brief `rmdir` on a regular file is refused and the file survives.
 *
 * @details The mirror of the unlink guard: each verb refuses the other's kind.
 *
 * @par MC/DC:
 * Decision: `if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) == 0U)` in
 * `priv_rmdir_locate()` -- 1 condition.
 * - V1: attr = 0x20 -> T -> k_ra8_err_invalid_arg (THIS test).
 * - V2: attr = 0x10 -> F -> the removal proceeds (test_rmdir_basic).
 *
 * @pre A hand-built FAT16 volume is mounted with "/F.TXT" present.
 * @post The file still resolves and still lists.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_refuses_a_file(void)
{
  TEST_BEGIN("rmdir: a regular file is refused");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t data[k_rd_payload] = {};
  internal_fill_payload(data, (uint32_t)k_rd_payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/F.TXT", data, (uint32_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, "/F.TXT"));
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/F.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: a regular file is refused");
}

/**
 * @test test_rmdir_argument_guards
 * @brief NULL arguments, an unmounted handle, the root, and missing names.
 *
 * @details Each is an independent single-condition guard; grouping them keeps
 *          one volume build for the whole set. The root cases matter most: "/"
 *          and "" both resolve to a parent with an empty leaf, and removing the
 *          root would take the volume with it.
 *
 * @par MC/DC:
 * Decision: `if (leaf[0] == '\0')` in `priv_rmdir_locate()` -- 1 condition.
 * - V1: leaf = "" (path "/" or "") -> T -> k_ra8_err_invalid_arg (THIS test).
 * - V2: leaf = "LOGS"              -> F -> the lookup proceeds
 *   (test_rmdir_basic).
 *
 * @pre A hand-built FAT16 volume is mounted.
 * @post Every guarded call returned its documented code and changed nothing.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_argument_guards(void)
{
  TEST_BEGIN("rmdir: argument, root, and lookup guards");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rmdir(nullptr, "/LOGS"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rmdir(h, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, "/"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, ""));
  /* A name too long for 8.3 is no longer a bad name -- since #600 it is a name
   * that could exist -- so the honest answer is that it does not (#600). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/VERYLONGNAME.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/NOPE"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/NOPE/X"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_rmdir(h, "/LOGS"));
  internal_free_vol();
  TEST_END("rmdir: argument, root, and lookup guards");
}

/**
 * @test test_rmdir_directory_without_a_cluster
 * @brief A directory entry claiming cluster 0 is reported as corrupt.
 *
 * @details Every real directory owns a cluster -- it has to, for its own "." and
 *          ".." links. An entry that says otherwise is a corrupt volume, not an
 *          empty directory, and walking it would start the iterator outside the
 *          data region. The entry is corrupted directly because no API call can
 *          produce one.
 *
 * @par MC/DC:
 * Decision: `if (*out_cluster < k_cluster_first_data)` in `priv_rmdir_locate()`
 * -- 1 condition.
 * - V1: cluster = 0 -> T -> k_ra8_err_protocol_error (THIS test).
 * - V2: cluster = 2 -> F -> the emptiness scan runs (test_rmdir_basic).
 *
 * @pre A hand-built FAT16 volume is mounted with an empty "/LOGS".
 * @post The call reports k_ra8_err_protocol_error.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_directory_without_a_cluster(void)
{
  TEST_BEGIN("rmdir: a directory entry with no cluster is corrupt, not empty");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  const uint32_t entry_off                             = internal_root_entry_byte(h, "LOGS       ");
  s_disk.bytes[entry_off + (uint32_t)k_rd_off_clus_lo] = 0U;
  s_disk.bytes[entry_off + (uint32_t)k_rd_off_clus_lo + 1U] = 0U;
  s_disk.bytes[entry_off + (uint32_t)k_rd_off_clus_hi]      = 0U;
  s_disk.bytes[entry_off + (uint32_t)k_rd_off_clus_hi + 1U] = 0U;

  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_fs_rmdir(h, "/LOGS"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rmdir: a directory entry with no cluster is corrupt, not empty");
}

/**
 * @test test_rmdir_read_failures
 * @brief Every backend read on the rmdir path is reported, not ignored.
 *
 * @details Rather than pinning one injection count to one source line -- which
 *          rots the moment the call sequence changes -- the read budget is
 *          raised from zero until the removal succeeds. Every budget below that
 *          threshold must surface the backend's error, which walks each read
 *          error return on the path in turn.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after each read in `priv_dir_is_empty()` and
 * `priv_fat_rmdir()` -- 1 condition each.
 * - V1: err = k_ra8_err_hw_error -> T -> propagated (THIS test, low budgets).
 * - V2: err = k_ra8_ok           -> F -> the call continues (THIS test, at the
 *   threshold budget, and every other test here).
 *
 * @pre The injection sweep is bounded by k_rd_io_budget_cap.
 * @pre Each iteration starts from a freshly built volume.
 * @post Some budget succeeded, proving the sweep covered the whole path.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_read_failures(void)
{
  TEST_BEGIN("rmdir: every read failure on the path is reported");
  bool succeeded = false;
  for (uint32_t budget = 0U; budget < (uint32_t)k_rd_io_budget_cap; budget++) {
    internal_build_fat16_vol();
    ra8_fs_mount_t* h = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));

    const ra8_fs_backend_t saved = h->backend;
    internal_swap_to_inject(h, budget, 0U);
    const ra8_err_t e = ra8_fs_rmdir(h, "/LOGS");
    h->backend        = saved;

    if (e == k_ra8_ok) {
      succeeded = true;
    } else {
      TEST_ASSERT_EQ(k_ra8_err_hw_error, e);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
    internal_free_vol();
    if (succeeded) {
      break;
    }
  }
  TEST_ASSERT(succeeded);
  TEST_END("rmdir: every read failure on the path is reported");
}

/**
 * @test test_rmdir_write_failures
 * @brief Every backend write on the rmdir path is reported, not ignored.
 *
 * @details The write-side twin of test_rmdir_read_failures(): the FAT updates
 *          that free the chain and the parent-entry rewrite that retires the
 *          name each get their turn at failing.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after `priv_free_chain()` and around the
 * final entry rewrite in `priv_fat_rmdir()` -- 1 condition each.
 * - V1: err = k_ra8_err_hw_error -> T -> propagated (THIS test, low budgets).
 * - V2: err = k_ra8_ok           -> F -> the call continues (THIS test, at the
 *   threshold budget).
 *
 * @pre The injection sweep is bounded by k_rd_io_budget_cap.
 * @pre Each iteration starts from a freshly built volume.
 * @post Some budget succeeded, proving the sweep covered the whole path.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_write_failures(void)
{
  TEST_BEGIN("rmdir: every write failure on the path is reported");
  bool succeeded = false;
  for (uint32_t budget = 0U; budget < (uint32_t)k_rd_io_budget_cap; budget++) {
    internal_build_fat16_vol();
    ra8_fs_mount_t* h = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));

    const ra8_fs_backend_t saved = h->backend;
    internal_swap_to_wcount(h, budget);
    const ra8_err_t e = ra8_fs_rmdir(h, "/LOGS");
    h->backend        = saved;

    if (e == k_ra8_ok) {
      succeeded = true;
    } else {
      TEST_ASSERT_EQ(k_ra8_err_hw_error, e);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
    internal_free_vol();
    if (succeeded) {
      break;
    }
  }
  TEST_ASSERT(succeeded);
  TEST_END("rmdir: every write failure on the path is reported");
}

/* ---- the guards --------------------------------------------------------- */

/**
 * @test test_unlink_refuses_a_directory
 * @brief `ra8_fs_unlink()` on a directory is refused and orphans nothing.
 *
 * @details The headline defect: this call used to free the directory's chain and
 *          0xE5 its entry, leaving every file inside allocated and unreachable.
 *          The test therefore checks the CONTENTS afterwards, not just the
 *          return code -- reading the file back is what proves no chain was
 *          freed underneath it.
 *
 * @par MC/DC:
 * Decision: `if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) != 0U)` in
 * `ra8_fs_unlink()` -- 1 condition.
 * - V1: attr = 0x10 -> T -> k_ra8_err_invalid_arg (THIS test).
 * - V2: attr = 0x20 -> F -> the file is unlinked (test_rmdir_not_empty).
 *
 * @pre A hand-built FAT16 volume is mounted with "/LOGS/A.TXT" present.
 * @post "/LOGS" still lists its file and the file still reads back.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_unlink_refuses_a_directory(void)
{
  TEST_BEGIN("unlink: a directory is refused, its contents are not orphaned");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  uint8_t data[k_rd_payload] = {};
  internal_fill_payload(data, (uint32_t)k_rd_payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/LOGS/A.TXT", data, (uint32_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_unlink(h, "/LOGS"));

  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/"));
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/LOGS"));
  ra8_fs_file_t* f                 = nullptr;
  uint8_t        got[k_rd_payload] = {};
  uint32_t       got_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/LOGS/A.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)k_rd_payload, &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_rd_payload, got_len);
  TEST_ASSERT_EQ(0, memcmp(data, got, (size_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: a directory is refused, its contents are not orphaned");
}

/**
 * @test test_open_refuses_a_directory
 * @brief `ra8_fs_open()` refuses a directory in read, write, and append modes.
 *
 * @details Write mode is the destructive one -- it used to truncate the
 *          directory, freeing the chain holding its children and writing
 *          cluster 0 / size 0 into an entry still flagged ATTR_DIRECTORY. Read
 *          mode merely lied, handing back a zero-byte handle. All three are
 *          refused, and the directory's contents are re-read afterwards to prove
 *          no truncation slipped through.
 *
 * @par MC/DC:
 * Decision: `if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) != 0U)` in
 * `priv_open_existing()` -- 1 condition.
 * - V1: attr = 0x10 -> T -> k_ra8_err_invalid_arg, all three modes (THIS test).
 * - V2: attr = 0x20 -> F -> the handle is populated (every file open here).
 *
 * @pre A hand-built FAT16 volume is mounted with "/LOGS/A.TXT" present.
 * @post No mode opened the directory and the file inside is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_open_refuses_a_directory(void)
{
  TEST_BEGIN("open: a directory is refused in every mode, and not truncated");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  uint8_t data[k_rd_payload] = {};
  internal_fill_payload(data, (uint32_t)k_rd_payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/LOGS/A.TXT", data, (uint32_t)k_rd_payload));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/LOGS", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/LOGS", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/LOGS", k_ra8_fs_mode_append, &f));

  /* the write-mode refusal must not have truncated the directory */
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/LOGS"));
  uint8_t  got[k_rd_payload] = {};
  uint32_t got_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/LOGS/A.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)k_rd_payload, &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_rd_payload, got_len);
  TEST_ASSERT_EQ(0, memcmp(data, got, (size_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("open: a directory is refused in every mode, and not truncated");
}

/**
 * @test test_write_file_refuses_a_directory
 * @brief `ra8_fs_write_file()` over a FAT directory name is refused.
 *
 * @details On FAT this rides the `ra8_fs_open()` write-mode guard, so the
 *          provisioning helper inherits the protection rather than needing its
 *          own check. The directory's contents are verified afterwards.
 *
 * @par MC/DC:
 * (no compound decision under test -- the guard is the single-condition check
 * in `priv_open_existing()` covered by test_open_refuses_a_directory; this test
 * proves the refusal reaches the write_file caller)
 *
 * @pre A hand-built FAT16 volume is mounted with "/LOGS/A.TXT" present.
 * @post The directory still lists its file.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_write_file_refuses_a_directory(void)
{
  TEST_BEGIN("write_file: writing over a FAT directory name is refused");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  uint8_t data[k_rd_payload] = {};
  internal_fill_payload(data, (uint32_t)k_rd_payload);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/LOGS/A.TXT", data, (uint32_t)k_rd_payload));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_write_file(h, "/LOGS", data, (uint32_t)k_rd_payload));
  TEST_ASSERT_EQ(k_rd_entries_one, internal_listing_size(h, "/LOGS"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("write_file: writing over a FAT directory name is refused");
}

/**
 * @brief Run every rmdir and directory-guard test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts inside the assertion macros).
 *
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every test built and released its own volume.
 * @post A success banner is written to stderr.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_rmdir_basic();
  internal_test_rmdir_nested();
  internal_test_rmdir_not_empty();
  internal_test_rmdir_skips_long_name_remnant();
  internal_test_rmdir_full_sector_of_deleted_slots();
  internal_test_rmdir_refuses_a_file();
  internal_test_rmdir_argument_guards();
  internal_test_rmdir_directory_without_a_cluster();
  internal_test_rmdir_read_failures();
  internal_test_rmdir_write_failures();
  internal_test_unlink_refuses_a_directory();
  internal_test_open_refuses_a_directory();
  internal_test_write_file_refuses_a_directory();
  return 0;
}
