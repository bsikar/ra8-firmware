/*
 */
/**
 * @file test_ra8_fs_stat.c
 * @brief Tests for `ra8_fs_stat()` on FAT and exFAT volumes (#609).
 *
 * @details
 * The defect this closes is a wrong answer, not a missing one: `stat` used to
 * be `open(read)` + `size()` + `close()`, and since opening a directory
 * succeeds and a directory's `DIR_FileSize` is 0 by specification, every
 * folder came back as an existing zero-byte FILE. The attribute byte -- the
 * one field that could have told them apart -- was hardcoded to `archive`.
 *
 * So the assertions here are about telling things apart:
 *   - a file reports its real length and no directory bit,
 *   - a directory reports the directory bit and length 0,
 *   - the attribute byte is the entry's own: a read-only file patched on disk
 *     reports read-only, which a hardcoded `archive` cannot do,
 *   - a missing name is not-found rather than an existing empty file,
 *   - the volume root is a directory, on FAT and on exFAT alike,
 *   - `stat` consumes no file-table slot: it still answers with all four file
 *     handles open, which the old open-based implementation could not.
 *
 * The volumes are formatted through the public ``ra8_fs_format()`` over the
 * RAM card in ``tests/test_ra8_fs_format_fixture.h``. Timestamp assertions
 * prove the metadata survives the stat seam instead of stopping at the
 * on-disk writer.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "test_ra8_fs_format_fixture.h"
#include "unity_minimal.h"

/**
 * @enum stat_fixture_t
 * @brief Payload sizes, seeds, and the on-disk offsets the attr probe pokes.
 */
typedef enum : uint32_t {
  k_stat_payload_bytes = 300U,  /**< File length, deliberately not a round sector. */
  k_stat_seed          = 0x21U, /**< Fill seed for that payload.                   */
  k_stat_stride        = 13U,   /**< Fill stride for that payload.                 */
  k_stat_name_bytes    = 11U,   /**< Packed 8.3 name length in a dir entry.        */
  k_stat_attr_off      = 11U,   /**< MS FAT spec sec 6 "DIR_Attr" byte offset.     */
  k_stat_entry_bytes   = 32U,   /**< MS FAT spec sec 6 directory-entry size.       */
  k_stat_open_handles  = 4U,    /**< ::k_ra8_fs_max_files, the whole file table.   */
} stat_fixture_t;

/**
 * @enum stat_entry_off_t
 * @brief Byte offsets of the timestamp fields the packed-field probes poke.
 */
typedef enum : uint8_t {
  k_stat_off_crt_tenth = 13U, /**< MS FAT spec sec 6 "DIR_CrtTimeTenth".          */
  k_stat_off_crt_time  = 14U, /**< MS FAT spec sec 6 "DIR_CrtTime".               */
  k_stat_off_crt_date  = 16U, /**< MS FAT spec sec 6 "DIR_CrtDate".               */
  k_stat_off_acc_date  = 18U, /**< MS FAT spec sec 6 "DIR_LstAccDate".            */
  k_stat_off_wrt_time  = 22U, /**< MS FAT spec sec 6 "DIR_WrtTime".               */
  k_stat_off_wrt_date  = 24U, /**< MS FAT spec sec 6 "DIR_WrtDate".               */
  k_stat_off_ex_ctime  = 8U,  /**< exFAT spec sec 7.4.8 "CreateTimestamp".        */
  k_stat_off_ex_mtime  = 12U, /**< exFAT spec sec 7.4.9 "LastModifiedTimestamp".  */
  k_stat_off_ex_m10ms  = 21U, /**< exFAT spec sec 7.4.12 "LastModified10ms".      */
  k_stat_off_ex_cutc   = 22U, /**< exFAT spec sec 7.4.13 "CreateUtcOffset".       */
  k_stat_off_ex_mutc   = 23U, /**< exFAT spec sec 7.4.14 "LastModifiedUtcOffset". */
  k_stat_off_ex_autc   = 24U, /**< exFAT spec sec 7.4.15 "LastAccessedUtcOffset". */
  k_stat_off_ex_name   = 2U,  /**< exFAT spec sec 7.7.3 "FileName" first unit.    */
} stat_entry_off_t;

/**
 * @enum stat_exfat_set_t
 * @brief Entry-type bytes and slot indices of a minimal exFAT File entry set.
 */
typedef enum : uint8_t {
  k_stat_exfat_type_file   = 0x85U, /**< exFAT spec sec 7.4 File entry type byte.   */
  k_stat_exfat_type_stream = 0xC0U, /**< exFAT spec sec 7.6 Stream entry type byte. */
  k_stat_exfat_type_name   = 0xC1U, /**< exFAT spec sec 7.7 Name entry type byte.   */
  k_stat_exfat_set_entries = 3U,    /**< File + Stream + one Name entry.            */
  k_stat_exfat_stream_idx  = 1U,    /**< Stream entry index within that set.        */
  k_stat_exfat_name_idx    = 2U,    /**< First Name entry index within that set.    */
} stat_exfat_set_t;

/**
 * @enum stat_byte_t
 * @brief Byte positions and shifts of the little-endian on-disk field writers.
 */
