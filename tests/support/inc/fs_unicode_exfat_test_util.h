/**
 * @file fs_unicode_exfat_test_util.h
 * @brief Shared fixtures for the exFAT non-ASCII name suites (#606).
 *
 * @details
 * The exFAT unicode suites -- the round-trip/hash suite in
 * `test_ra8_fs_unicode_exfat.c`, the directory suite in
 * `test_ra8_fs_unicode_exfat_dirs.c` and the out-of-band image suite in
 * `test_ra8_fs_unicode_exfat_cov.c` -- all reach for the same four things: the
 * specification field offsets in ::ux_val_t, the names spelled out by hand as
 * UTF-8 bytes and UTF-16 units, a little-endian reader over the fixture's RAM
 * disk, and ::internal_unicode_dump_image below. They live here rather than in any one
 * suite so no file crosses the 1000-line cap and none owns data the others
 * borrow -- the same reason `fs_exfat_upcase_test_util.h` factored out the
 * up-case expansion.
 *
 *
 * @par Out-of-band evidence:
 * ::internal_unicode_dump_image writes the volume out as `<tag>.img` when
 * `RA8_EXFAT_DUMP_DIR` names a directory, the same hook and the same variable
 * `fs_exfat_stream_test_util.h` carries for the streaming suites. That is how
 * this suite's evidence stops being a claim about our own byte assertions and
 * becomes something a third party can re-derive -- `fsck.exfat`, or an
 * operating system asked to DISPLAY the names, which is the one question a
 * byte assertion cannot answer:
 * @code
 *   RA8_EXFAT_DUMP_DIR=/tmp/xuni ./test_ra8_fs_unicode_exfat_dirs
 *   fsck.exfat -n /tmp/xuni/unicode_showcase.img       # Linux
 *   hdiutil attach -imagekey diskimage-class=CRawDiskImage \
 *           /tmp/xuni/unicode_showcase.img             # macOS, then look
 * @endcode
 * With the variable unset -- which is how CI runs -- it costs one `getenv` per
 * scenario and touches no filesystem, so the suites keep no dependency on
 * exfatprogs or on anything else being installed.
 *
 * What is written is the PARTITION, not the whole RAM disk: `ra8_fs_format()`
 * lays exFAT down inside an MBR partition at 1 MiB, and a checker handed the
 * disk reads the MBR and says `Bad fs_name in boot sector`, which is it being
 * right. Hence the @p base_lba parameter, which the caller takes from the
 * mount handle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fs_fat_exfat_mutate_test_util.h"
#include "ra8_test_file.h"
#include "ra8_test_file_posix.h"
#include "unity_minimal.h"

/**
 * @enum ux_val_t
 * @brief Offsets, sizes and expected values for these cases.
 *
 * @details The directory-entry offsets restate the exFAT specification's own
 *          field positions, so the raw inspection below can be checked against
 *          the document rather than against the driver's header.
 *
 * @invariant `k_ux_upcase_csum` is the published checksum of the canonical
 *            Microsoft up-case table; it is an EXTERNAL reference, which is
 *            what makes asserting it worth anything.
 * @see test_upcase_table_is_the_canonical_one()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ux_path_cap      = 320U,        /**< Scratch path buffer.                        */
  k_ux_units_cap     = 64U,         /**< Units in a scratch name buffer.             */
  k_ux_bmp_units     = 65536U,      /**< Code units in the Basic Multilingual Plane. */
  k_ux_upc_off_csum  = 4U,          /**< Up-case entry: TableChecksum (32-bit).      */
  k_ux_strm_off_len  = 3U,          /**< Stream entry: NameLength (units).           */
  k_ux_strm_off_hsh  = 4U,          /**< Stream entry: NameHash (16-bit).            */
  k_ux_name_off      = 2U,          /**< Name entry: first UTF-16 unit.              */
  k_ux_per_entry     = 15U,         /**< UTF-16 units per Name entry.                */
  k_ux_type_upcase   = 0x82U,       /**< Up-case-table entry type byte.              */
  k_ux_run_tag       = 0xFFFFU,     /**< Compressed table: an identity run.          */
  k_ux_csum_hi_bit   = 0x8000U,     /**< Rotate-add wrap bit (16-bit).               */
  k_ux_upcase_csum   = 0xE619D30DU, /**< Published checksum of the MS table.         */
  k_ux_payload       = 64U,         /**< Bytes written into each test file.          */
  k_ux_scan_slots    = 64U,         /**< Root slots any raw scan here visits.        */
  k_ux_mapped_floor  = 800U,        /**< Non-vacuity floor: 874 units really map.    */
  k_ux_file_off_csum = 2U,          /**< File entry: SetChecksum (2 bytes).          */
  k_ux_csum_mask     = 0xFFFFU,     /**< Keep a rotate-add checksum 16-bit.          */
} ux_val_t;

/* ===========================================================================
 * The names, spelled out in bytes, and their units spelled out separately
 * ===========================================================================
 */

