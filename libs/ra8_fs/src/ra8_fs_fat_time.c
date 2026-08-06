/**
 * @file ra8_fs_fat_time.c
 * @brief The wall-clock seam, and the packing of a reading into on-disk fields.
 *
 * @details
 * Every timestamp `ra8_fs` used to write was zero, and stayed zero forever.
 * That is not merely uninformative, it is not a legal date: FAT packs the
 * month and the day as 1-based fields, so a `DIR_WrtDate` of 0x0000 claims
 * month 0 of day 0. macOS reads it as uninitialised and shows 31 Dec 1969,
 * Linux clamps both fields to 1 and shows 1980-01-01, Windows shows a blank.
 * No host shows a real time and no two hosts agree.
 *
 * This file fixes that in two independent layers.
 *
 * The first has no dependencies at all: with nothing installed, every stamp is
 * 1980-01-01 00:00:00, the first instant either format can express. That alone
 * makes every host agree, and it is what a board with no RTC will write.
 *
 * The second is the injected clock. `ra8_fs` cannot reach a calendar itself --
 * `ra8_time` counts monotonic milliseconds and `ra8_rtc_get()` lives in
 * `ra8_hal`, which this library must not depend on if it is to keep building
 * against the host test mock -- so the calendar arrives as a function pointer
 * plus a cookie, in the same shape as the block backend and the lock seam. A
 * `now()` that fails falls back to the epoch for that one stamp: a clock that
 * has not been set yet must not stop a data logger from writing.
 *
 * Readings are CLAMPED, never rejected. A filesystem that refused to write
 * because a clock came back with month 13 would be trading a cosmetic problem
 * for a data-loss one.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000.
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/* =============================================================================
 * Module state -- the installed binding
 * =============================================================================
 */

/**
 * @var s_clock
 * @brief The installed calendar binding; meaningful only when `s_clock_bound`.
 *
 * @details A copy of the caller's ::ra8_fs_clock_t rather than a pointer to
 *          it, so a binding built as a compound literal or on the installing
 *          function's stack stays valid for the life of the program. The `ctx`
 *          cookie is still the caller's to keep alive.
 *
 * @note Written only by ::ra8_fs_set_clock, which is an init-time call.
 * @warning Never modify directly; a half-updated binding would leave the
 *          library calling through a function pointer with the wrong cookie.
 * @since 0.1.0
 */
static ra8_fs_clock_t s_clock;

/**
 * @var s_clock_bound
 * @brief True once a complete binding has been installed.
 *
 * @details Kept separate from `s_clock` so "no clock" is one predictable
 *          branch rather than a NULL test on a function pointer the compiler
 *          must reload.
 *
 * @note Written only by ::ra8_fs_set_clock.
 * @warning Never modify directly.
 * @since 0.1.0
 */
static bool s_clock_bound;

/**
 * @struct fat_stamp_t
 * @brief One reading already packed into the two 16-bit FAT words plus tenths.
 *
 * @details Produced once per stamping call so a file's create, write and
 *          access fields all describe the same instant instead of straddling
 *          a clock tick between three separate reads.
 *
 * @invariant `date` is never 0 -- month and day are 1-based and always clamped
 *            into range, which is the whole point of this file.
 * @see priv_fat_stamp_now()
 * @since 0.1.0
 */
typedef struct {
  uint16_t date;  /**< MS FAT spec sec 6 packed date word.  */
  uint16_t time;  /**< MS FAT spec sec 6 packed time word.  */
  uint8_t  tenth; /**< `DIR_CrtTimeTenth`: 0..199 in 10 ms. */
} fat_stamp_t;

/**
 * @struct exfat_stamp_t
 * @brief One reading already packed into exFAT's 32-bit stamp plus its extras.
 *
 * @details exFAT stores the same six calendar fields in a single 32-bit word,
 *          and adds the two side fields FAT has no room for: the 10 ms
 *          increment that recovers the odd second, and the UtcOffset byte that
 *          records which zone the civil time belongs to.
 *
 * @invariant `stamp`'s month and day sub-fields are never 0.
 * @invariant `utc` is either ::k_fs_utc_unknown or has ::k_fs_utc_valid_bit set.
 * @see priv_exfat_stamp_now()
 * @since 0.1.0
 */
typedef struct {
  uint32_t stamp; /**< exFAT spec sec 7.4.8 packed timestamp. */
  uint8_t  inc10; /**< 10 ms increment, 0..199.               */
  uint8_t  utc;   /**< UtcOffset byte, or ::k_fs_utc_unknown. */
} exfat_stamp_t;

/* =============================================================================
 * Clamping
 * =============================================================================
 */