typedef enum : uint8_t {
  k_stat_byte_0   = 0U,  /**< Least-significant byte of a packed field. */
  k_stat_byte_1   = 1U,  /**< Second byte of a packed field.            */
  k_stat_byte_2   = 2U,  /**< Third byte of a packed field.             */
  k_stat_byte_3   = 3U,  /**< Most-significant byte of a 32-bit field.  */
  k_stat_shift_8  = 8U,  /**< Shift to byte 1.                          */
  k_stat_shift_16 = 16U, /**< Shift to byte 2.                          */
  k_stat_shift_24 = 24U, /**< Shift to byte 3.                          */
} stat_byte_t;

/**
 * @enum stat_mask_t
 * @brief Masks used by the little-endian on-disk field writers.
 */
typedef enum : uint32_t {
  k_stat_mask_byte = 0xFFU, /**< Low-order byte of a wider packed value. */
} stat_mask_t;

/**
 * @enum stat_packed_t
 * @brief Packed FAT date/time words, each tripping exactly one range guard.
 */
typedef enum : uint16_t {
  k_stat_date_month_zero = 0x0005U, /**< 1980, month 0, day 5: only month==0 rejects.  */
  k_stat_date_month_13   = 0x01A1U, /**< 1980, month 13, day 1: only month>12 rejects. */
  k_stat_date_day_zero   = 0x0020U, /**< 1980, month 1, day 0: only day==0 rejects.    */
  k_stat_time_hour_24    = 0xC000U, /**< 24:00:00 -- only hour>23 rejects.             */
  k_stat_time_minute_60  = 0x0780U, /**< 00:60:00 -- only minute>59 rejects.           */
  k_stat_time_second_60  = 0x001EU, /**< 00:00:60 -- only second>59 rejects.           */
  k_stat_date_legal      = 0x58CFU, /**< 2024-06-15: a legal, non-epoch date.          */
  k_stat_time_legal      = 0x73D6U, /**< 14:30:44: a legal, non-epoch time.            */
  k_stat_time_legal_crt  = 0x000AU, /**< 00:00:20, so tenth 199 lands on second 21.    */
} stat_packed_t;

/**
 * @enum stat_tenth_t
 * @brief The two sides of the FAT creation 10-ms increment bound.
 */
typedef enum : uint8_t {
  k_stat_tenth_illegal = 200U, /**< One past the largest legal FAT increment. */
  k_stat_tenth_max     = 199U, /**< The largest legal FAT increment.          */
} stat_tenth_t;

/**
 * @enum stat_exfat_stamp_t
 * @brief Packed exFAT 32-bit timestamps written straight into a File entry.
 */
typedef enum : uint32_t {
  k_stat_exfat_stamp_month_zero = 0x00050000UL, /**< Month 0, day 5, time 00:00:00. */
  k_stat_exfat_stamp_legal      = 0x58CF73D6UL, /**< 2024-06-15 14:30:44.           */
} stat_exfat_stamp_t;

/**
 * @enum stat_utc_byte_t
 * @brief exFAT UtcOffset bytes spanning, bounding and overshooting the legal zone range.
 */
typedef enum : uint8_t {
  k_stat_utc_zero_valid = 0x80U, /**< OffsetValid, +0 steps: UTC+00:00.  */
  k_stat_utc_minus_8h   = 0xE0U, /**< OffsetValid, -32 steps: UTC-08:00. */
  k_stat_utc_below_span = 0xC0U, /**< OffsetValid, -64 steps: UTC-16:00. */
  k_stat_utc_above_span = 0xBFU, /**< OffsetValid, +63 steps: UTC+15:45. */
  k_stat_utc_span_lo    = 0xD0U, /**< OffsetValid, -48 steps: UTC-12:00. */
  k_stat_utc_span_hi    = 0xB8U, /**< OffsetValid, +56 steps: UTC+14:00. */
} stat_utc_byte_t;

/**
 * @enum stat_expect_t
 * @brief The civil fields the legal non-epoch vectors must decode to.
 */
typedef enum : uint16_t {
  k_stat_exp_year       = 2024U, /**< Year of ::k_stat_date_legal.       */
  k_stat_exp_month      = 6U,    /**< Month of ::k_stat_date_legal.      */
  k_stat_exp_day        = 15U,   /**< Day of ::k_stat_date_legal.        */
  k_stat_exp_hour       = 14U,   /**< Hour of ::k_stat_time_legal.       */
  k_stat_exp_minute     = 30U,   /**< Minute of ::k_stat_time_legal.     */
  k_stat_exp_second     = 44U,   /**< Second of ::k_stat_time_legal.     */
  k_stat_exp_csec       = 99U,   /**< Centisecond from a 199 tenth byte. */
  k_stat_exp_crt_second = 21U,   /**< 20 s plus the 199-tenth carry.     */
  k_stat_exp_mod_second = 45U,   /**< 44 s plus the 199-tenth carry.     */
} stat_expect_t;

/**
 * @enum stat_utc_expect_t
 * @brief The UTC offsets, in minutes, the exFAT vectors must decode to.
 */
