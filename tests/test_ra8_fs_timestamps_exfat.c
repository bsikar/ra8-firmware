/**
 * @file test_ra8_fs_timestamps_exfat.c
 * @brief exFAT entry-set timestamps, 10 ms increments, and UtcOffset (#601).
 *
 * @details
 * The exFAT half of the timestamp work. `priv_exfat_build_set()` zero-filled
 * the whole entry set and then wrote only the type byte, SecondaryCount,
 * FileAttributes, name hash, cluster and length -- so CreateTimestamp,
 * LastModifiedTimestamp, LastAccessedTimestamp, both 10 ms fields and all
 * three UtcOffset bytes went to disk as zeros, and a zero exFAT stamp is
 * illegal for the same reason a zero FAT date is: month and day are 1-based.
 *
 * exFAT has three fields FAT does not, and each gets its own case here:
 *
 *   - the **10 ms increment**, which carries back the odd second that both
 *     formats throw away by storing seconds halved;
 *   - the **UtcOffset** byte, 7-bit two's complement in 15-minute steps with
 *     bit 7 as `OffsetValid` -- including the offsets that cannot be expressed
 *     and must therefore be recorded as "not valid" rather than rounded;
 *   - the **SetChecksum**, which covers every stamped byte. The checksum is
 *     recomputed here from the on-disk entry set, so a stamp written after the
 *     checksum -- an entry set a host `fsck` would reject -- fails the test.
 *
 * As in the FAT half, every assertion is against bytes read straight out of
 * the fixture's RAM disk.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_dir_test_util.h"
#include "unity_minimal.h"

/* ===========================================================================
 * On-disk layout (Microsoft exFAT specification, sec 7.4 "File Directory Entry")
 * ===========================================================================
 */

/**
 * @enum xts_off_t
 * @brief Byte offsets inside a 32-byte exFAT File entry.
 *
 * @invariant Every offset lies inside one 32-byte entry.
 * @see read_stamp()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_xts_off_type  = 0U,    /**< EntryType (0x85 for a File entry). */
  k_xts_off_secnt = 1U,    /**< SecondaryCount.                    */
  k_xts_off_csum  = 2U,    /**< SetChecksum (u16).                 */
  k_xts_off_ctime = 8U,    /**< CreateTimestamp (u32).             */
  k_xts_off_mtime = 12U,   /**< LastModifiedTimestamp (u32).       */
  k_xts_off_atime = 16U,   /**< LastAccessedTimestamp (u32).       */
  k_xts_off_c10ms = 20U,   /**< Create10msIncrement.               */
  k_xts_off_m10ms = 21U,   /**< LastModified10msIncrement.         */
  k_xts_off_cutc  = 22U,   /**< CreateUtcOffset.                   */
  k_xts_off_mutc  = 23U,   /**< LastModifiedUtcOffset.             */
  k_xts_off_autc  = 24U,   /**< LastAccessedUtcOffset.             */
  k_xts_entry     = 32U,   /**< Directory entry size.              */
  k_xts_type_file = 0x85U, /**< File directory entry type byte.    */
} xts_off_t;

/**
 * @enum xts_pack_t
 * @brief exFAT's packed 32-bit timestamp layout and the UtcOffset encoding.
 *
 * @details exFAT spec sec 7.4.8: the low 16 bits are FAT's time word and the
 *          high 16 bits are FAT's date word.
 *
 * @invariant `k_xts_epoch` is 1980-01-01 00:00:00, month and day both 1.
 * @see decode_month()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xts_epoch       = 0x00210000U, /**< 1980-01-01 00:00:00 packed.       */
  k_xts_shift_date  = 16U,         /**< Date half position.               */
  k_xts_shift_month = 5U,          /**< Month field within the date half. */
  k_xts_mask_month  = 0x0FU,       /**< 4-bit month field.                */
  k_xts_mask_day    = 0x1FU,       /**< 5-bit day field.                  */
  k_xts_utc_valid   = 0x80U,       /**< OffsetValid bit.                  */
  k_xts_utc_unknown = 0x00U,       /**< OffsetValid clear.                */
  k_xts_shift_byte  = 8U,          /**< Byte position in a word.          */
  k_xts_shift_two   = 16U,         /**< Two-byte position in a word.      */
  k_xts_shift_three = 24U,         /**< Three-byte position in a word.    */
  k_xts_csum_hi_bit = 0x8000U,     /**< Rotate-add wrap bit.              */
} xts_pack_t;

