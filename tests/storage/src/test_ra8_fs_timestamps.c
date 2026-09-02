/**
 * @file test_ra8_fs_timestamps.c
 * @brief FAT file timestamps: the injected clock, and the bytes on the disk (#601).
 *
 * @details
 * Every date `ra8_fs` used to write was zero, and a zero FAT date is not a
 * date: `DIR_CrtDate` packs the month and the day as 1-based fields, so
 * 0x0000 claims month 0 of day 0. macOS reads that as uninitialised and shows
 * 31 Dec 1969, Linux clamps both fields to 1 and shows 1980-01-01, Windows
 * shows a blank. This file asserts that no such entry can be written any more.
 *
 * Everything here checks the ON-DISK BYTES, read straight out of the fixture's
 * RAM disk, not a return code: a stamping bug that wrote the right value into
 * the wrong field would sail through a return-code test, and what a host reads
 * back off the card is the whole defect. Each byte pair is then decoded and
 * checked to be a LEGAL date -- month and day both non-zero -- because that,
 * not any particular instant, is what the three hosts disagreed over.
 *
 * The exFAT half lives in `test_ra8_fs_timestamps_exfat.c`.
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

/* ===========================================================================
 * On-disk timestamp layout (MS FAT spec sec 6, "Directory Entry")
 * ===========================================================================
 */

/**
 * @enum ts_dir_off_t
 * @brief Byte offsets of the fields these tests read back out of the RAM disk.
 *
 * @details Spelled with the specification's own field names so each assertion
 *          can be checked against the document rather than decoded from a
 *          constant.
 *
 * @invariant Every offset lies inside one 32-byte directory entry.
 * @see read_entry_word()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ts_off_name      = 0U,  /**< DIR_Name (11 bytes).   */
  k_ts_name_len      = 11U, /**< DIR_Name field length. */
  k_ts_off_crt_tenth = 13U, /**< DIR_CrtTimeTenth.      */
  k_ts_off_crt_time  = 14U, /**< DIR_CrtTime (u16).     */
  k_ts_off_crt_date  = 16U, /**< DIR_CrtDate (u16).     */
  k_ts_off_acc_date  = 18U, /**< DIR_LstAccDate (u16).  */
  k_ts_off_clus_hi   = 20U, /**< DIR_FstClusHI (u16).   */
  k_ts_off_wrt_time  = 22U, /**< DIR_WrtTime (u16).     */
  k_ts_off_wrt_date  = 24U, /**< DIR_WrtDate (u16).     */
  k_ts_off_clus_lo   = 26U, /**< DIR_FstClusLO (u16).   */
  k_ts_entry_bytes   = 32U, /**< Directory entry size.  */
} ts_dir_off_t;

/**
 * @enum ts_pack_t
 * @brief Bit layout of the two packed FAT words, and the epoch encodings.
 *
 * @details `k_ts_epoch_date` is the value the driver must write when no clock
 *          is installed: 1980-01-01, i.e. year offset 0, month 1, day 1.
 *
 * @invariant `k_ts_epoch_date` is `(1 << 5) | 1`.
 * @see decode_date()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ts_epoch_date   = 0x0021U, /**< 1980-01-01 packed.                */
  k_ts_epoch_time   = 0x0000U, /**< 00:00:00 packed.                  */
  k_ts_epoch_year   = 1980U,   /**< Year 0 of the on-disk format.     */
  k_ts_shift_year   = 9U,      /**< Date word: year field position.   */
  k_ts_shift_month  = 5U,      /**< Date word: month field position.  */
  k_ts_shift_hour   = 11U,     /**< Time word: hour field position.   */
  k_ts_shift_minute = 5U,      /**< Time word: minute field position. */
  k_ts_mask_year    = 0x7FU,   /**< 7-bit year field.                 */
  k_ts_mask_month   = 0x0FU,   /**< 4-bit month field.                */
  k_ts_mask_day     = 0x1FU,   /**< 5-bit day field.                  */
  k_ts_mask_hour    = 0x1FU,   /**< 5-bit hour field.                 */
  k_ts_mask_minute  = 0x3FU,   /**< 6-bit minute field.               */
  k_ts_mask_sec2    = 0x1FU,   /**< 5-bit two-second field.           */
  k_ts_shift_byte   = 8U,      /**< Byte position in a 16-bit word.   */
  k_ts_shift_word   = 16U,     /**< Word position in a 32-bit value.  */
  k_ts_month_max    = 12U,     /**< Highest legal month.              */
  k_ts_day_max      = 31U,     /**< Highest day the field expresses.  */
  k_ts_hour_max     = 23U,     /**< Highest legal hour.               */
  k_ts_min_max      = 59U,     /**< Highest legal minute.             */
  k_ts_sec2_max     = 29U,     /**< Highest two-second field value.   */
  k_ts_tenth_max    = 199U,    /**< Highest 10 ms increment.          */
  k_ts_year_max     = 2107U,   /**< Last year the 7-bit field holds.  */
} ts_pack_t;