typedef enum : int16_t {
  k_stat_exp_utc_minus_480 = -480, /**< Minutes for a -32-step offset. */
  k_stat_exp_utc_min       = -720, /**< Minutes at exactly UTC-12:00.  */
  k_stat_exp_utc_max       = 840,  /**< Minutes at exactly UTC+14:00.  */
} stat_utc_expect_t;

/** @brief Fill @p buf with a deterministic, non-repeating pattern. @details Implements the bounded fill fixture step using caller-owned state. @param[in,out] buf Caller-owned bounded byte storage. @param[in] len Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_fill(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)((i * (uint32_t)k_stat_stride) + (uint32_t)k_stat_seed);
  }
}

/** @brief Require the legal no-clock fallback civil fields, saying nothing about the zone. @details Implements the bounded expect epoch civil fixture step using caller-owned state. @param[in] stamp Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_epoch_civil(const ra8_fs_timestamp_t* stamp)
{
  TEST_ASSERT(stamp->valid);
  TEST_ASSERT_EQ(1980U, stamp->value.year);
  TEST_ASSERT_EQ(1U, stamp->value.month);
  TEST_ASSERT_EQ(1U, stamp->value.day);
  TEST_ASSERT_EQ(0U, stamp->value.hour);
  TEST_ASSERT_EQ(0U, stamp->value.minute);
  TEST_ASSERT_EQ(0U, stamp->value.second);
}

/** @brief Require the legal no-clock fallback and no invented UTC offset. @details Implements the bounded expect epoch fixture step using caller-owned state. @param[in] stamp Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_epoch(const ra8_fs_timestamp_t* stamp)
{
  internal_expect_epoch_civil(stamp);
  TEST_ASSERT(!stamp->utc_offset_valid);
}

/** @brief Require a rejected stamp: not valid, no zone, and every field zeroed. @details The decoder answers an illegal packed field by overwriting the whole timestamp with a zero value, so checking `valid` alone would still pass if a guard were dropped and a garbage year survived in `value`; asserting the zeroing is what makes a removed range guard fail. @param[in] stamp Decoded timestamp expected to have been rejected. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_invalid(const ra8_fs_timestamp_t* stamp)
{
  TEST_ASSERT(!stamp->valid);
  TEST_ASSERT(!stamp->utc_offset_valid);
  TEST_ASSERT_EQ(0U, stamp->value.year);
  TEST_ASSERT_EQ(0U, stamp->value.month);
  TEST_ASSERT_EQ(0U, stamp->value.day);
  TEST_ASSERT_EQ(0U, stamp->value.hour);
  TEST_ASSERT_EQ(0U, stamp->value.minute);
  TEST_ASSERT_EQ(0U, stamp->value.second);
  TEST_ASSERT_EQ(0U, stamp->value.centisecond);
  TEST_ASSERT_EQ(0, stamp->value.utc_offset_min);
}

/** @brief Locate the on-disk 8.3 directory entry whose packed name is @p name83. @details Walks the RAM card sector by sector, 32 bytes at a time; the whole card is walked because the formatter, not this fixture, decides where the root directory lands. Returning the entry rather than patching one field is what lets the timestamp probes rewrite arbitrary bytes. @param[in] name83 Packed 11-byte 8.3 name (no dot, space padded). @return Pointer into ::s_disk at the matching entry, or nullptr. @retval nullptr No entry on the card carries that packed name. @retval non-null The first matching 32-byte directory entry. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint8_t* internal_find_entry83(const char* name83)
{
  for (uint32_t lba = 0U; lba < s_disk.block_count; ++lba) {
    uint8_t* sec = &s_disk.bytes[(size_t)lba * (size_t)k_fmt_block_size];
    for (uint32_t off = 0U; off < (uint32_t)k_fmt_block_size; off += (uint32_t)k_stat_entry_bytes) {
      if (memcmp(&sec[off], name83, (size_t)k_stat_name_bytes) == 0) {
        return &sec[off];
      }
    }
  }
  return nullptr;
}

/**
 * @brief Set attribute bits directly in the on-disk 8.3 entry named @p name83.
 *
 * @details Walks the RAM card sector by sector looking for the packed 11-byte
 *          name and ORs @p bits into its DIR_Attr. The whole card is walked
 *          because the formatter, not this fixture, decides where the root
 *          directory lands. Poking the image rather
 *          than asking `ra8_fs` to do it is the point: the driver has no
 *          "set attributes" verb, and the question under test is whether
 *          `stat` REPORTS what is on the medium or invents it.
 *
 * @param[in] name83 Packed 11-byte 8.3 name (no dot, space padded).
 * @param[in] bits   Attribute bits to set.
 *
 * @return bool true when the entry was found and patched. @retval true The named condition holds. @retval false The condition does not hold. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static bool internal_patch_entry_attr(const char* name83, uint8_t bits)
{
  uint8_t* entry = internal_find_entry83(name83);
  if (entry == nullptr) {
    return false;
  }
  entry[k_stat_attr_off] |= bits;
  return true;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a file, a directory and a missing name
 * must be three distinguishable answers, which is exactly what the old
 * open-based stat could not produce) @brief Exercise the stat fat file dir missing filesystem operation. @details Runs the stat fat file dir missing vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_fat_file_dir_missing(void)
{
  TEST_BEGIN("stat on FAT: file vs directory vs missing");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "STAT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/DATA.BIN", payload, (uint32_t)k_stat_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));

  /* A file: real length, archive attribute, not a directory. */
  ra8_fs_stat_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/DATA.BIN", &file));
  TEST_ASSERT(!file.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, file.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_archive, (file.attr & (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT_EQ(0U, (file.attr & (uint8_t)k_ra8_fs_attr_directory));
  internal_expect_epoch(&file.created);
  internal_expect_epoch(&file.modified);
  internal_expect_epoch(&file.accessed);

  /* A directory: the bit that says so, and length 0 -- the case that used to
   * come back indistinguishable from an empty file. */
  ra8_fs_stat_t dir = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BOOKS", &dir));
  TEST_ASSERT(dir.is_directory);
  TEST_ASSERT_EQ(0U, dir.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_directory, (dir.attr & (uint8_t)k_ra8_fs_attr_directory));
  TEST_ASSERT(dir.first_cluster >= 2U);

  /* Missing: not-found, not an existing zero-byte file. */
  ra8_fs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/NOPE.BIN", &gone));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on FAT: file vs directory vs missing");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the reported attribute byte must be the
 * entry's own, which a hardcoded `archive` cannot be) @brief Exercise the stat reports the real attribute byte filesystem operation. @details Runs the stat reports the real attribute byte vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_reports_the_real_attribute_byte(void)
{
  TEST_BEGIN("stat on FAT: the attribute byte is the entry's, not a constant");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "ATTR"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/RO.BIN", payload, (uint32_t)k_stat_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  const uint8_t extra = (uint8_t)((uint8_t)k_ra8_fs_attr_read_only | (uint8_t)k_ra8_fs_attr_hidden |
                                  (uint8_t)k_ra8_fs_attr_system);
  TEST_ASSERT(internal_patch_entry_attr("RO      BIN", extra));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/RO.BIN", &st));
  TEST_ASSERT_EQ(extra, (st.attr & extra));
  TEST_ASSERT(!st.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, st.size_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on FAT: the attribute byte is the entry's, not a constant");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the root has no directory entry to read,
 * so it is answered from mount geometry, and nested paths resolve like open's) @brief Exercise the stat root and nested filesystem operation. @details Runs the stat root and nested vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_root_and_nested(void)
{
  TEST_BEGIN("stat on FAT: the root, and a path two components deep");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "ROOT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/", &root));
  TEST_ASSERT(root.is_directory);
  TEST_ASSERT_EQ(0U, root.size_bytes);
  TEST_ASSERT(!root.created.valid);
  TEST_ASSERT(!root.modified.valid);
  TEST_ASSERT(!root.accessed.valid);

  ra8_fs_stat_t empty_path = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "", &empty_path));
  TEST_ASSERT(empty_path.is_directory);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/BOOKS/A.BIN", payload, (uint32_t)k_stat_payload_bytes));

  ra8_fs_stat_t nested = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BOOKS/A.BIN", &nested));
  TEST_ASSERT(!nested.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, nested.size_bytes);

  ra8_fs_stat_t missing_parent = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/GONE/A.BIN", &missing_parent));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on FAT: the root, and a path two components deep");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a metadata query must not spend one of
 * the four file slots, which is what the open-based implementation did) @brief Exercise the stat consumes no file slot filesystem operation. @details Runs the stat consumes no file slot vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_consumes_no_file_slot(void)
{
  TEST_BEGIN("stat with every file handle already open");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "SLOTS"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/DATA.BIN", payload, (uint32_t)k_stat_payload_bytes));

  /* Fill the file table. */
  ra8_fs_file_t*           held[k_stat_open_handles] = {};
  static const char* const names[]                   = {"/H0.BIN", "/H1.BIN", "/H2.BIN", "/H3.BIN"};
  for (uint32_t i = 0U; i < (uint32_t)k_stat_open_handles; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, names[i], k_ra8_fs_mode_write, &held[i]));
  }
  ra8_fs_file_t* overflow = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "/DATA.BIN", k_ra8_fs_mode_read, &overflow));

  /* ...and stat still answers. */
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/DATA.BIN", &st));
  TEST_ASSERT_EQ(k_stat_payload_bytes, st.size_bytes);

  for (uint32_t i = 0U; i < (uint32_t)k_stat_open_handles; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(held[i]));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat with every file handle already open");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the argument guards and the
 * not-in-use mount guard are single-condition each) @brief Exercise the stat guards filesystem operation. @details Runs the stat guards vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_guards(void)
{
  TEST_BEGIN("stat argument and mount-state guards");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "GUARD"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(nullptr, "/", &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(h, nullptr, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(h, "/", nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_stat(h, "/", &st));
  internal_free_volume();
  TEST_END("stat argument and mount-state guards");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the exFAT lookup must answer file,
 * missing and root the same way the FAT one does) @brief Exercise the stat exfat filesystem operation. @details Runs the stat exfat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_exfat(void)
{
  TEST_BEGIN("stat on exFAT: file, missing, root");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_exfat, .label = "EXSTAT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "STORY.TXT", payload, (uint32_t)k_stat_payload_bytes));

  ra8_fs_stat_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/STORY.TXT", &file));
  TEST_ASSERT(!file.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, file.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_archive, (file.attr & (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT(file.first_cluster >= 2U);
  internal_expect_epoch(&file.created);
  internal_expect_epoch(&file.modified);
  internal_expect_epoch(&file.accessed);

  /* The leading slash is optional on exFAT, exactly as it is for open (#93). */
  ra8_fs_stat_t no_slash = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "STORY.TXT", &no_slash));
  TEST_ASSERT_EQ(file.size_bytes, no_slash.size_bytes);

  ra8_fs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/NOPE.TXT", &gone));

  ra8_fs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/", &root));
  TEST_ASSERT(root.is_directory);
  TEST_ASSERT_EQ(0U, root.size_bytes);
  TEST_ASSERT_EQ(h->root_cluster, root.first_cluster);
  TEST_ASSERT(!root.created.valid);
  TEST_ASSERT(!root.modified.valid);
  TEST_ASSERT(!root.accessed.valid);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on exFAT: file, missing, root");
}

/** @brief Store @p value into @p p as a little-endian 16-bit on-disk field. @details FAT stores every multi-byte field little-endian regardless of host byte order, so the probes below write the bytes explicitly rather than aliasing a wider type over the image. @param[out] p Two writable bytes inside the RAM card. @param[in] value Value to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_wr16(uint8_t* p, uint16_t value)
{
  p[k_stat_byte_0] = (uint8_t)(value & (uint16_t)k_stat_mask_byte);
  p[k_stat_byte_1] = (uint8_t)(value >> (uint16_t)k_stat_shift_8);
}

/** @brief Store @p value into @p p as a little-endian 32-bit on-disk field. @details The exFAT packed timestamp is one 32-bit little-endian word whose high half is the FAT-compatible date, so a byte-wise store is the only portable way to plant a date/time pair. @param[out] p Four writable bytes inside the RAM card. @param[in] value Value to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_wr32(uint8_t* p, uint32_t value)
{
  p[k_stat_byte_0] = (uint8_t)(value & (uint32_t)k_stat_mask_byte);
  p[k_stat_byte_1] = (uint8_t)((value >> (uint32_t)k_stat_shift_8) & (uint32_t)k_stat_mask_byte);
  p[k_stat_byte_2] = (uint8_t)((value >> (uint32_t)k_stat_shift_16) & (uint32_t)k_stat_mask_byte);
  p[k_stat_byte_3] = (uint8_t)((value >> (uint32_t)k_stat_shift_24) & (uint32_t)k_stat_mask_byte);
}

/** @brief Overwrite a 16-bit field of the 8.3 entry named @p name83. @details `ra8_fs` has no verb for writing an illegal date, and the question under test is what `stat` does with metadata some other implementation left behind, so bytes are planted directly. @param[in] name83 Packed 11-byte 8.3 name (no dot, space padded). @param[in] off Byte offset of the field within the 32-byte entry. @param[in] value Packed value to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_u16(const char* name83, uint8_t off, uint16_t value)
{
  uint8_t* entry = internal_find_entry83(name83);
  TEST_ASSERT_NOT_NULL(entry);
  internal_wr16(&entry[off], value);
}

/** @brief Overwrite a single byte of the 8.3 entry named @p name83. @details The FAT creation stamp's 10-ms increment is one byte, so it needs a narrower store. @param[in] name83 Packed 11-byte 8.3 name (no dot, space padded). @param[in] off Byte offset of the field within the 32-byte entry. @param[in] value Byte to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_u8(const char* name83, uint8_t off, uint8_t value)
{
  uint8_t* entry = internal_find_entry83(name83);
  TEST_ASSERT_NOT_NULL(entry);
  entry[off] = value;
}

/** @brief Locate the exFAT File entry whose name begins with @p first_char. @details exFAT spreads one name across a File entry, a Stream entry and one or more Name entries, so the card is scanned in 32-byte slots for that exact three-entry shape and the Name entry's first UTF-16 unit is matched; requiring all three type bytes is what keeps file payload bytes from being mistaken for a directory entry. @param[in] first_char First character of the file's name. @return Pointer into ::s_disk at the File entry, or nullptr. @retval nullptr No matching entry set is present on the card. @retval non-null The File entry of the first matching set. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint8_t* internal_find_exfat_file(char first_char)
{
  const size_t total = (size_t)s_disk.block_count * (size_t)k_fmt_block_size;
  const size_t slot  = (size_t)k_stat_entry_bytes;
  const size_t set   = slot * (size_t)k_stat_exfat_set_entries;
  for (size_t off = 0U; (off + set) <= total; off += slot) {
    uint8_t* file = &s_disk.bytes[off];
    if (file[k_stat_byte_0] != (uint8_t)k_stat_exfat_type_file) {
      continue;
    }
    if (file[slot * (size_t)k_stat_exfat_stream_idx] != (uint8_t)k_stat_exfat_type_stream) {
      continue;
    }
    const uint8_t* name = &file[slot * (size_t)k_stat_exfat_name_idx];
    if (name[k_stat_byte_0] != (uint8_t)k_stat_exfat_type_name) {
      continue;
    }
    if (name[k_stat_off_ex_name] != (uint8_t)first_char) {
      continue;
    }
    return file;
  }
  return nullptr;
}

/** @brief Overwrite a 32-bit field of the exFAT File entry named by @p first_char. @details Used to plant a packed exFAT timestamp; the SetChecksum is left stale on purpose because the lookup path does not consult it and recomputing it would test the fixture. @param[in] first_char First character of the file's name. @param[in] off Byte offset of the field within the 32-byte entry. @param[in] value Packed value to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_exfat_u32(char first_char, uint8_t off, uint32_t value)
{
  uint8_t* entry = internal_find_exfat_file(first_char);
  TEST_ASSERT_NOT_NULL(entry);
  internal_wr32(&entry[off], value);
}

/** @brief Overwrite a single byte of the exFAT File entry named by @p first_char. @details The UtcOffset and 10-ms increment fields are one byte each. @param[in] first_char First character of the file's name. @param[in] off Byte offset of the field within the 32-byte entry. @param[in] value Byte to store. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_exfat_u8(char first_char, uint8_t off, uint8_t value)
{
  uint8_t* entry = internal_find_exfat_file(first_char);
  TEST_ASSERT_NOT_NULL(entry);
  entry[off] = value;
}

/** @brief Plant one out-of-range FAT date/time field per guard, plus a legal one. @details Each vector is chosen so exactly ONE range guard can reject it -- the month-zero date keeps a legal day, the month-thirteen date keeps a legal day, and so on -- which is what makes the assertions detect a deleted guard instead of watching a neighbouring guard cover for it. `GOOD.BIN` is the must-stay-quiet direction: a legal non-epoch stamp with the largest legal 10-ms increment. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_fat_stamps(void)
{
  internal_patch_u16("BADA    BIN", (uint8_t)k_stat_off_crt_date, (uint16_t)k_stat_date_month_zero);
  internal_patch_u16("BADA    BIN", (uint8_t)k_stat_off_wrt_date, (uint16_t)k_stat_date_month_13);
  internal_patch_u16("BADA    BIN", (uint8_t)k_stat_off_acc_date, (uint16_t)k_stat_date_day_zero);
  internal_patch_u16("BADB    BIN", (uint8_t)k_stat_off_crt_time, (uint16_t)k_stat_time_hour_24);
  internal_patch_u16("BADB    BIN", (uint8_t)k_stat_off_wrt_time, (uint16_t)k_stat_time_minute_60);
  internal_patch_u16("BADC    BIN", (uint8_t)k_stat_off_crt_time, (uint16_t)k_stat_time_second_60);
  internal_patch_u8("BADD    BIN", (uint8_t)k_stat_off_crt_tenth, (uint8_t)k_stat_tenth_illegal);
  internal_patch_u16("GOOD    BIN", (uint8_t)k_stat_off_crt_date, (uint16_t)k_stat_date_legal);
  internal_patch_u16("GOOD    BIN", (uint8_t)k_stat_off_crt_time, (uint16_t)k_stat_time_legal_crt);
  internal_patch_u8("GOOD    BIN", (uint8_t)k_stat_off_crt_tenth, (uint8_t)k_stat_tenth_max);
  internal_patch_u16("GOOD    BIN", (uint8_t)k_stat_off_wrt_date, (uint16_t)k_stat_date_legal);
  internal_patch_u16("GOOD    BIN", (uint8_t)k_stat_off_wrt_time, (uint16_t)k_stat_time_legal);
  internal_patch_u16("GOOD    BIN", (uint8_t)k_stat_off_acc_date, (uint16_t)k_stat_date_legal);
}

/** @brief Check the calendar-date guards of the FAT stamp decoder. @details `BADA.BIN` carries one illegal date per stamp -- month 0, month 13 and day 0 -- and `BADB.BIN` carries hour 24 and minute 60; the untouched access stamp of `BADB.BIN` proves the rejection is per-field rather than per-entry. @param[in] h Mounted FAT volume holding the patched entries. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_fat_bad_dates(ra8_fs_mount_t* h)
{
  ra8_fs_stat_t bad_a = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BADA.BIN", &bad_a));
  internal_expect_invalid(&bad_a.created);  /* month == 0 */
  internal_expect_invalid(&bad_a.modified); /* month > 12 */
  internal_expect_invalid(&bad_a.accessed); /* day == 0   */

  ra8_fs_stat_t bad_b = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BADB.BIN", &bad_b));
  internal_expect_invalid(&bad_b.created);  /* hour > 23   */
  internal_expect_invalid(&bad_b.modified); /* minute > 59 */
  internal_expect_epoch(&bad_b.accessed);   /* untouched   */
}

/** @brief Check the second-overflow and 10-ms-increment guards. @details `BADC.BIN` carries a 60-second creation time -- reachable because FAT stores whole two-second units in five bits -- and `BADD.BIN` carries a 200 creation increment, one past the largest legal value; both leave the other two stamps at the epoch fallback. @param[in] h Mounted FAT volume holding the patched entries. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_fat_bad_times(ra8_fs_mount_t* h)
{
  ra8_fs_stat_t bad_c = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BADC.BIN", &bad_c));
  internal_expect_invalid(&bad_c.created); /* second > 59 */
  internal_expect_epoch(&bad_c.modified);
  internal_expect_epoch(&bad_c.accessed);

  ra8_fs_stat_t bad_d = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BADD.BIN", &bad_d));
  internal_expect_invalid(&bad_d.created); /* 10-ms increment > 199 */
  internal_expect_epoch(&bad_d.modified);
  internal_expect_epoch(&bad_d.accessed);
}

/** @brief Check that a legal, non-epoch FAT stamp survives every guard intact. @details The must-stay-quiet direction: without it a decoder that rejected everything would pass the guard vectors above, so this asserts the exact decoded civil fields, including the second the maximum 10-ms increment carries into and the centiseconds it leaves behind. @param[in] h Mounted FAT volume holding the patched entry. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_fat_legal_stamp(ra8_fs_mount_t* h)
{
  ra8_fs_stat_t good = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/GOOD.BIN", &good));

  TEST_ASSERT(good.created.valid);
  TEST_ASSERT(!good.created.utc_offset_valid);
  TEST_ASSERT_EQ(k_stat_exp_year, good.created.value.year);
  TEST_ASSERT_EQ(k_stat_exp_month, good.created.value.month);
  TEST_ASSERT_EQ(k_stat_exp_day, good.created.value.day);
  TEST_ASSERT_EQ(0U, good.created.value.hour);
  TEST_ASSERT_EQ(0U, good.created.value.minute);
  TEST_ASSERT_EQ(k_stat_exp_crt_second, good.created.value.second);
  TEST_ASSERT_EQ(k_stat_exp_csec, good.created.value.centisecond);

  TEST_ASSERT(good.modified.valid);
  TEST_ASSERT_EQ(k_stat_exp_year, good.modified.value.year);
  TEST_ASSERT_EQ(k_stat_exp_hour, good.modified.value.hour);
  TEST_ASSERT_EQ(k_stat_exp_minute, good.modified.value.minute);
  TEST_ASSERT_EQ(k_stat_exp_second, good.modified.value.second);
  TEST_ASSERT_EQ(0U, good.modified.value.centisecond);

  TEST_ASSERT(good.accessed.valid);
  TEST_ASSERT_EQ(k_stat_exp_year, good.accessed.value.year);
  TEST_ASSERT_EQ(k_stat_exp_day, good.accessed.value.day);
  TEST_ASSERT_EQ(0U, good.accessed.value.hour);
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- every packed-field bound in
 * `internal_stat_fat_time_valid()` is a single-condition `if`, and each vector
 * below trips exactly one of them while leaving the neighbouring fields legal)
 * @brief Exercise the FAT packed date/time range guards through ::ra8_fs_stat.
 * @details Formats one FAT16 volume, writes five files, plants one out-of-range
 * packed field per guard plus one legal non-epoch stamp, remounts and checks
 * every decoded timestamp.
 * @pre Pointer arguments address their documented readable or writable extents.
 * @pre Required fixture and backend state is initialized before the call.
 * @post No access exceeds a caller-advertised capacity.
 * @post The return value or assertions describe the observed filesystem state.
 * @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_fat_timestamp_guards(void)
{
  TEST_BEGIN("stat on FAT: out-of-range packed date/time fields are rejected");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "TIME"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  const uint32_t len = (uint32_t)k_stat_payload_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BADA.BIN", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BADB.BIN", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BADC.BIN", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BADD.BIN", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/GOOD.BIN", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  internal_patch_fat_stamps();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_check_fat_bad_dates(h);
  internal_check_fat_bad_times(h);
  internal_check_fat_legal_stamp(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on FAT: out-of-range packed date/time fields are rejected");
}

/** @brief Plant the exFAT UtcOffset and timestamp vectors into two File entries. @details `AAA.TXT` gets an illegal creation stamp carrying an otherwise perfectly valid zone byte (so a dropped validity gate shows up as a fabricated offset on a rejected stamp), a negative in-span modification zone, and an access zone below UTC-12:00. `BBB.TXT` gets a zone above UTC+14:00 plus both inclusive span endpoints. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_patch_exfat_stamps(void)
{
  internal_patch_exfat_u32('A',
                           (uint8_t)k_stat_off_ex_ctime,
                           (uint32_t)k_stat_exfat_stamp_month_zero);
  internal_patch_exfat_u8('A', (uint8_t)k_stat_off_ex_cutc, (uint8_t)k_stat_utc_zero_valid);
  internal_patch_exfat_u8('A', (uint8_t)k_stat_off_ex_mutc, (uint8_t)k_stat_utc_minus_8h);
  internal_patch_exfat_u8('A', (uint8_t)k_stat_off_ex_autc, (uint8_t)k_stat_utc_below_span);
  internal_patch_exfat_u8('B', (uint8_t)k_stat_off_ex_cutc, (uint8_t)k_stat_utc_above_span);
  internal_patch_exfat_u32('B', (uint8_t)k_stat_off_ex_mtime, (uint32_t)k_stat_exfat_stamp_legal);
  internal_patch_exfat_u8('B', (uint8_t)k_stat_off_ex_m10ms, (uint8_t)k_stat_tenth_max);
  internal_patch_exfat_u8('B', (uint8_t)k_stat_off_ex_mutc, (uint8_t)k_stat_utc_span_lo);
  internal_patch_exfat_u8('B', (uint8_t)k_stat_off_ex_autc, (uint8_t)k_stat_utc_span_hi);
}

/** @brief Check that an illegal stamp and an out-of-span zone are both refused. @details The creation stamp of `AAA.TXT` is illegal while its zone byte is well-formed, so `utc_offset_valid` staying false is the only thing separating 'rejected before the zone was read' from 'zone decoded onto a stamp that does not exist'. Its access zone is UTC-16:00, out of span, so the civil fields must survive while the zone does not. @param[in] h Mounted exFAT volume holding the patched entry. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_exfat_utc_rejects(ra8_fs_mount_t* h)
{
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/AAA.TXT", &st));

  /* Illegal packed stamp: the zone byte is never consulted. */
  internal_expect_invalid(&st.created);

  /* Legal stamp, -32 steps: the sign extension of the seven-bit field. */
  internal_expect_epoch_civil(&st.modified);
  TEST_ASSERT(st.modified.utc_offset_valid);
  TEST_ASSERT_EQ(k_stat_exp_utc_minus_480, st.modified.value.utc_offset_min);

  /* Legal stamp, -64 steps: below UTC-12:00, so the zone alone is dropped. */
  internal_expect_epoch(&st.accessed);
  TEST_ASSERT_EQ(0, st.accessed.value.utc_offset_min);
}

