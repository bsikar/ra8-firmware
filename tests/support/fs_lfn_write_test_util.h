/**
 * @file fs_lfn_write_test_util.h
 * @brief Shared fixture for the VFAT long-name write suites (#600).
 *
 * @details
 * Header-only fixture for `test_ra8_fs_lfn_write.c` (creating a long name) and
 * `test_ra8_fs_lfn_erase.c` (taking one away again), split apart when the two
 * halves together passed the 1000-line source cap. It layers on
 * `fs_fat_dir_test_util.h` -- the FAT16 volume builders and the three synthetic
 * block devices -- and adds what BOTH halves need in order to check the volume
 * rather than only the return code:
 *
 * - A RAW directory scanner (::scan_root()) written from the FAT specification
 *   rather than from the driver's own helpers, including its own copy of the
 *   8.3 checksum, so a driver-side mistake cannot agree with itself.
 * - The `listdir` collector both halves compare names through.
 * - ::write_and_verify(), the create-write-close-reopen-read round trip.
 *
 * Everything here has internal linkage, so each including executable gets its
 * own copy and the two suites stay fully independent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_dir_test_util.h"
#include "unity_minimal.h"

/**
 * @enum lw_dir_off_t
 * @brief Byte offsets inside a 32-byte FAT directory entry.
 *
 * @details The FAT specification's own names (section 6 "Directory Entry" and
 *          section 7 "Long Directory Entries"), so the raw scanning below can
 *          be checked against the document rather than against the driver.
 *
 * @invariant Every offset is inside one 32-byte entry.
 * @see count_orphan_slots()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_lw_off_name  = 0U,  /**< DIR_Name / LDIR_Ord.        */
  k_lw_off_attr  = 11U, /**< DIR_Attr / LDIR_Attr.       */
  k_lw_off_ntres = 12U, /**< DIR_NTRes (the case flags). */
  k_lw_off_csum  = 13U, /**< LDIR_Chksum.                */
  k_lw_name_len  = 11U, /**< DIR_Name field length.      */
  k_lw_entry     = 32U, /**< Directory entry size.       */
  k_lw_per_sec   = 16U, /**< 512 / 32.                   */
} lw_dir_off_t;

/**
 * @enum lw_val_t
 * @brief Marker bytes, sizes and loop bounds used by these cases.
 *
 * @invariant k_lw_scan_slots bounds every raw scan (NASA Power of 10 Rule 2).
 * @see scan_root()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lw_free_perm  = 0x00U,   /**< DIR_Name[0]: end of directory.            */
  k_lw_free_used  = 0xE5U,   /**< DIR_Name[0]: deleted slot.                */
  k_lw_attr_lfn   = 0x0FU,   /**< Long-name entry attribute.                */
  k_lw_ntres_base = 0x08U,   /**< DIR_NTRes: render the base lower case.    */
  k_lw_ntres_ext  = 0x10U,   /**< DIR_NTRes: render the extension likewise. */
  k_lw_csum_hibit = 0x80U,   /**< Rotate-in bit of the 8.3 checksum.        */
  k_lw_scan_slots = 4096U,   /**< Hard cap on any raw directory scan.       */
  k_lw_payload    = 96U,     /**< Bytes written into the test files.        */
  k_lw_seed       = 0x5AU,   /**< Fill seed for those payloads.             */
  k_lw_stride     = 11U,     /**< Stride of the fill generator.             */
  k_lw_names_cap  = 16U,     /**< Names a listdir collector will remember.  */
  k_lw_name_cap   = 260U,    /**< Bytes per remembered name.                */
  k_lw_fmt_blocks = 131072U, /**< 64 MiB: enough for a FAT16 format.        */
  k_lw_path_cap   = 320U,    /**< Scratch path buffer size.                 */
  k_lw_max_name   = 247U,    /**< Longest storable name: 19 groups of 13.   */
} lw_val_t;

/* ===========================================================================
 * Raw directory inspection -- written from the spec, not from the driver
 * ===========================================================================
 */

/**
 * @brief Byte offset in the RAM disk of the volume's root directory.
 *
 * @param[in] h Mounted FAT12/16 volume.
 *
 * @return Offset of the root directory's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The root region's start.
 *
 * @pre @p h is non-NULL and mounted with a fixed root region.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state is modified.
 * @post The result addresses the root region.
 *
 * @note Partition-adjusted: `priv_read_sector()` adds `partition_base_lba`, so
 *       a test poking `s_disk.bytes` has to add it too or it lands in the
 *       pre-partition gap (#568).
 * @since 0.1.0
 */