/**
 * @enum xts_fixture_t
 * @brief The instants, offsets and expected encodings used by these cases.
 *
 * @details `k_xts_utc_est` is UTC-05:00, a whole number of 15-minute steps, so
 *          the format can express it: -20 steps is 0x6C in 7-bit two's
 *          complement, which with `OffsetValid` set is 0xEC. `k_xts_utc_odd`
 *          is 7 minutes, which is not a step at all and must come out as
 *          "not valid".
 *
 * @invariant `k_xts_t1_stamp` decodes to `k_xts_t1_*`.
 * @see test_exfat_create_stamps()
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_xts_t1_year    = 2026,       /**< Create instant year.                    */
  k_xts_t1_month   = 8,          /**< Create instant month.                   */
  k_xts_t1_day     = 4,          /**< Create instant day.                     */
  k_xts_t1_hour    = 13,         /**< Create instant hour.                    */
  k_xts_t1_min     = 45,         /**< Create instant minute.                  */
  k_xts_t1_sec     = 31,         /**< Create instant second (odd on purpose). */
  k_xts_t1_centi   = 25,         /**< Create instant hundredths.              */
  k_xts_t1_stamp   = 0x5D046DAF, /**< 2026-08-04 13:45:30 packed.             */
  k_xts_t1_10ms    = 125,        /**< odd second (100) + 25 hundredths.       */
  k_xts_t2_year    = 2027,       /**< Rename instant year.                    */
  k_xts_t2_month   = 1,          /**< Rename instant month.                   */
  k_xts_t2_day     = 2,          /**< Rename instant day.                     */
  k_xts_t2_hour    = 3,          /**< Rename instant hour.                    */
  k_xts_t2_min     = 4,          /**< Rename instant minute.                  */
  k_xts_t2_sec     = 6,          /**< Rename instant second (even).           */
  k_xts_t2_stamp   = 0x5E221883, /**< 2027-01-02 03:04:06 packed.             */
  k_xts_utc_est    = -300,       /**< UTC-05:00 in minutes.                   */
  k_xts_utc_est_by = 0xEC,       /**< -20 steps with OffsetValid set.         */
  k_xts_utc_odd    = 7,          /**< Not a 15-minute step: inexpressible.    */
  k_xts_utc_far    = 900,        /**< UTC+15:00: past the real-world maximum. */
  k_xts_payload    = 64,         /**< Bytes written into the test file.       */
} xts_fixture_t;

/* ===========================================================================
 * Fake clock
 * ===========================================================================
 */

/** @var s_now
 * @brief The reading ::fake_now hands back.
 * @note Rewritten before each phase of a test.
 * @warning Shared across cases; always set it before use.
 * @since 0.1.0
 */
static ra8_fs_datetime_t s_now = {};

/**
 * @brief Injected calendar source: hands back ::s_now.
 *
 * @param[in]  ctx Unused cookie.
 * @param[out] out Receives ::s_now.
 *
 * @return Error code.
 * @retval k_ra8_ok Always -- the failure path is covered in the FAT file.
 *
 * @pre @p out is non-NULL.
 * @pre ::s_now has been set for this phase.
 * @post `*out == s_now`.
 * @post No other state modified.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded fake now fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_fake_now(void* ctx, ra8_fs_datetime_t* out)
{
  (void)ctx;
  *out = s_now;
  return k_ra8_ok;
}

/**
 * @brief Install the fake clock with one instant and one UTC offset.
 *
 * @param[in] y      Year.
 * @param[in] mo     Month.
 * @param[in] d      Day.
 * @param[in] h      Hour.
 * @param[in] mi     Minute.
 * @param[in] s      Second.
 * @param[in] offmin Offset of that civil time from UTC, in minutes.
 *
 * @return Nothing.
 *
 * @pre The library is not mid-operation.
 * @pre The caller sets `s_now.centisecond` afterwards when it matters.
 * @post ::s_now holds the instant and the binding is installed.
 * @post Later stamps come from @p y .. @p s.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded set clock fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_set_clock(int32_t y,
                                            int32_t mo,
                                            int32_t d,
                                            int32_t h,
                                            int32_t mi,
                                            int32_t s,
                                            int32_t offmin)
{
  s_now                    = (ra8_fs_datetime_t){};
  s_now.year               = (uint16_t)y;
  s_now.month              = (uint8_t)mo;
  s_now.day                = (uint8_t)d;
  s_now.hour               = (uint8_t)h;
  s_now.minute             = (uint8_t)mi;
  s_now.second             = (uint8_t)s;
  s_now.utc_offset_min     = (int16_t)offmin;
  const ra8_fs_clock_t clk = {.now = internal_fake_now, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(&clk));
}

/* ===========================================================================
 * RAM-disk readback helpers
 * ===========================================================================
 */