/**
 * @enum ts_fixture_t
 * @brief The fixed instants and payload sizes these tests inject and expect.
 *
 * @details Three distinct instants so "created", "written" and "closed" can
 *          never be confused for one another by accident: a stamper that wrote
 *          the create time into the write field would still produce a legal
 *          date, and only distinct instants catch it.
 *
 * @invariant The three instants are strictly increasing in wall-clock order.
 * @see set_fake_clock()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ts_t1_year      = 2026U,   /**< Instant 1 ("created") year.        */
  k_ts_t1_month     = 8U,      /**< Instant 1 month.                   */
  k_ts_t1_day       = 4U,      /**< Instant 1 day.                     */
  k_ts_t1_hour      = 13U,     /**< Instant 1 hour.                    */
  k_ts_t1_min       = 45U,     /**< Instant 1 minute.                  */
  k_ts_t1_sec       = 31U,     /**< Instant 1 second (odd on purpose). */
  k_ts_t1_centi     = 25U,     /**< Instant 1 hundredths.              */
  k_ts_t1_date      = 0x5D04U, /**< 2026-08-04 packed.                 */
  k_ts_t1_time      = 0x6DAFU, /**< 13:45:30 packed (31 -> 15).        */
  k_ts_t1_tenth     = 125U,    /**< odd second (100) + 25 centis.      */
  k_ts_t2_year      = 2026U,   /**< Instant 2 ("written") year.        */
  k_ts_t2_month     = 9U,      /**< Instant 2 month.                   */
  k_ts_t2_day       = 15U,     /**< Instant 2 day.                     */
  k_ts_t2_hour      = 6U,      /**< Instant 2 hour.                    */
  k_ts_t2_min       = 7U,      /**< Instant 2 minute.                  */
  k_ts_t2_sec       = 8U,      /**< Instant 2 second (even).           */
  k_ts_t2_date      = 0x5D2FU, /**< 2026-09-15 packed.                 */
  k_ts_t2_time      = 0x30E4U, /**< 06:07:08 packed.                   */
  k_ts_t3_year      = 2027U,   /**< Instant 3 ("closed") year.         */
  k_ts_t3_month     = 1U,      /**< Instant 3 month.                   */
  k_ts_t3_day       = 2U,      /**< Instant 3 day.                     */
  k_ts_t3_hour      = 3U,      /**< Instant 3 hour.                    */
  k_ts_t3_min       = 4U,      /**< Instant 3 minute.                  */
  k_ts_t3_sec       = 5U,      /**< Instant 3 second.                  */
  k_ts_t3_date      = 0x5E22U, /**< 2027-01-02 packed.                 */
  k_ts_t3_time      = 0x1882U, /**< 03:04:04 packed (5 -> 2).          */
  k_ts_payload      = 64U,     /**< Bytes written into the test files. */
  k_ts_dot_slots    = 2U,      /**< "." and ".." occupy slots 0 and 1. */
  k_ts_bad_year_lo  = 1900U,   /**< Below the epoch: must clamp up.    */
  k_ts_bad_year_hi  = 3000U,   /**< Above the field: must clamp down.  */
  k_ts_bad_month_hi = 13U,     /**< Above 12: must clamp to 12.        */
  k_ts_bad_field_hi = 99U,     /**< Above every day/time field.        */
  k_ts_bad_centi_hi = 200U,    /**< Above 99 hundredths.               */
} ts_fixture_t;

/* ===========================================================================
 * Fake clock
 * ===========================================================================
 */

/** @var s_now
 * @brief The reading ::fake_now hands back.
 * @note Rewritten by set_fake_clock() before each phase of a test.
 * @warning Shared across tests in this file; always set it before use.
 * @since 0.1.0
 */
static ra8_fs_datetime_t s_now = {};

/** @var s_now_fails
 * @brief Non-zero makes ::fake_now report a failure instead of a reading.
 * @note Drives the "clock installed but broken" MC/DC vector.
 * @warning Reset to 0 after the case that sets it.
 * @since 0.1.0
 */
static uint8_t s_now_fails = 0U;