static inline uint32_t root_dir_byte(const ra8_fs_mount_t* h)
{
  return (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Address slot @p idx of the fixed root directory in the RAM disk.
 *
 * @param[in] h   Mounted FAT12/16 volume.
 * @param[in] idx Slot index from the start of the root region.
 *
 * @return Pointer to the 32-byte slot.
 * @retval non-NULL Always; the fixture's disk is allocated for the whole region.
 *
 * @pre @p h is mounted and `s_disk.bytes` is allocated.
 * @pre @p idx addresses a slot inside the root region.
 * @post No state is modified.
 * @post The returned pointer stays valid until free_vol().
 *
 * @note Not thread-safe (shares the fixture singleton).
 * @since 0.1.0
 */
static inline uint8_t* root_slot(const ra8_fs_mount_t* h, uint32_t idx)
{
  return &s_disk.bytes[root_dir_byte(h) + (idx * (uint32_t)k_lw_entry)];
}

/**
 * @brief The FAT 8.3 name checksum, reimplemented from the specification.
 *
 * @details Deliberately not `priv_sfn_checksum()`. These cases exist to catch a
 *          driver that binds a chain to the wrong entry, and a check that reuses
 *          the driver's own arithmetic to decide whether the driver's arithmetic
 *          is right cannot catch that.
 *
 * @param[in] name83 The 11-byte DIR_Name field.
 *
 * @return The folded checksum.
 * @retval 0..255 All values are possible.
 *
 * @pre @p name83 is non-NULL and addresses 11 readable bytes.
 * @pre @p name83 is the raw on-disk field, space-padded.
 * @post No state is modified.
 * @post The result matches LDIR_Chksum in every slot of that entry's chain.
 *
 * @note Pure function; trivially thread-safe.
 * @since 0.1.0
 */
static inline uint8_t spec_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_lw_name_len; i++) {
    sum = (uint8_t)((((sum & 1U) != 0U) ? (uint32_t)k_lw_csum_hibit : 0U) + (uint32_t)(sum >> 1U) +
                    (uint32_t)name83[i]);
  }
  return sum;
}

/**
 * @struct scan_result_t
 * @brief What one raw pass over a directory region saw.
 *
 * @invariant `orphans + chained` never exceeds `lfn_slots`.
 * @see scan_root()
 * @since 0.1.0
 */
typedef struct {
  uint32_t live;      /**< Live 8.3 entries (not deleted, not long-name).    */
  uint32_t lfn_slots; /**< Live attr-0x0F slots.                             */
  uint32_t orphans;   /**< Of those, ones no 8.3 entry claims.               */
  uint32_t chained;   /**< Of those, ones bound to the 8.3 entry after them. */
} scan_result_t;

/**
 * @brief Walk a directory region byte-for-byte and classify every slot.
 *
 * @details A long-name slot counts as CHAINED only when it is part of an
 *          unbroken run that ends at a live 8.3 entry whose ::spec_checksum()
 *          equals the run's `LDIR_Chksum`. Anything else -- a run broken by a
 *          deleted slot, a run whose checksum does not match the entry behind
 *          it, or a run that reaches the end of the directory -- is what
 *          `fsck.fat` calls an orphaned long file name part.
 *
 * @param[in]  base  First byte of the directory region in the RAM disk.
 * @param[in]  slots Number of 32-byte slots to inspect.
 * @param[out] out   Receives the tallies.
 *
 * @return Nothing.
 *
 * @pre @p base and @p out are non-NULL; the region holds @p slots entries.
 * @pre @p slots is at most ::k_lw_scan_slots.
 * @post Every live long-name slot is counted exactly once, as chained or orphan.
 * @post The RAM disk is not modified.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
static inline void scan_root(const uint8_t* base, uint32_t slots, scan_result_t* out)
{
  scan_result_t  r       = {};
  uint32_t       run     = 0U;
  uint8_t        run_sum = 0U;
  const uint32_t cap     = (slots < (uint32_t)k_lw_scan_slots) ? slots : (uint32_t)k_lw_scan_slots;
  for (uint32_t i = 0U; i < cap; i++) {
    const uint8_t* ent = &base[(size_t)i * (size_t)k_lw_entry];
    if (ent[k_lw_off_name] == (uint8_t)k_lw_free_perm) {
      break; /* end of directory: any pending run is orphaned */
    }
    if (ent[k_lw_off_name] == (uint8_t)k_lw_free_used) {
      r.orphans += run;
      run = 0U;
      continue;
    }
    if (ent[k_lw_off_attr] == (uint8_t)k_lw_attr_lfn) {
      r.lfn_slots++;
      if ((run != 0U) && (ent[k_lw_off_csum] != run_sum)) {
        r.orphans += run; /* a different chain starts here */
        run = 0U;
      }
      run_sum = ent[k_lw_off_csum];
      run++;
      continue;
    }
    r.live++;
    if (run != 0U) {
      if (spec_checksum(&ent[k_lw_off_name]) == run_sum) {
        r.chained += run;
      } else {
        r.orphans += run;
      }
      run = 0U;
    }
  }
  r.orphans += run;
  *out = r;
}