/** @var s_acc_u8
 *  @brief "Caf" U+00E9 ".txt" -- two-byte UTF-8, eight units, nine bytes.
 *  @details The unit and byte counts differ, which is the discrepancy the old
 *           writer collapsed by emitting one unit per byte.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_acc_u8[] =
  {'C', 'a', 'f', (char)(unsigned char)0xC3U, (char)(unsigned char)0xA9U, '.', 't', 'x', 't', '\0'};

/** @var s_acc_units
 *  @brief ::s_acc_u8 as UTF-16, written out by hand.
 *  @details Independent of the codec under test on purpose: it is the value the
 *           conversion is CHECKED against, so deriving it from the conversion
 *           would make the check vacuous.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const uint16_t s_acc_units[] = {(uint16_t)'C',
                                       (uint16_t)'a',
                                       (uint16_t)'f',
                                       0x00E9U,
                                       (uint16_t)'.',
                                       (uint16_t)'t',
                                       (uint16_t)'x',
                                       (uint16_t)'t'};

/** @var s_cjk_u8
 *  @brief U+4F60 U+597D ".txt" -- three-byte UTF-8, seven units, ten bytes.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_cjk_u8[] = {(char)(unsigned char)0xE4U,
                                (char)(unsigned char)0xBDU,
                                (char)(unsigned char)0xA0U,
                                (char)(unsigned char)0xE5U,
                                (char)(unsigned char)0xA5U,
                                (char)(unsigned char)0xBDU,
                                '.',
                                't',
                                'x',
                                't',
                                '\0'};

/** @var s_cjk_units
 *  @brief ::s_cjk_u8 as UTF-16, written out by hand.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const uint16_t s_cjk_units[] =
  {0x4F60U, 0x597DU, (uint16_t)'.', (uint16_t)'t', (uint16_t)'x', (uint16_t)'t'};

/** @var s_emoji_u8
 *  @brief U+1F600 ".txt" -- four-byte UTF-8, SIX units (a surrogate pair + 4).
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_emoji_u8[] = {(char)(unsigned char)0xF0U,
                                  (char)(unsigned char)0x9FU,
                                  (char)(unsigned char)0x98U,
                                  (char)(unsigned char)0x80U,
                                  '.',
                                  't',
                                  'x',
                                  't',
                                  '\0'};

/** @var s_emoji_units
 *  @brief ::s_emoji_u8 as UTF-16: the surrogate pair, then ".txt".
 *  @note Read-only.
 *  @since 0.1.0
 */
static const uint16_t s_emoji_units[] =
  {0xD83DU, 0xDE00U, (uint16_t)'.', (uint16_t)'t', (uint16_t)'x', (uint16_t)'t'};

/** @var s_acc_upper_u8
 *  @brief "CAF" U+00C9 ".TXT" -- the upper-case spelling of ::s_acc_u8.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_acc_upper_u8[] =
  {'C', 'A', 'F', (char)(unsigned char)0xC3U, (char)(unsigned char)0x89U, '.', 'T', 'X', 'T', '\0'};

/**
 * @brief Read a little-endian 16-bit value out of the RAM disk.
 *
 * @param[in] off Byte offset within `s_disk.bytes`.
 *
 * @return The value.
 * @retval 0..0xFFFF Whatever the two bytes hold.
 *
 * @pre `s_disk.bytes` is allocated and @p off + 1 is inside it.
 * @pre The caller wants the on-disk byte order, which is little-endian.
 * @post No state is modified.
 * @post Written here rather than reused from the driver, so the inspection is
 *       independent of the code it inspects.
 *
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static inline uint32_t internal_disk_rd16(uint32_t off)
{
  return (uint32_t)s_disk.bytes[off] |
         ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_mut_shift_byte8);
}

/**
 * @brief Write the volume to `$RA8_EXFAT_DUMP_DIR/<tag>.img`.
 *
 * @details The reproducible half of this suite's out-of-band evidence, and the
 *          only part of it that can answer "does a real operating system SHOW
 *          this name": every other assertion here is this codebase reading back
 *          bytes it wrote itself. A no-op unless the environment names a
 *          directory, so CI never touches the filesystem and the suites depend
 *          on no host tooling; a developer who wants the evidence sets the
 *          variable and gets one image per scenario, ready to hand to
 *          `fsck.exfat` or to mount.
 *
 *          The bytes written start at @p base_lba, so the file is a bare exFAT
 *          volume rather than the partitioned disk the fixture holds -- see the
 *          file header for why handing a checker the disk is a mistake.
 *
 * @param[in] tag      Scenario name; becomes the file's basename.
 * @param[in] base_lba The volume's partition start, from `partition_base_lba`
 *                     on the mount handle. Captured before an unmount when the
 *                     caller dumps afterwards.
 *
 * @return Nothing. A dump that cannot be written fails the test, because a
 *         silently skipped dump is worse than no dump at all.
 *
 * @pre `s_disk.bytes` holds a formatted volume.
 * @pre @p tag contains no path separators.
 * @post With the variable set, the image is on disk and closed.
 * @post With it unset, nothing is written and no state changes.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_unicode_dump_image(const char* tag, uint32_t base_lba)
{
  const char* dir = getenv("RA8_EXFAT_DUMP_DIR");
  if ((dir == nullptr) || (s_disk.bytes == nullptr)) {
    return;
  }
  char path[k_ux_path_cap] = {};
  (void)snprintf(path, sizeof(path), "%s/%s.img", dir, tag);
  const size_t                 base  = (size_t)base_lba * (size_t)k_mut_block_size;
  const size_t                 total = (size_t)s_disk.block_count * (size_t)k_mut_block_size;
  const ra8_test_file_result_t result =
    internal_test_file_replace(path, &s_disk.bytes[base], total - base);
  if (result.status != k_ra8_test_file_ok) {
    TEST_FAIL_FMT("short dump write to %s", path);
  }
}