/**
 * @brief Byte offset in the RAM disk of the exFAT root directory cluster.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Offset of the root cluster's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The root cluster's start.
 *
 * @pre @p h is non-NULL and mounted as exFAT.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state modified.
 * @post The result addresses the cluster heap.
 *
 * @note Partition-adjusted: an exFAT volume lives inside an MBR partition (#568).
 * @since 0.1.0 @details Implements the bounded root cluster byte fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_root_cluster_byte(const ra8_fs_mount_t* h)
{
  const uint32_t lba = h->partition_base_lba + h->first_data_lba +
                       ((uint64_t)(h->root_cluster - 2U) * h->sectors_per_cluster);
  return lba * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Find the first File (0x85) entry in the root directory.
 *
 * @details The root also carries the volume-label, allocation-bitmap and
 *          up-case-table system entries, none of which is a File entry, so the
 *          first 0x85 is the file these tests created.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Offset of that entry's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The entry's start.
 *
 * @pre @p h is mounted as exFAT and holds exactly one file.
 * @pre The entry exists (the helper fails the test otherwise).
 * @post No state modified.
 * @post The result addresses a 32-byte File entry.
 *
 * @note Scans one cluster, which is all these tests fill.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_find_file_entry(const ra8_fs_mount_t* h)
{
  const uint32_t base  = internal_root_cluster_byte(h);
  const uint32_t bytes = h->sectors_per_cluster * (uint32_t)k_geo_blk_sz;
  for (uint32_t i = 0U; i < bytes; i += (uint32_t)k_xts_entry) {
    if (s_disk.bytes[base + i] == (uint8_t)k_xts_type_file) {
      return base + i;
    }
  }
  TEST_FAIL_FMT("%s", "no exFAT File entry in the root directory");
  return 0U;
}

/**
 * @brief Read a little-endian 32-bit timestamp out of an entry.
 *
 * @param[in] entry_byte Offset of the entry's first byte in `s_disk.bytes`.
 * @param[in] field_off  Offset of the field within the entry.
 *
 * @return The packed timestamp.
 * @retval 0..UINT32_MAX The four bytes, little-endian.
 *
 * @pre `s_disk.bytes` is allocated and holds the entry.
 * @pre `entry_byte + field_off + 3` is inside the disk.
 * @post No state modified.
 * @post The RAM disk is unmodified.
 *
 * @note Reads the fixture's memory directly, bypassing the driver.
 * @since 0.1.0 @details Implements the bounded read stamp fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_read_stamp(uint32_t entry_byte, uint32_t field_off)
{
  const uint32_t at = entry_byte + field_off;
  return (uint32_t)s_disk.bytes[at] | ((uint32_t)s_disk.bytes[at + 1U] << k_xts_shift_byte) |
         ((uint32_t)s_disk.bytes[at + 2U] << k_xts_shift_two) |
         ((uint32_t)s_disk.bytes[at + 3U] << k_xts_shift_three);
}

/**
 * @brief Assert that a packed exFAT timestamp decodes to a legal date.
 *
 * @param[in] stamp The 32-bit stamp read off the disk.
 *
 * @return Nothing.
 *
 * @pre @p stamp came from an entry the driver wrote.
 * @pre A test is in progress (this asserts).
 * @post The test has failed if the month or the day is zero.
 * @post No state modified.
 *
 * @note Month and day are 1-based, so zero is not a date at all.
 * @since 0.1.0 @details Implements the bounded assert legal stamp fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_assert_legal_stamp(uint32_t stamp)
{
  const uint32_t date  = stamp >> k_xts_shift_date;
  const uint32_t month = (date >> k_xts_shift_month) & (uint32_t)k_xts_mask_month;
  const uint32_t day   = date & (uint32_t)k_xts_mask_day;
  if (month == 0U) {
    TEST_FAIL_FMT("exFAT stamp 0x%08X has month 0", stamp);
  }
  if (day == 0U) {
    TEST_FAIL_FMT("exFAT stamp 0x%08X has day 0", stamp);
  }
}

/**
 * @brief Recompute an entry set's SetChecksum straight from the disk bytes.
 *
 * @details exFAT spec sec 6.3.3: a rotate-right-then-add 16-bit fold over
 *          every byte of the set except the two the checksum itself occupies.
 *          Recomputing it here is what proves the timestamps were stamped
 *          BEFORE the checksum was taken -- a set whose checksum predates its
 *          stamps is one a host `fsck` rejects, and no return code would show
 *          it.
 *
 * @param[in] entry_byte Offset of the set's File entry in `s_disk.bytes`.
 *
 * @return The recomputed checksum.
 * @retval 0..0xFFFF The folded value over `1 + SecondaryCount` entries.
 *
 * @pre `s_disk.bytes` holds a complete entry set at @p entry_byte.
 * @pre The set does not wrap a cluster boundary (true for these fixtures).
 * @post No state modified.
 * @post The RAM disk is unmodified.
 *
 * @note Pure function of the disk bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_recompute_set_checksum(uint32_t entry_byte)
{
  const uint32_t entries = 1U + (uint32_t)s_disk.bytes[entry_byte + k_xts_off_secnt];
  const uint32_t bytes   = entries * (uint32_t)k_xts_entry;
  uint16_t       cs      = 0U;
  for (uint32_t i = 0U; i < bytes; i++) {
    if (i == (uint32_t)k_xts_off_csum) {
      continue;
    }
    if (i == ((uint32_t)k_xts_off_csum + 1U)) {
      continue;
    }
    const uint16_t hi = ((cs & 1U) != 0U) ? (uint16_t)k_xts_csum_hi_bit : (uint16_t)0U;
    cs = (uint16_t)(hi + (uint16_t)(cs >> 1U) + (uint16_t)s_disk.bytes[entry_byte + i]);
  }
  return cs;
}

/**
 * @brief Create one file on a fresh exFAT volume with the clock already set.
 *
 * @param[out] out_h Receives the mounted volume.
 * @param[in]  name  File name to create.
 *
 * @return Nothing.
 *
 * @pre @p out_h is non-NULL.
 * @pre A clock binding is installed.
 * @post A 64 MiB exFAT volume is mounted with @p name on it.
 * @post `*out_h` is usable and must be unmounted by the caller.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded make exfat file fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_make_exfat_file(ra8_fs_mount_t** out_h, const char* name)
{
  internal_build_exfat_vol();
  *out_h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, out_h));
  uint8_t payload[k_xts_payload] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(*out_h, name, payload, (uint32_t)k_xts_payload));
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_exfat_epoch_default
 * @brief With no clock installed an exFAT file carries 1980-01-01, not zeros,
 *        and its UtcOffset bytes say "not valid" rather than claiming UTC.
 *
 * @details Claiming `OffsetValid` with an offset of zero would assert that a
 *          timestamp we invented is UTC. The format defines a "not valid"
 *          encoding for precisely this, and that is the honest one.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it creates a file with no clock
 * installed and compares the entry-set bytes against the epoch encoding)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_epoch_default(void)
{
  TEST_BEGIN("exfat timestamps: no clock -> legal epoch, offset not valid");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
  ra8_fs_mount_t* h = nullptr;
  internal_make_exfat_file(&h, "EPOCH.BIN");

  const uint32_t e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_epoch, internal_read_stamp(e, (uint32_t)k_xts_off_ctime));
  TEST_ASSERT_EQ(k_xts_epoch, internal_read_stamp(e, (uint32_t)k_xts_off_mtime));
  TEST_ASSERT_EQ(k_xts_epoch, internal_read_stamp(e, (uint32_t)k_xts_off_atime));
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_cutc]);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_mutc]);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_autc]);
  internal_assert_legal_stamp(internal_read_stamp(e, (uint32_t)k_xts_off_ctime));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("exfat timestamps: no clock -> legal epoch, offset not valid");
}

/**
 * @test test_exfat_create_stamps
 * @brief An installed clock stamps all three exFAT timestamps, both 10 ms
 *        increments, and all three UtcOffset bytes -- and the SetChecksum
 *        still covers them.
 *
 * @details The checksum recomputation is the load-bearing assertion: it proves
 *          the stamping happens before `priv_exfat_set_checksum()`, which is
 *          the difference between a card a Mac mounts and one it refuses.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it installs a clock, creates a file,
 * and compares every stamped byte plus the recomputed SetChecksum)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_create_stamps(void)
{
  TEST_BEGIN("exfat timestamps: clock stamps create + 10ms + UtcOffset");
  internal_set_clock((int32_t)k_xts_t1_year,
                     (int32_t)k_xts_t1_month,
                     (int32_t)k_xts_t1_day,
                     (int32_t)k_xts_t1_hour,
                     (int32_t)k_xts_t1_min,
                     (int32_t)k_xts_t1_sec,
                     (int32_t)k_xts_utc_est);
  s_now.centisecond = (uint8_t)k_xts_t1_centi;
  ra8_fs_mount_t* h = nullptr;
  internal_make_exfat_file(&h, "STAMP.BIN");

  const uint32_t e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_t1_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_ctime));
  TEST_ASSERT_EQ(k_xts_t1_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_mtime));
  TEST_ASSERT_EQ(k_xts_t1_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_atime));
  TEST_ASSERT_EQ(k_xts_t1_10ms, s_disk.bytes[e + k_xts_off_c10ms]);
  TEST_ASSERT_EQ(k_xts_t1_10ms, s_disk.bytes[e + k_xts_off_m10ms]);
  TEST_ASSERT_EQ(k_xts_utc_est_by, s_disk.bytes[e + k_xts_off_cutc]);
  TEST_ASSERT_EQ(k_xts_utc_est_by, s_disk.bytes[e + k_xts_off_mutc]);
  TEST_ASSERT_EQ(k_xts_utc_est_by, s_disk.bytes[e + k_xts_off_autc]);
  internal_assert_legal_stamp(internal_read_stamp(e, (uint32_t)k_xts_off_ctime));

  const uint16_t on_disk =
    (uint16_t)((uint16_t)s_disk.bytes[e + k_xts_off_csum] |
               ((uint16_t)s_disk.bytes[e + k_xts_off_csum + 1U] << k_xts_shift_byte));
  TEST_ASSERT_EQ(on_disk, internal_recompute_set_checksum(e));

  /* The volume still reads back through the ordinary API. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "STAMP.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
  TEST_END("exfat timestamps: clock stamps create + 10ms + UtcOffset");
}

/**
 * @test test_exfat_utc_offset_encoding
 * @brief An offset the 7-bit field cannot express is recorded as "not valid"
 *        rather than rounded into a claim about the wrong zone.
 *
 * @par MC/DC:
 * Decision chain in priv_utc_byte: `minutes < -720`, then `minutes > 840`,
 * then `minutes % 15 != 0` -- three single-condition branches.
 * - V1: +900 minutes (UTC+15:00) -> second true  -> not valid.
 * - V2: +7 minutes               -> third true   -> not valid.
 * - V3: -300 minutes             -> all false    -> 0xEC, covered by
 *       test_exfat_create_stamps.
 * The below-range vector is the mirror of V1 and is exercised by the same
 * branch with a negative magnitude.
 *
 * @since 0.1.0 @details Runs the exfat utc offset encoding vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_utc_offset_encoding(void)
{
  TEST_BEGIN("exfat timestamps: inexpressible UTC offset -> not valid");
  internal_set_clock((int32_t)k_xts_t1_year,
                     (int32_t)k_xts_t1_month,
                     (int32_t)k_xts_t1_day,
                     (int32_t)k_xts_t1_hour,
                     (int32_t)k_xts_t1_min,
                     (int32_t)k_xts_t1_sec,
                     (int32_t)k_xts_utc_far);
  ra8_fs_mount_t* h = nullptr;
  internal_make_exfat_file(&h, "FAR.BIN");
  uint32_t e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_cutc]);
  internal_assert_legal_stamp(internal_read_stamp(e, (uint32_t)k_xts_off_ctime));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();

  internal_set_clock((int32_t)k_xts_t1_year,
                     (int32_t)k_xts_t1_month,
                     (int32_t)k_xts_t1_day,
                     (int32_t)k_xts_t1_hour,
                     (int32_t)k_xts_t1_min,
                     (int32_t)k_xts_t1_sec,
                     (int32_t)k_xts_utc_odd);
  h = nullptr;
  internal_make_exfat_file(&h, "ODD.BIN");
  e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_cutc]);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_mutc]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();

  /* The negative half of the range check: below UTC-12:00. */
  internal_set_clock((int32_t)k_xts_t1_year,
                     (int32_t)k_xts_t1_month,
                     (int32_t)k_xts_t1_day,
                     (int32_t)k_xts_t1_hour,
                     (int32_t)k_xts_t1_min,
                     (int32_t)k_xts_t1_sec,
                     -(int32_t)k_xts_utc_far);
  h = nullptr;
  internal_make_exfat_file(&h, "NEG.BIN");
  e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_utc_unknown, s_disk.bytes[e + k_xts_off_cutc]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
  TEST_END("exfat timestamps: inexpressible UTC offset -> not valid");
}