/**
 * @brief Clamp @p v into the closed range [@p lo, @p hi].
 *
 * @details The whole clamping policy in one place: a clock that returns
 *          nonsense degrades the stamp, it does not fail the write.
 *
 * @param[in] v  Value to constrain.
 * @param[in] lo Lowest permitted value.
 * @param[in] hi Highest permitted value.
 *
 * @return The constrained value.
 * @retval lo   @p v was below the range.
 * @retval hi   @p v was above the range.
 * @retval v    @p v was already inside the range.
 *
 * @pre `lo <= hi`.
 * @pre All three arguments are already widened to 32 bits.
 * @post No state modified.
 * @post The result is in [@p lo, @p hi].
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

/**
 * @brief Read the installed clock, or hand back the FAT epoch.
 *
 * @details Zero-initialises @p out to the epoch first, so every path -- no
 *          binding, a `now()` that fails, a `now()` that succeeds -- leaves a
 *          fully populated struct. Then clamps every field into the range the
 *          on-disk formats can express.
 *
 * @param[out] out Receives a reading that is always legal to pack.
 *
 * @return Whether the fields came from a real clock.
 * @retval true  An installed `now()` answered; the fields describe an instant.
 * @retval false No binding, or `now()` failed; the fields are the epoch
 *               placeholder and the caller must not attach a UTC offset to it.
 *
 * @pre @p out is non-NULL.
 * @pre ::ra8_fs_set_clock is not running concurrently.
 * @post `out->year` is in [1980, 2107] and every other field is in range.
 * @post `out->month` and `out->day` are at least 1.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock.
 *
 * @par MC/DC:
 * Two single-condition guards, nested rather than joined, because the second
 * cannot be evaluated at all when the first is false (there is no function
 * pointer to call): `s_clock_bound`, then `s_clock.now(...) == k_ra8_ok`.
 * Vectors: no binding -> epoch; binding whose `now()` fails -> epoch; binding
 * whose `now()` succeeds -> the reading.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_now_or_epoch(ra8_fs_datetime_t* out)
{
  *out       = (ra8_fs_datetime_t){};
  out->year  = (uint16_t)k_fs_time_epoch_year;
  out->month = (uint8_t)k_fs_time_month_min;
  out->day   = (uint8_t)k_fs_time_day_min;
  bool real  = false;
  if (s_clock_bound) {
    ra8_fs_datetime_t got = {};
    if (s_clock.now(s_clock.ctx, &got) == k_ra8_ok) {
      *out = got;
      real = true;
    }
  }
  out->year  = (uint16_t)priv_clamp_u32(out->year,
                                        (uint32_t)k_fs_time_epoch_year,
                                        (uint32_t)k_fs_time_year_max);
  out->month = (uint8_t)priv_clamp_u32(out->month,
                                       (uint32_t)k_fs_time_month_min,
                                       (uint32_t)k_fs_time_month_max);
  out->day =
    (uint8_t)priv_clamp_u32(out->day, (uint32_t)k_fs_time_day_min, (uint32_t)k_fs_time_day_max);
  out->hour        = (uint8_t)priv_clamp_u32(out->hour, 0U, (uint32_t)k_fs_time_hour_max);
  out->minute      = (uint8_t)priv_clamp_u32(out->minute, 0U, (uint32_t)k_fs_time_minute_max);
  out->second      = (uint8_t)priv_clamp_u32(out->second, 0U, (uint32_t)k_fs_time_second_max);
  out->centisecond = (uint8_t)priv_clamp_u32(out->centisecond, 0U, (uint32_t)k_fs_time_centi_max);
  return real;
}

/* =============================================================================
 * Packing
 * =============================================================================
 */

