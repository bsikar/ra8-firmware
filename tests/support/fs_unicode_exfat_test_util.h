/**
 * @file fs_unicode_exfat_test_util.h
 * @brief Shared fixtures for the exFAT non-ASCII name suites (#606).
 *
 * @details
 * The two exFAT unicode suites -- the round-trip/hash suite in
 * `test_ra8_fs_unicode_exfat.c` and the out-of-band `fsck.exfat` image suite
 * in `test_ra8_fs_unicode_exfat_cov.c` -- both reach for the same three
 * things: the specification field offsets in ::ux_val_t, the names spelled
 * out by hand as UTF-8 bytes and UTF-16 units, and a little-endian reader
 * over the fixture's RAM disk. They live here rather than in either suite so
 * neither file crosses the 1000-line cap and neither owns data the other
 * borrows -- the same reason `fs_exfat_upcase_test_util.h` factored out the
 * up-case expansion.
 *
 * The vectors carry `[[maybe_unused]]` because the image suite exercises only
 * the three names it dumps, not the hand-written unit arrays the hash suite
 * checks against.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "support/fs_fat_exfat_mutate_test_util.h"

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
[[maybe_unused]] static const char s_acc_u8[] =
  {'C', 'a', 'f', (char)(unsigned char)0xC3U, (char)(unsigned char)0xA9U, '.', 't', 'x', 't', '\0'};

/** @var s_acc_units
 *  @brief ::s_acc_u8 as UTF-16, written out by hand.
 *  @details Independent of the codec under test on purpose: it is the value the
 *           conversion is CHECKED against, so deriving it from the conversion
 *           would make the check vacuous.
 *  @note Read-only.
 *  @since 0.1.0
 */
[[maybe_unused]] static const uint16_t s_acc_units[] = {(uint16_t)'C',
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
[[maybe_unused]] static const char s_cjk_u8[] = {(char)(unsigned char)0xE4U,
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
[[maybe_unused]] static const uint16_t s_cjk_units[] =
  {0x4F60U, 0x597DU, (uint16_t)'.', (uint16_t)'t', (uint16_t)'x', (uint16_t)'t'};

/** @var s_emoji_u8
 *  @brief U+1F600 ".txt" -- four-byte UTF-8, SIX units (a surrogate pair + 4).
 *  @note Read-only.
 *  @since 0.1.0
 */
[[maybe_unused]] static const char s_emoji_u8[] = {(char)(unsigned char)0xF0U,
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
[[maybe_unused]] static const uint16_t s_emoji_units[] =
  {0xD83DU, 0xDE00U, (uint16_t)'.', (uint16_t)'t', (uint16_t)'x', (uint16_t)'t'};

/** @var s_acc_upper_u8
 *  @brief "CAF" U+00C9 ".TXT" -- the upper-case spelling of ::s_acc_u8.
 *  @note Read-only.
 *  @since 0.1.0
 */
[[maybe_unused]] static const char s_acc_upper_u8[] =
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
 */
static inline uint32_t disk_rd16(uint32_t off)
{
  return (uint32_t)s_disk.bytes[off] |
         ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_mut_shift_byte8);
}