/** @brief Check both inclusive endpoints of the exFAT UTC-offset span. @details `BBB.TXT` carries +63 steps (UTC+15:45, refused), exactly -48 steps (UTC-12:00, kept) and exactly +56 steps (UTC+14:00, kept); its modification stamp is a legal non-epoch date with the largest legal 10-ms increment, so the exFAT civil decode is asserted too. @param[in] h Mounted exFAT volume holding the patched entry. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_exfat_utc_bounds(ra8_fs_mount_t* h)
{
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BBB.TXT", &st));

  /* +63 steps is UTC+15:45 -- past the span, so no offset is reported. */
  internal_expect_epoch(&st.created);
  TEST_ASSERT_EQ(0, st.created.value.utc_offset_min);

  /* Exactly UTC-12:00, on a legal non-epoch stamp with a 199 increment. */
  TEST_ASSERT(st.modified.valid);
  TEST_ASSERT(st.modified.utc_offset_valid);
  TEST_ASSERT_EQ(k_stat_exp_utc_min, st.modified.value.utc_offset_min);
  TEST_ASSERT_EQ(k_stat_exp_year, st.modified.value.year);
  TEST_ASSERT_EQ(k_stat_exp_month, st.modified.value.month);
  TEST_ASSERT_EQ(k_stat_exp_day, st.modified.value.day);
  TEST_ASSERT_EQ(k_stat_exp_hour, st.modified.value.hour);
  TEST_ASSERT_EQ(k_stat_exp_minute, st.modified.value.minute);
  TEST_ASSERT_EQ(k_stat_exp_mod_second, st.modified.value.second);
  TEST_ASSERT_EQ(k_stat_exp_csec, st.modified.value.centisecond);

  /* Exactly UTC+14:00. */
  internal_expect_epoch_civil(&st.accessed);
  TEST_ASSERT(st.accessed.utc_offset_valid);
  TEST_ASSERT_EQ(k_stat_exp_utc_max, st.accessed.value.utc_offset_min);
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the validity gate, the OffsetValid bit,
 * the sign bit and both span bounds in `internal_stat_decode_exfat()` are each
 * a single-condition `if`, and every vector below trips exactly one of them)
 * @brief Exercise the exFAT UTC-offset decode and its span through ::ra8_fs_stat.
 * @details Formats one exFAT volume, writes two files, plants a rejected stamp,
 * a negative in-span zone, an out-of-span zone on each side and both inclusive
 * endpoints, then remounts and checks every decoded timestamp.
 * @pre Pointer arguments address their documented readable or writable extents.
 * @pre Required fixture and backend state is initialized before the call.
 * @post No access exceeds a caller-advertised capacity.
 * @post The return value or assertions describe the observed filesystem state.
 * @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stat_exfat_utc_offsets(void)
{
  TEST_BEGIN("stat on exFAT: UTC-offset decoding and its inclusive span");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_exfat, .label = "EXUTC"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  internal_fill(payload, (uint32_t)k_stat_payload_bytes);
  const uint32_t len = (uint32_t)k_stat_payload_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "AAA.TXT", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "BBB.TXT", payload, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  internal_patch_exfat_stamps();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_check_exfat_utc_rejects(h);
  internal_check_exfat_utc_bounds(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("stat on exFAT: UTC-offset decoding and its inclusive span");
}

int main(void)
{
  internal_test_stat_fat_file_dir_missing();
  internal_test_stat_reports_the_real_attribute_byte();
  internal_test_stat_root_and_nested();
  internal_test_stat_consumes_no_file_slot();
  internal_test_stat_guards();
  internal_test_stat_exfat();
  internal_test_stat_fat_timestamp_guards();
  internal_test_stat_exfat_utc_offsets();
  return 0;
}