/**
 * @brief Count the 10 ms increments the 2-second stamp granularity discards.
 *
 * @details Both formats store seconds divided by two, so an odd second is
 *          lost. `DIR_CrtTimeTenth` (MS FAT spec sec 6) and exFAT's
 *          `Create10msIncrement` (exFAT spec sec 7.4.11) exist to carry it
 *          back: the field counts 10 ms units in 0..199, i.e. the odd second
 *          contributes a whole 100 and the hundredths contribute the rest.
 *
 * @param[in] t Reading already clamped by ::priv_now_or_epoch.
 *
 * @return The 10 ms increment.
 * @retval 0..199 Odd-second flag folded together with the hundredths.
 *
 * @pre `t->second <= 59` and `t->centisecond <= 99`.
 * @pre @p t is non-NULL.
 * @post No state modified.
 * @post The result is at most 199.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_tenths_of(const ra8_fs_datetime_t* t)
{
  const uint32_t odd = (uint32_t)t->second % (uint32_t)k_fs_time_second_div;
  return (uint8_t)((odd * (uint32_t)k_fs_centi_per_second) + (uint32_t)t->centisecond);
}

/**
 * @brief Encode a UTC offset in minutes as an exFAT `UtcOffset` byte.
 *
 * @details exFAT spec sec 7.4.13: bit 7 is `OffsetValid` and bits 6:0 are the
 *          offset in 15-minute steps as a 7-bit two's-complement value. An
 *          offset that is not a whole 15-minute step, or that falls outside
 *          UTC-12:00..UTC+14:00, cannot be expressed -- so it is recorded as
 *          "not valid" rather than rounded into a lie about which zone the
 *          civil time in the stamp belongs to.
 *
 * @param[in] minutes Offset of the stamped civil time from UTC.
 *
 * @return The encoded byte.
 * @retval k_fs_utc_unknown  @p minutes is not representable.
 * @retval 0x80..0xFF        `OffsetValid` set, with the step count in bits 6:0.
 *
 * @pre None -- every input is handled.
 * @pre @p minutes is the offset of the civil time, not of the observer.
 * @post No state modified.
 * @post Either the result is ::k_fs_utc_unknown or bit 7 is set.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_utc_byte(int16_t minutes)
{
  if (minutes < (int16_t)k_fs_utc_span_min) {
    return (uint8_t)k_fs_utc_unknown;
  }
  if (minutes > (int16_t)k_fs_utc_span_max) {
    return (uint8_t)k_fs_utc_unknown;
  }
  if ((int32_t)minutes % (int32_t)k_fs_utc_step_min != 0) {
    return (uint8_t)k_fs_utc_unknown;
  }
  const int32_t steps = (int32_t)minutes / (int32_t)k_fs_utc_step_min;
  return (uint8_t)((uint32_t)k_fs_utc_valid_bit |
                   ((uint32_t)steps & (uint32_t)k_fs_utc_field_mask));
}

/**
 * @brief Pack a reading into FAT's two 16-bit words plus the tenths byte.
 *
 * @details MS FAT spec sec 6: the date word is
 *          `(year - 1980) << 9 | month << 5 | day` and the time word is
 *          `hour << 11 | minute << 5 | second / 2`.
 *
 * @param[out] out Receives the packed fields.
 *
 * @return Nothing.
 *
 * @pre @p out is non-NULL.
 * @pre ::ra8_fs_set_clock is not running concurrently.
 * @post `out->date` is a legal FAT date (month and day both non-zero).
 * @post `out->tenth` is at most 199.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_fat_stamp_now(fat_stamp_t* out)
{
  ra8_fs_datetime_t t = {};
  (void)priv_now_or_epoch(&t); /* FAT has no offset field: real or epoch, same packing */
  const uint32_t years = (uint32_t)t.year - (uint32_t)k_fs_time_epoch_year;
  out->date  = (uint16_t)((years << (uint32_t)k_fs_date_shift_year) |
                          ((uint32_t)t.month << (uint32_t)k_fs_date_shift_month) | (uint32_t)t.day);
  out->time  = (uint16_t)(((uint32_t)t.hour << (uint32_t)k_fs_time_shift_hour) |
                          ((uint32_t)t.minute << (uint32_t)k_fs_time_shift_minute) |
                          ((uint32_t)t.second / (uint32_t)k_fs_time_second_div));
  out->tenth = priv_tenths_of(&t);
}

/**
 * @brief Pack a reading into exFAT's 32-bit stamp plus its two side fields.
 *
 * @details exFAT spec sec 7.4.8: the low 16 bits are FAT's time word and the
 *          high 16 bits are FAT's date word, which is why the shifts are
 *          shared with ::priv_fat_stamp_now.
 *
 * @param[out] out Receives the packed stamp, 10 ms increment, and UtcOffset.
 *
 * @return Nothing.
 *
 * @pre @p out is non-NULL.
 * @pre ::ra8_fs_set_clock is not running concurrently.
 * @post `out->stamp`'s month and day sub-fields are non-zero.
 * @post `out->utc` is ::k_fs_utc_unknown or has ::k_fs_utc_valid_bit set.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exfat_stamp_now(exfat_stamp_t* out)
{
  ra8_fs_datetime_t t     = {};
  const bool        real  = priv_now_or_epoch(&t);
  const uint32_t    years = (uint32_t)t.year - (uint32_t)k_fs_time_epoch_year;
  const uint32_t    date  = (years << (uint32_t)k_fs_xf_shift_year) |
                            ((uint32_t)t.month << (uint32_t)k_fs_xf_shift_month) | (uint32_t)t.day;
  const uint32_t    time  = ((uint32_t)t.hour << (uint32_t)k_fs_time_shift_hour) |
                            ((uint32_t)t.minute << (uint32_t)k_fs_time_shift_minute) |
                            ((uint32_t)t.second / (uint32_t)k_fs_time_second_div);
  out->stamp              = (date << (uint32_t)k_fs_xf_shift_date) | time;
  out->inc10              = priv_tenths_of(&t);
  /* An offset is a claim about a real instant. The epoch placeholder is not
   * one, so saying "this is UTC" about it would be a fabrication -- exFAT's
   * OffsetValid-clear encoding exists for exactly that case. */
  out->utc = real ? priv_utc_byte(t.utc_offset_min) : (uint8_t)k_fs_utc_unknown;
}