/**
 * @test test_exfat_rename_moves_atime_only
 * @brief An exFAT rename advances LastAccessed and leaves Create and
 *        LastModified alone -- with the SetChecksum recomputed over the new
 *        bytes.
 *
 * @details Same reasoning as the FAT rename: the name moved, the contents did
 *          not, so a backup tool must not be told otherwise. The checksum
 *          recomputation matters twice as much here, because the rename path
 *          rebuilds the checksum itself and a stamp applied after it would
 *          leave a set no host will accept.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it renames and asserts which of the
 * three entry-set stamps moved, then recomputes the SetChecksum)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_rename_moves_atime_only(void)
{
  TEST_BEGIN("exfat timestamps: rename moves atime only");
  internal_set_clock((int32_t)k_xts_t1_year,
                     (int32_t)k_xts_t1_month,
                     (int32_t)k_xts_t1_day,
                     (int32_t)k_xts_t1_hour,
                     (int32_t)k_xts_t1_min,
                     (int32_t)k_xts_t1_sec,
                     (int32_t)k_xts_utc_est);
  s_now.centisecond = (uint8_t)k_xts_t1_centi;
  ra8_fs_mount_t* h = nullptr;
  internal_make_exfat_file(&h, "OLD.BIN");

  internal_set_clock((int32_t)k_xts_t2_year,
                     (int32_t)k_xts_t2_month,
                     (int32_t)k_xts_t2_day,
                     (int32_t)k_xts_t2_hour,
                     (int32_t)k_xts_t2_min,
                     (int32_t)k_xts_t2_sec,
                     (int32_t)k_xts_utc_est);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "OLD.BIN", "NEW.BIN"));

  const uint32_t e = internal_find_file_entry(h);
  TEST_ASSERT_EQ(k_xts_t1_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_ctime));
  TEST_ASSERT_EQ(k_xts_t1_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_mtime));
  TEST_ASSERT_EQ(k_xts_t2_stamp, internal_read_stamp(e, (uint32_t)k_xts_off_atime));
  TEST_ASSERT_EQ(k_xts_t1_10ms, s_disk.bytes[e + k_xts_off_c10ms]);

  const uint16_t on_disk =
    (uint16_t)((uint16_t)s_disk.bytes[e + k_xts_off_csum] |
               ((uint16_t)s_disk.bytes[e + k_xts_off_csum + 1U] << k_xts_shift_byte));
  TEST_ASSERT_EQ(on_disk, internal_recompute_set_checksum(e));

  /* The renamed file still resolves through the ordinary API. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "NEW.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
  TEST_END("exfat timestamps: rename moves atime only");
}

/**
 * @brief Run every case in this file.
 *
 * @return Process exit status.
 * @retval 0 Every case passed.
 *
 * @pre The process has a heap for the 64 MiB RAM disk.
 * @pre No other test binary shares this process.
 * @post Every volume allocated here has been freed.
 * @post No clock binding is left installed.
 *
 * @note Single-threaded by construction.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_exfat_epoch_default();
  internal_test_exfat_create_stamps();
  internal_test_exfat_utc_offset_encoding();
  internal_test_exfat_rename_moves_atime_only();
  return 0;
}