/**
 * @brief Injected calendar source: hands back ::s_now, or fails.
 *
 * @param[in]  ctx Unused cookie.
 * @param[out] out Receives ::s_now when the clock is not in its failing mode.
 *
 * @return Error code.
 * @retval k_ra8_ok            Reading delivered.
 * @retval k_ra8_err_hw_error  ::s_now_fails is set.
 *
 * @pre @p out is non-NULL.
 * @pre ::s_now has been set for this phase.
 * @post On success `*out == s_now`.
 * @post On failure @p out is untouched.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded fake now fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_fake_now(void* ctx, ra8_fs_datetime_t* out)
{
  (void)ctx;
  if (s_now_fails != 0U) {
    return k_ra8_err_hw_error;
  }
  *out = s_now;
  return k_ra8_ok;
}

/**
 * @brief Install the fake clock with one instant.
 *
 * @param[in] y  Year.
 * @param[in] mo Month.
 * @param[in] d  Day.
 * @param[in] h  Hour.
 * @param[in] mi Minute.
 * @param[in] s  Second.
 *
 * @return Nothing.
 *
 * @pre The library is not mid-operation.
 * @pre The caller sets `s_now.centisecond` afterwards when it matters.
 * @post ::s_now holds the instant and the binding is installed.
 * @post ::s_now_fails is cleared.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded set fake clock fixture step using caller-owned state.
 */
RA8_INTERNAL static void
internal_set_fake_clock(uint32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi, uint32_t s)
{
  s_now                    = (ra8_fs_datetime_t){};
  s_now.year               = (uint16_t)y;
  s_now.month              = (uint8_t)mo;
  s_now.day                = (uint8_t)d;
  s_now.hour               = (uint8_t)h;
  s_now.minute             = (uint8_t)mi;
  s_now.second             = (uint8_t)s;
  s_now_fails              = 0U;
  const ra8_fs_clock_t clk = {.now = internal_fake_now, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(&clk));
}