/* =============================================================================
 * Stampers -- what the create / write / rename paths call
 * =============================================================================
 */

/* `priv_fat_entry_stamp_create()`: see header for the documented contract. */
void priv_fat_entry_stamp_create(uint8_t* entry)
{
  fat_stamp_t s = {};
  priv_fat_stamp_now(&s);
  entry[k_dir_off_crt_time_tenth] = s.tenth;
  priv_wr16(&entry[k_dir_off_crt_time], s.time);
  priv_wr16(&entry[k_dir_off_crt_date], s.date);
  priv_wr16(&entry[k_dir_off_lst_acc_date], s.date);
  priv_wr16(&entry[k_dir_off_wrt_time], s.time);
  priv_wr16(&entry[k_dir_off_wrt_date], s.date);
}

/* `priv_fat_entry_stamp_write()`: see header for the documented contract. */
void priv_fat_entry_stamp_write(uint8_t* entry)
{
  fat_stamp_t s = {};
  priv_fat_stamp_now(&s);
  priv_wr16(&entry[k_dir_off_wrt_time], s.time);
  priv_wr16(&entry[k_dir_off_wrt_date], s.date);
  priv_wr16(&entry[k_dir_off_lst_acc_date], s.date);
}

/* `priv_fat_entry_stamp_access()`: see header for the documented contract. */
void priv_fat_entry_stamp_access(uint8_t* entry)
{
  fat_stamp_t s = {};
  priv_fat_stamp_now(&s);
  priv_wr16(&entry[k_dir_off_lst_acc_date], s.date);
}

/* `priv_exfat_file_stamp_create()`: see header for the documented contract. */
void priv_exfat_file_stamp_create(uint8_t* file_entry)
{
  exfat_stamp_t s = {};
  priv_exfat_stamp_now(&s);
  priv_wr32(&file_entry[k_exfat_off_file_ctime], s.stamp);
  priv_wr32(&file_entry[k_exfat_off_file_mtime], s.stamp);
  priv_wr32(&file_entry[k_exfat_off_file_atime], s.stamp);
  file_entry[k_exfat_off_file_c10ms] = s.inc10;
  file_entry[k_exfat_off_file_m10ms] = s.inc10;
  file_entry[k_exfat_off_file_cutc]  = s.utc;
  file_entry[k_exfat_off_file_mutc]  = s.utc;
  file_entry[k_exfat_off_file_autc]  = s.utc;
}

/* `priv_exfat_file_stamp_write()`: see header for the documented contract. */
void priv_exfat_file_stamp_write(uint8_t* file_entry)
{
  exfat_stamp_t s = {};
  priv_exfat_stamp_now(&s);
  priv_wr32(&file_entry[k_exfat_off_file_mtime], s.stamp);
  priv_wr32(&file_entry[k_exfat_off_file_atime], s.stamp);
  file_entry[k_exfat_off_file_m10ms] = s.inc10;
  file_entry[k_exfat_off_file_mutc]  = s.utc;
  file_entry[k_exfat_off_file_autc]  = s.utc;
}

/* `priv_exfat_file_stamp_access()`: see header for the documented contract. */
void priv_exfat_file_stamp_access(uint8_t* file_entry)
{
  exfat_stamp_t s = {};
  priv_exfat_stamp_now(&s);
  priv_wr32(&file_entry[k_exfat_off_file_atime], s.stamp);
  file_entry[k_exfat_off_file_autc] = s.utc;
}

/* =============================================================================
 * Public API: the dependency-injection slot
 * =============================================================================
 */

RA8_DI_SLOT("fs_clock")
ra8_err_t ra8_fs_set_clock(const ra8_fs_clock_t* clock)
{
  if (clock == nullptr) {
    s_clock_bound = false;
    s_clock       = (ra8_fs_clock_t){};
    return k_ra8_ok;
  }
  if (clock->now == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  s_clock       = *clock;
  s_clock_bound = true;
  return k_ra8_ok;
}