/**
 * @brief Tally the root directory of a mounted fixed-root volume.
 *
 * @param[in]  h   Mounted FAT12/16 volume.
 * @param[out] out Receives the tallies.
 *
 * @return Nothing.
 *
 * @pre @p h is mounted with a fixed root region; @p out is non-NULL.
 * @pre The fixture's RAM disk backs @p h.
 * @post @p out describes the root as it currently is on the RAM disk.
 * @post No state is modified.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
static inline void scan_root_of(const ra8_fs_mount_t* h, scan_result_t* out)
{
  scan_root(&s_disk.bytes[root_dir_byte(h)], h->root_entries, out);
}

/**
 * @brief Count how many orphaned long-name slots the root directory holds.
 *
 * @param[in] h Mounted FAT12/16 volume.
 *
 * @return Orphan count.
 * @retval 0 The directory is what `fsck.fat` would call clean.
 * @retval n That many long-name slots reference nothing.
 *
 * @pre @p h is mounted with a fixed root region.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state is modified.
 * @post The count is derived only from the on-disk bytes.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
static inline uint32_t count_orphan_slots(const ra8_fs_mount_t* h)
{
  scan_result_t r = {};
  scan_root_of(h, &r);
  return r.orphans;
}

/* ===========================================================================
 * listdir collection
 * ===========================================================================
 */

/**
 * @struct name_list_t
 * @brief Names collected from one `ra8_fs_listdir()` pass.
 * @invariant `count` never exceeds ::k_lw_names_cap.
 * @see collect_cb()
 * @since 0.1.0
 */
typedef struct {
  char     name[k_lw_names_cap][k_lw_name_cap]; /**< Reported names, in order. */
  uint32_t count;                               /**< How many were reported.   */
} name_list_t;

/**
 * @brief `ra8_fs_listdir()` callback that remembers every reported name.
 *
 * @param[in] name Entry name as the driver chose to report it.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a ::name_list_t.
 *
 * @return Nothing.
 *
 * @pre @p name and @p ctx are non-NULL.
 * @pre @p ctx points to a zero-initialised ::name_list_t.
 * @post At most ::k_lw_names_cap names are stored.
 * @post `count` reflects the number stored.
 *
 * @note Not thread-safe against the same @p ctx.
 * @since 0.1.0
 */
static inline void collect_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  name_list_t* l = (name_list_t*)ctx;
  if (l->count < (uint32_t)k_lw_names_cap) {
    (void)snprintf(l->name[l->count], (size_t)k_lw_name_cap, "%s", name);
    l->count++;
  }
}

/**
 * @brief Does a listdir pass over @p path report exactly @p want?
 *
 * @param[in] h    Mounted volume.
 * @param[in] path Directory to list.
 * @param[in] want The one name expected.
 *
 * @return Match flag.
 * @retval 1 Exactly one entry was reported and it equals @p want.
 * @retval 0 Anything else.
 *
 * @pre All pointers are non-NULL; @p h is mounted.
 * @pre @p path names a directory that exists.
 * @post No state is modified.
 * @post The listing was performed through the public API only.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline uint8_t listdir_is_exactly(ra8_fs_mount_t* h, const char* path, const char* want)
{
  name_list_t l = {};
  if (ra8_fs_listdir(h, path, collect_cb, &l) != k_ra8_ok) {
    return 0U;
  }
  if (l.count != 1U) {
    return 0U;
  }
  if (strcmp(l.name[0], want) == 0) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Fill @p buf with a deterministic pattern.
 *
 * @param[out] buf Destination buffer of at least @p len bytes.
 * @param[in]  len Number of bytes to write.
 *
 * @return Nothing.
 *
 * @pre @p buf is non-NULL and addresses @p len writable bytes.
 * @pre @p len is the exact buffer length.
 * @post Every byte of `buf[0..len-1]` is written.
 * @post No other state is modified.
 *
 * @note Trivially thread-safe (writes only through @p buf).
 * @since 0.1.0
 */
static inline void fill_payload(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_lw_stride) + (uint32_t)k_lw_seed);
  }
}

/**
 * @brief Create @p path, write a known payload, close, reopen and verify it.
 *
 * @details The round trip that matters: the name goes in through `open`, and
 *          the same name has to come back out of a fresh `open` on a volume
 *          that has been through a close since.
 *
 * @param[in,out] h    Mounted volume.
 * @param[in]     path Full path of the file to create.
 *
 * @return Nothing; assertion failures are reported by the harness.
 *
 * @pre @p h is mounted and @p path is non-NULL.
 * @pre No file is currently open on @p h.
 * @post @p path exists and holds ::k_lw_payload bytes of the known pattern.
 * @post Every handle opened here is closed again.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void write_and_verify(ra8_fs_mount_t* h, const char* path)
{
  uint8_t payload[k_lw_payload] = {};
  fill_payload(payload, (uint32_t)k_lw_payload);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, (uint32_t)k_lw_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  uint8_t  back[k_lw_payload] = {};
  uint32_t got                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_lw_payload, &got));
  TEST_ASSERT_EQ(k_lw_payload, got);
  TEST_ASSERT_EQ(0, memcmp(payload, back, (size_t)k_lw_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}