/**
 * @brief Install the fake clock at instant 1 ("created").
 * @return Nothing.
 * @pre The library is not mid-operation.
 * @pre ::k_ts_t1_date and ::k_ts_t1_time are the packed form of this instant.
 * @post Later stamps encode as ::k_ts_t1_date / ::k_ts_t1_time.
 * @post ::s_now_fails is cleared.
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded use instant 1 fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_use_instant_1(void)
{
  internal_set_fake_clock((uint32_t)k_ts_t1_year,
                          (uint32_t)k_ts_t1_month,
                          (uint32_t)k_ts_t1_day,
                          (uint32_t)k_ts_t1_hour,
                          (uint32_t)k_ts_t1_min,
                          (uint32_t)k_ts_t1_sec);
}

/**
 * @brief Install the fake clock at instant 2 ("written").
 * @return Nothing.
 * @pre The library is not mid-operation.
 * @pre ::k_ts_t2_date and ::k_ts_t2_time are the packed form of this instant.
 * @post Later stamps encode as ::k_ts_t2_date / ::k_ts_t2_time.
 * @post ::s_now_fails is cleared.
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded use instant 2 fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_use_instant_2(void)
{
  internal_set_fake_clock((uint32_t)k_ts_t2_year,
                          (uint32_t)k_ts_t2_month,
                          (uint32_t)k_ts_t2_day,
                          (uint32_t)k_ts_t2_hour,
                          (uint32_t)k_ts_t2_min,
                          (uint32_t)k_ts_t2_sec);
}

/**
 * @brief Install the fake clock at instant 3 ("closed").
 * @return Nothing.
 * @pre The library is not mid-operation.
 * @pre ::k_ts_t3_date and ::k_ts_t3_time are the packed form of this instant.
 * @post Later stamps encode as ::k_ts_t3_date / ::k_ts_t3_time.
 * @post ::s_now_fails is cleared.
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded use instant 3 fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_use_instant_3(void)
{
  internal_set_fake_clock((uint32_t)k_ts_t3_year,
                          (uint32_t)k_ts_t3_month,
                          (uint32_t)k_ts_t3_day,
                          (uint32_t)k_ts_t3_hour,
                          (uint32_t)k_ts_t3_min,
                          (uint32_t)k_ts_t3_sec);
}

/**
 * @brief Remove any installed clock, restoring the epoch default.
 * @return Nothing.
 * @pre The library is not mid-operation.
 * @pre A binding may or may not be installed.
 * @post No binding is installed.
 * @post Later stamps are 1980-01-01 00:00:00.
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded clear fake clock fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_clear_fake_clock(void)
{
  s_now_fails = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
}

/* ===========================================================================
 * RAM-disk readback helpers
 * ===========================================================================
 */

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
 * @post No state modified.
 * @post The result addresses the root region.
 *
 * @note Partition-adjusted, like every other direct RAM-disk probe (#568).
 * @since 0.1.0 @details Implements the bounded root dir byte fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_root_dir_byte(const ra8_fs_mount_t* h)
{
  return (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Byte offset in the RAM disk of a data cluster's first sector.
 *
 * @param[in] h       Mounted FAT16 volume.
 * @param[in] cluster Cluster number (>= 2).
 *
 * @return Offset of that cluster's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The cluster's start.
 *
 * @pre @p h is non-NULL and mounted; @p cluster >= 2.
 * @pre The fixture's RAM disk backs @p h.
 * @post No state modified.
 * @post The result addresses the data region.
 *
 * @note Partition-adjusted, like every other direct RAM-disk probe (#568).
 * @since 0.1.0 @details Implements the bounded cluster byte fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_cluster_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t lba =
    h->partition_base_lba + h->first_data_lba + ((uint64_t)(cluster - 2U) * h->sectors_per_cluster);
  return lba * (uint32_t)k_geo_blk_sz;
}

/**
 * @brief Read a little-endian 16-bit field out of a directory entry.
 *
 * @param[in] entry_byte Offset of the entry's first byte in `s_disk.bytes`.
 * @param[in] field_off  Offset of the field within the entry.
 *
 * @return The field value.
 * @retval 0..0xFFFF The two bytes, little-endian.
 *
 * @pre `s_disk.bytes` is allocated and holds the entry.
 * @pre `entry_byte + field_off + 1` is inside the disk.
 * @post No state modified.
 * @post The RAM disk is unmodified.
 *
 * @note Reads the fixture's memory directly, bypassing the driver.
 * @since 0.1.0 @details Implements the bounded read entry word fixture step using caller-owned state.
 */
RA8_INTERNAL static uint16_t internal_read_entry_word(uint32_t entry_byte, uint32_t field_off)
{
  const uint32_t at = entry_byte + field_off;
  return (uint16_t)((uint16_t)s_disk.bytes[at] |
                    ((uint16_t)s_disk.bytes[at + 1U] << k_ts_shift_byte));
}

/**
 * @brief Locate the first live 8.3 entry named @p name83 in the root directory.
 *
 * @param[in] h      Mounted FAT16 volume.
 * @param[in] name83 The packed 11-byte name to match, as a plain string.
 *
 * @return Offset of that entry's first byte in `s_disk.bytes`.
 * @retval 0..byte_count The entry's start.
 *
 * @pre @p h is mounted and @p name83 is exactly 11 characters.
 * @pre The entry exists (the helper fails the test otherwise).
 * @post No state modified.
 * @post The result addresses a 32-byte entry.
 *
 * @note Scans the first root sector only, which is all these tests fill.
 * @since 0.1.0 @details Implements the bounded find entry fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_find_entry(const ra8_fs_mount_t* h, const char* name83)
{
  const uint32_t base = internal_root_dir_byte(h);
  for (uint32_t i = 0U; i < (uint32_t)k_geo_blk_sz; i += (uint32_t)k_ts_entry_bytes) {
    if (memcmp(&s_disk.bytes[base + i + k_ts_off_name], name83, (size_t)k_ts_name_len) == 0) {
      return base + i;
    }
  }
  TEST_FAIL_FMT("entry '%s' not found in root", name83);
  return 0U;
}

/**
 * @brief Assert that a packed FAT date decodes to a legal calendar date.
 *
 * @details The property every host disagreed over. A zero date decodes to
 *          month 0 of day 0, which macOS reports as the Unix epoch (31 Dec
 *          1969) and Linux silently clamps -- so "non-zero month AND non-zero
 *          day" is exactly the check that separates a date from a hole.
 *
 * @param[in] packed The `DIR_*Date` word read off the disk.
 *
 * @return Nothing.
 *
 * @pre @p packed came from a directory entry the driver wrote.
 * @pre A test is in progress (this asserts).
 * @post The test has failed if the date is not legal.
 * @post No state modified.
 *
 * @note The year is unconstrained: 1980 and 2107 are both legal.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_legal_date(uint16_t packed)
{
  const uint32_t month = ((uint32_t)packed >> k_ts_shift_month) & (uint32_t)k_ts_mask_month;
  const uint32_t day   = (uint32_t)packed & (uint32_t)k_ts_mask_day;
  if (month == 0U) {
    TEST_FAIL_FMT("packed date 0x%04X has month 0 -- macOS shows 31 Dec 1969", packed);
  }
  if (day == 0U) {
    TEST_FAIL_FMT("packed date 0x%04X has day 0 -- macOS shows 31 Dec 1969", packed);
  }
  if (month > (uint32_t)k_ts_month_max) {
    TEST_FAIL_FMT("packed date 0x%04X has month %u", packed, month);
  }
}

/**
 * @brief Create a small file through the public API.
 *
 * @param[in] h    Mounted volume.
 * @param[in] path Path to create.
 *
 * @return Nothing.
 *
 * @pre @p h is mounted read-write.
 * @pre @p path is an 8.3 name that does not yet exist.
 * @post @p path holds ::k_ts_payload bytes.
 * @post The handle used is closed.
 *
 * @note Uses the same seam a caller would.
 * @since 0.1.0 @details Implements the bounded make file fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_make_file(ra8_fs_mount_t* h, const char* path)
{
  uint8_t payload[k_ts_payload] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, path, payload, (uint32_t)k_ts_payload));
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_clock_seam_guards
 * @brief `ra8_fs_set_clock()` accepts a binding, rejects a broken one, and
 *        accepts NULL as "go back to the epoch default".
 *
 * @par MC/DC:
 * Decision: `clock == nullptr` then `clock->now == nullptr`, written as two
 * single-condition `if`s because the second cannot be evaluated when the first
 * is true (there is no struct to dereference).
 * - V1: clock = NULL          -> first true  -> binding removed, k_ra8_ok.
 * - V2: clock with now = NULL -> first false, second true  -> invalid_arg.
 * - V3: complete binding      -> first false, second false -> installed, ok.
 *
 * @since 0.1.0 @details Runs the clock seam guards vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_clock_seam_guards(void)
{
  TEST_BEGIN("fs timestamps: set_clock guards");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(nullptr));
  const ra8_fs_clock_t broken = {.now = nullptr, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_set_clock(&broken));
  const ra8_fs_clock_t good = {.now = internal_fake_now, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_clock(&good));
  internal_clear_fake_clock();
  TEST_END("fs timestamps: set_clock guards");
}

/**
 * @test test_epoch_default_is_legal
 * @brief With no clock installed, a created file carries 1980-01-01 -- not the
 *        zeros that made three hosts disagree.
 *
 * @details Checks the raw `DIR_CrtDate`, `DIR_WrtDate` and `DIR_LstAccDate`
 *          words against 0x0021 and then decodes them, so a regression that
 *          wrote some other non-zero value would still be caught.
 *
 * @par MC/DC:
 * Decision: `s_clock_bound` in priv_now_or_epoch (1 condition).
 * - V1 (here): no binding -> false -> the epoch is written.
 * - V2: see test_clock_stamps_create, which installs one -> true.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_epoch_default_is_legal(void)
{
  TEST_BEGIN("fs timestamps: no clock -> legal 1980-01-01");
  internal_clear_fake_clock();
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_make_file(h, "EPOCH.BIN");

  const uint32_t ent = internal_find_entry(h, "EPOCH   BIN");
  TEST_ASSERT_EQ(k_ts_epoch_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  TEST_ASSERT_EQ(k_ts_epoch_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_epoch_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_acc_date));
  TEST_ASSERT_EQ(k_ts_epoch_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_time));
  TEST_ASSERT_EQ(k_ts_epoch_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));
  TEST_ASSERT_EQ(0U, s_disk.bytes[ent + k_ts_off_crt_tenth]);
  internal_assert_legal_date(internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  internal_assert_legal_date(internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs timestamps: no clock -> legal 1980-01-01");
}

/**
 * @test test_broken_clock_falls_back
 * @brief A clock that is installed but fails leaves the epoch on disk, not
 *        zeros, and does not fail the write.
 *
 * @details A logger whose RTC has not been set yet must still be able to
 *          write. This is the middle vector of priv_now_or_epoch: bound, but
 *          the call does not return ::k_ra8_ok.
 *
 * @par MC/DC:
 * Decision: `s_clock.now(...) == k_ra8_ok` inside the `s_clock_bound` branch
 * (1 condition).
 * - V1 (here): now() fails    -> false -> epoch written, create still succeeds.
 * - V2: test_clock_stamps_create -> true -> the reading is written.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_broken_clock_falls_back(void)
{
  TEST_BEGIN("fs timestamps: failing clock -> epoch, write still succeeds");
  internal_use_instant_1();
  s_now_fails = 1U;
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_make_file(h, "BROKE.BIN");

  const uint32_t ent = internal_find_entry(h, "BROKE   BIN");
  TEST_ASSERT_EQ(k_ts_epoch_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  internal_assert_legal_date(internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: failing clock -> epoch, write still succeeds");
}

/**
 * @test test_clock_stamps_create
 * @brief An installed clock puts its exact instant into all three date fields
 *        and into `DIR_CrtTimeTenth`.
 *
 * @details The tenths field is the interesting one: both formats store seconds
 *          halved, so an odd second is lost unless the 10 ms field carries it
 *          back. Second 31 with 25 hundredths must encode as 125, not 25.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it installs a clock, creates a file,
 * and compares the six on-disk timestamp fields against the packed encoding
 * of that one instant)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_clock_stamps_create(void)
{
  TEST_BEGIN("fs timestamps: installed clock stamps create");
  internal_use_instant_1();
  s_now.centisecond = (uint8_t)k_ts_t1_centi;
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_make_file(h, "STAMP.BIN");

  const uint32_t ent = internal_find_entry(h, "STAMP   BIN");
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  TEST_ASSERT_EQ(k_ts_t1_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_time));
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_acc_date));
  TEST_ASSERT_EQ(k_ts_t1_tenth, s_disk.bytes[ent + k_ts_off_crt_tenth]);
  internal_assert_legal_date(internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: installed clock stamps create");
}

/**
 * @test test_out_of_range_is_clamped
 * @brief A clock returning nonsense produces a legal date, not a failed write.
 *
 * @details Two sweeps: everything below the range, then everything above it. A
 *          filesystem that refused to write because the RTC came back with
 *          month 13 would trade a cosmetic defect for a data-loss one.
 *
 * @par MC/DC:
 * Decision: `v < lo` then `v > hi` in priv_clamp_u32, two single-condition
 * branches driven for every field.
 * - V1: year 1900, month 0, day 0            -> first true  -> clamps up.
 * - V2: year 3000, month 13, day 99, hour 99 -> second true -> clamps down.
 * - V3: the in-range case, covered by test_clock_stamps_create -> both false.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_out_of_range_is_clamped(void)
{
  TEST_BEGIN("fs timestamps: out-of-range clock readings clamp");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_set_fake_clock((uint32_t)k_ts_bad_year_lo, 0U, 0U, 0U, 0U, 0U);
  internal_make_file(h, "LOW.BIN");
  const uint32_t low = internal_find_entry(h, "LOW     BIN");
  TEST_ASSERT_EQ(k_ts_epoch_date, internal_read_entry_word(low, (uint32_t)k_ts_off_crt_date));
  internal_assert_legal_date(internal_read_entry_word(low, (uint32_t)k_ts_off_crt_date));

  internal_set_fake_clock((uint32_t)k_ts_bad_year_hi,
                          (uint32_t)k_ts_bad_month_hi,
                          (uint32_t)k_ts_bad_field_hi,
                          (uint32_t)k_ts_bad_field_hi,
                          (uint32_t)k_ts_bad_field_hi,
                          (uint32_t)k_ts_bad_field_hi);
  s_now.centisecond = (uint8_t)k_ts_bad_centi_hi;
  internal_make_file(h, "HIGH.BIN");
  const uint32_t high  = internal_find_entry(h, "HIGH    BIN");
  const uint16_t date  = internal_read_entry_word(high, (uint32_t)k_ts_off_crt_date);
  const uint16_t time  = internal_read_entry_word(high, (uint32_t)k_ts_off_crt_time);
  const uint32_t year  = ((uint32_t)date >> k_ts_shift_year) & (uint32_t)k_ts_mask_year;
  const uint32_t month = ((uint32_t)date >> k_ts_shift_month) & (uint32_t)k_ts_mask_month;
  const uint32_t day   = (uint32_t)date & (uint32_t)k_ts_mask_day;
  const uint32_t hour  = ((uint32_t)time >> k_ts_shift_hour) & (uint32_t)k_ts_mask_hour;
  const uint32_t min   = ((uint32_t)time >> k_ts_shift_minute) & (uint32_t)k_ts_mask_minute;
  const uint32_t sec2  = (uint32_t)time & (uint32_t)k_ts_mask_sec2;
  TEST_ASSERT_EQ(k_ts_year_max, k_ts_epoch_year + year);
  TEST_ASSERT_EQ(k_ts_month_max, month);
  TEST_ASSERT_EQ(k_ts_day_max, day);
  TEST_ASSERT_EQ(k_ts_hour_max, hour);
  TEST_ASSERT_EQ(k_ts_min_max, min);
  TEST_ASSERT_EQ(k_ts_sec2_max, sec2);
  /* 59 is odd, so the tenths field is 100 + the clamped 99 hundredths. */
  TEST_ASSERT_EQ(k_ts_tenth_max, s_disk.bytes[high + k_ts_off_crt_tenth]);
  internal_assert_legal_date(date);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: out-of-range clock readings clamp");
}

/**
 * @test test_write_moves_mtime_not_ctime
 * @brief Writing content advances the modification time and leaves the
 *        creation time where it was.
 *
 * @details The half of #601 that matters most to a backup tool: before this,
 *          the write path touched only offsets 20, 26 and 28, so a file a PC
 *          created kept the PC's `DIR_WrtDate` through every firmware write
 *          and `rsync` concluded nothing had changed.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it moves the clock between the create
 * and the write and asserts which fields followed it)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_write_moves_mtime_not_ctime(void)
{
  TEST_BEGIN("fs timestamps: write moves mtime, not ctime");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_use_instant_1();
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "MTIME.BIN", k_ra8_fs_mode_write, &f));

  internal_use_instant_2();
  uint8_t payload[k_ts_payload] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, (uint32_t)k_ts_payload));

  const uint32_t ent = internal_find_entry(h, "MTIME   BIN");
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  TEST_ASSERT_EQ(k_ts_t1_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_time));
  TEST_ASSERT_EQ(k_ts_t2_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_t2_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));
  TEST_ASSERT_EQ(k_ts_t2_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_acc_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: write moves mtime, not ctime");
}

/**
 * @test test_close_stamps_final_mtime
 * @brief Closing a handle that was written advances the modification time to
 *        the moment of the close; closing a read handle touches nothing.
 *
 * @par MC/DC:
 * Decision: `(dirty != 0) && (in_use != 0) && (mount != nullptr) &&
 * (mount->in_use != 0)` in
 * `libs/ra8_fs/src/ra8_fs_fat_file.c@priv_close_locked` (4 conditions).
 * - V1 (here): written handle, open, mounted -> true  (control).
 * - V2 (here): read-only handle (dirty == 0) -> false (varies condition 1).
 * - V3..V5: see test_close_guards_an_unusable_handle, which varies conditions
 *   2, 3 and 4.
 *
 * @since 0.1.0 @details Runs the close stamps final mtime vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_close_stamps_final_mtime(void)
{
  TEST_BEGIN("fs timestamps: close stamps the final mtime");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_use_instant_2();
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CLOSE.BIN", k_ra8_fs_mode_write, &f));
  uint8_t payload[k_ts_payload] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, (uint32_t)k_ts_payload));

  /* V1: the clock moves between the write and the close, so the close stamp is
   * distinguishable from the write stamp. */
  internal_use_instant_3();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  const uint32_t ent = internal_find_entry(h, "CLOSE   BIN");
  TEST_ASSERT_EQ(k_ts_t3_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_t3_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));

  /* V2: a read-only handle is never dirty, so closing it changes nothing. */
  internal_use_instant_1();
  ra8_fs_file_t* r = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CLOSE.BIN", k_ra8_fs_mode_read, &r));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(r));
  TEST_ASSERT_EQ(k_ts_t3_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_t3_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: close stamps the final mtime");
}

/**
 * @test test_close_guards_an_unusable_handle
 * @brief A dirty handle whose slot or mount has gone is closed without
 *        touching the volume.
 *
 * @details The guard exists because the dir-entry LBA in a stale handle would
 *          be read against a mount slot that now describes some other card.
 *          None of these three states can be produced by an ordinary call
 *          sequence, so each is poked through the public struct -- which is
 *          exactly the shape a use-after-unmount bug presents.
 *
 * @par MC/DC:
 * The same decision as test_close_stamps_final_mtime -- the four-condition
 * guard in `libs/ra8_fs/src/ra8_fs_fat_file.c@priv_close_locked` -- completing its
 * vector set.
 * - V3: handle already marked closed    -> false (varies condition 2).
 * - V4: handle with mount cleared       -> false (varies condition 3).
 * - V5: handle whose mount was released -> false (varies condition 4).
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_close_guards_an_unusable_handle(void)
{
  TEST_BEGIN("fs timestamps: close refuses to touch a stale handle");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_use_instant_1();

  uint8_t        payload[k_ts_payload] = {};
  ra8_fs_file_t* g                     = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "GUARD.BIN", k_ra8_fs_mode_write, &g));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(g, payload, (uint32_t)k_ts_payload));
  g->in_use = 0U; /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(g));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "GUARD.BIN", k_ra8_fs_mode_append, &g));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(g, payload, (uint32_t)k_ts_payload));
  g->mount = nullptr; /* V4 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(g));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "GUARD.BIN", k_ra8_fs_mode_append, &g));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(g, payload, (uint32_t)k_ts_payload));
  h->in_use = 0U; /* V5 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(g));
  h->in_use = 1U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: close refuses to touch a stale handle");
}

/**
 * @test test_truncate_moves_mtime
 * @brief Re-opening an existing file for writing advances its modification
 *        time even if not one byte is then written.
 *
 * @details Truncation IS a content change: the file went from N bytes to zero.
 *          Leaving the old mtime would describe contents that no longer exist.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it re-opens for writing and asserts
 * the modification stamp moved while the creation stamp did not)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_truncate_moves_mtime(void)
{
  TEST_BEGIN("fs timestamps: truncate moves mtime");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_use_instant_1();
  internal_make_file(h, "TRUNC.BIN");
  const uint32_t ent = internal_find_entry(h, "TRUNC   BIN");
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));

  internal_use_instant_2();
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "TRUNC.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ts_t2_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_t2_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));
  /* The creation stamp survives a truncation. */
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: truncate moves mtime");
}

/**
 * @test test_rename_moves_atime_only
 * @brief Renaming advances the access date and deliberately leaves the
 *        modification stamp alone.
 *
 * @details A rename changes the name, not the bytes. Moving `DIR_WrtDate`
 *          would tell every `rsync`, backup and "newest image" OTA heuristic
 *          that the contents changed -- the same false signal #601 exists to
 *          remove, only inverted.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it renames and asserts which of the
 * three date fields moved)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rename_moves_atime_only(void)
{
  TEST_BEGIN("fs timestamps: rename moves atime only");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_use_instant_1();
  internal_make_file(h, "OLD.BIN");

  internal_use_instant_2();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "OLD.BIN", "NEW.BIN"));

  const uint32_t ent = internal_find_entry(h, "NEW     BIN");
  TEST_ASSERT_EQ(k_ts_t2_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_acc_date));
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_date));
  TEST_ASSERT_EQ(k_ts_t1_time, internal_read_entry_word(ent, (uint32_t)k_ts_off_wrt_time));
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: rename moves atime only");
}

/**
 * @test test_mkdir_stamps_dot_entries
 * @brief A new directory's own entry AND its "." / ".." links carry a legal
 *        date.
 *
 * @details "." and ".." are ordinary directory entries to every host, and
 *          `priv_pack_dot_entry()` zero-filled them, so a directory created by
 *          this firmware used to contain two illegal dates inside itself.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it creates a directory and decodes
 * the dates in its own entry and in its "." / ".." links)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mkdir_stamps_dot_entries(void)
{
  TEST_BEGIN("fs timestamps: mkdir stamps the dot entries");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_use_instant_1();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));

  const uint32_t ent = internal_find_entry(h, "LOGS       ");
  TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));
  internal_assert_legal_date(internal_read_entry_word(ent, (uint32_t)k_ts_off_crt_date));

  const uint32_t hi   = internal_read_entry_word(ent, (uint32_t)k_ts_off_clus_hi);
  const uint32_t lo   = internal_read_entry_word(ent, (uint32_t)k_ts_off_clus_lo);
  const uint32_t clus = (hi << k_ts_shift_word) | lo;
  const uint32_t dots = internal_cluster_byte(h, clus);
  for (uint32_t i = 0U; i < (uint32_t)k_ts_dot_slots; i++) {
    const uint32_t slot = dots + (i * (uint32_t)k_ts_entry_bytes);
    TEST_ASSERT_EQ('.', s_disk.bytes[slot]);
    TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(slot, (uint32_t)k_ts_off_crt_date));
    TEST_ASSERT_EQ(k_ts_t1_date, internal_read_entry_word(slot, (uint32_t)k_ts_off_wrt_date));
    internal_assert_legal_date(internal_read_entry_word(slot, (uint32_t)k_ts_off_wrt_date));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  internal_clear_fake_clock();
  TEST_END("fs timestamps: mkdir stamps the dot entries");
}

/**
 * @brief Run every case in this file.
 *
 * @return Process exit status.
 * @retval 0 Every case passed.
 *
 * @pre The process has a heap for the RAM disk.
 * @pre No other test binary shares this process.
 * @post Every volume allocated here has been freed.
 * @post No clock binding is left installed.
 *
 * @note Single-threaded by construction.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_clock_seam_guards();
  internal_test_epoch_default_is_legal();
  internal_test_broken_clock_falls_back();
  internal_test_clock_stamps_create();
  internal_test_out_of_range_is_clamped();
  internal_test_write_moves_mtime_not_ctime();
  internal_test_close_stamps_final_mtime();
  internal_test_close_guards_an_unusable_handle();
  internal_test_truncate_moves_mtime();
  internal_test_rename_moves_atime_only();
  internal_test_mkdir_stamps_dot_entries();
  return 0;
}
