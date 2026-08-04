/**
 * @file ra8_fs_fat_time_internal.h
 * @brief On-disk timestamp field offsets, packing constants, and stampers.
 * @ingroup grp_storage
 *
 * @details
 * The module-private half of the wall-clock seam (`ra8_fs_set_clock()`). It
 * carries the byte offsets of the timestamp fields inside a 32-byte FAT
 * directory entry and inside an exFAT File entry, the bit layout both formats
 * pack a date and a time into, and the five stamping helpers the create /
 * write / rename paths call.
 *
 * The stampers deliberately take a raw entry pointer rather than a mount or a
 * file handle: a timestamp is a property of the 32 bytes being assembled, and
 * keeping it that way means the helper can be applied to an entry that is
 * still in a stack buffer (a fresh directory slot, a "." link, an exFAT entry
 * set being built) as easily as to one just read back off the volume.
 *
 * Pulled in by the `ra8_fs_fat_internal.h` umbrella, so every `ra8_fs_fat*.c`
 * file sees it; included by nothing outside this module.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000.
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, section 7.4 "File Directory Entry".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"

/**
 * @enum ra8_fs_time_off_t
 * @brief Byte offsets of the timestamp fields in a 32-byte FAT directory entry.
 *
 * @details MS FAT spec sec 6, table "Directory Entry". These are the fields
 *          `priv_write_new_dir_entry()` used to leave as zeros: a zero
 *          `DIR_CrtDate` / `DIR_WrtDate` encodes month 0 and day 0, and both
 *          are 1-based, so it is not a date any host can agree on.
 *
 * @invariant Every offset addresses a field wholly inside one 32-byte entry.
 * @see priv_fat_entry_stamp_create()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dir_off_crt_time_tenth = 13, /**< MS FAT spec sec 6 "DIR_CrtTimeTenth". */
  k_dir_off_crt_time       = 14, /**< MS FAT spec sec 6 "DIR_CrtTime".      */
  k_dir_off_crt_date       = 16, /**< MS FAT spec sec 6 "DIR_CrtDate".      */
  k_dir_off_lst_acc_date   = 18, /**< MS FAT spec sec 6 "DIR_LstAccDate".   */
  k_dir_off_wrt_time       = 22, /**< MS FAT spec sec 6 "DIR_WrtTime".      */
  k_dir_off_wrt_date       = 24, /**< MS FAT spec sec 6 "DIR_WrtDate".      */
} ra8_fs_time_off_t;

/**
 * @enum ra8_fs_exfat_time_off_t
 * @brief Byte offsets of the timestamp fields in an exFAT File entry (0x85).
 *
 * @details exFAT spec sec 7.4. The three 32-bit stamps share one packed
 *          layout (::ra8_fs_time_pack_t); the 10ms fields recover the odd
 *          second and the hundredths that the 2-second stamp granularity
 *          throws away; the UtcOffset bytes say which zone the civil time in
 *          the stamp belongs to.
 *
 * @invariant Every offset addresses a field wholly inside one 32-byte entry.
 * @see priv_exfat_file_stamp_create()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_exfat_off_file_ctime = 8,  /**< exFAT spec sec 7.4.8  "CreateTimestamp".       */
  k_exfat_off_file_mtime = 12, /**< exFAT spec sec 7.4.9  "LastModifiedTimestamp". */
  k_exfat_off_file_atime = 16, /**< exFAT spec sec 7.4.10 "LastAccessedTimestamp". */
  k_exfat_off_file_c10ms = 20, /**< exFAT spec sec 7.4.11 "Create10msIncrement".   */
  k_exfat_off_file_m10ms = 21, /**< exFAT spec sec 7.4.12 "LastModified10ms".      */
  k_exfat_off_file_cutc  = 22, /**< exFAT spec sec 7.4.13 "CreateUtcOffset".       */
  k_exfat_off_file_mutc  = 23, /**< exFAT spec sec 7.4.14 "LastModifiedUtcOffset". */
  k_exfat_off_file_autc  = 24, /**< exFAT spec sec 7.4.15 "LastAccessedUtcOffset". */
} ra8_fs_exfat_time_off_t;

/**
 * @enum ra8_fs_time_pack_t
 * @brief Bit positions, field ranges, and epoch constants for both packings.
 *
 * @details FAT splits the stamp across two 16-bit words (MS FAT spec sec 6):
 *          date = `year-1980 << 9 | month << 5 | day`, time =
 *          `hour << 11 | minute << 5 | second/2`. exFAT packs the same six
 *          fields into one 32-bit word (exFAT spec sec 7.4.8) with the time
 *          in the low half and the date in the high half, which is why the
 *          two share the sub-field shifts below.
 *
 * @invariant The epoch is 1980 in both formats; the year field is 7 bits, so
 *            2107 is the last representable year.
 * @see priv_fat_entry_stamp_create()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fs_time_epoch_year     = 1980U,   /**< Year 0 of both on-disk formats.        */
  k_fs_time_year_max       = 2107U,   /**< 1980 + 127: last representable year.   */
  k_fs_time_month_min      = 1U,      /**< Month is 1-based on disk.              */
  k_fs_time_month_max      = 12U,     /**< Highest month.                         */
  k_fs_time_day_min        = 1U,      /**< Day is 1-based on disk.                */
  k_fs_time_day_max        = 31U,     /**< Highest day the field can express.     */
  k_fs_time_hour_max       = 23U,     /**< Highest hour.                          */
  k_fs_time_minute_max     = 59U,     /**< Highest minute.                        */
  k_fs_time_second_max     = 59U,     /**< Highest second.                        */
  k_fs_time_centi_max      = 99U,     /**< Highest hundredth within a second.     */
  k_fs_date_shift_year     = 9U,      /**< FAT date: year field position.         */
  k_fs_date_shift_month    = 5U,      /**< FAT date: month field position.        */
  k_fs_time_shift_hour     = 11U,     /**< FAT time: hour field position.         */
  k_fs_time_shift_minute   = 5U,      /**< FAT time: minute field position.       */
  k_fs_time_second_div     = 2U,      /**< FAT/exFAT store seconds/2.             */
  k_fs_xf_shift_date       = 16U,     /**< exFAT stamp: date half position.       */
  k_fs_xf_shift_month      = 5U,      /**< exFAT date half: month field position. */
  k_fs_xf_shift_year       = 9U,      /**< exFAT date half: year field position.  */
  k_fs_centi_per_second    = 100U,    /**< Hundredths in one second.              */
  k_fs_utc_step_min        = 15U,     /**< exFAT UtcOffset granularity, minutes.  */
  k_fs_utc_valid_bit       = 0x80U,   /**< exFAT UtcOffset bit 7 = OffsetValid.   */
  k_fs_utc_field_mask      = 0x7FU,   /**< exFAT UtcOffset bits 6:0 = the offset. */
  k_fs_utc_unknown         = 0x00U,   /**< OffsetValid clear: zone not recorded.  */
  k_fs_time_epoch_fat_date = 0x0021U, /**< 1980-01-01 packed: month 1, day 1.     */
} ra8_fs_time_pack_t;

/**
 * @enum ra8_fs_utc_span_t
 * @brief The signed range of UTC offsets exFAT's 7-bit field can express.
 *
 * @details Kept apart from ::ra8_fs_time_pack_t because these are the only
 *          negative constants in the timestamp layer and that enum's
 *          underlying type is unsigned. UTC-12:00 and UTC+14:00 are the real
 *          extremes of civil time on Earth, and both land inside the 7-bit
 *          two's-complement field (-48 and +56 fifteen-minute steps).
 *
 * @invariant Both bounds are whole multiples of ::k_fs_utc_step_min.
 * @see priv_exfat_file_stamp_create()
 * @since 0.1.0
 */
typedef enum : int16_t {
  k_fs_utc_span_min = -720, /**< UTC-12:00 expressed in minutes. */
  k_fs_utc_span_max = 840,  /**< UTC+14:00 expressed in minutes. */
} ra8_fs_utc_span_t;

/**
 * @brief Stamp a fresh FAT directory entry's create, write, and access fields.
 *
 * @details Reads the installed clock once (or falls back to the FAT epoch) and
 *          writes `DIR_CrtTimeTenth`, `DIR_CrtTime`, `DIR_CrtDate`,
 *          `DIR_LstAccDate`, `DIR_WrtTime` and `DIR_WrtDate` from that single
 *          reading, so a newly created file's three dates agree with each
 *          other instead of straddling a clock tick.
 *
 * @param[in,out] entry 32-byte directory entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p entry is non-NULL and addresses 32 writable bytes.
 * @pre The other fields of @p entry (name, attribute, cluster) are already set
 *      or will be set without disturbing offsets 13..25.
 * @post Every timestamp field of @p entry holds a legal calendar value.
 * @post No field outside offsets 13..25 is modified.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fat_entry_stamp_create(uint8_t* entry);

/**
 * @brief Advance a FAT directory entry's modification time and access date.
 *
 * @details The write half: `DIR_WrtTime` / `DIR_WrtDate` (what every host
 *          shows as "date modified" and what every backup and sync tool keys
 *          on) plus `DIR_LstAccDate`, because writing is also accessing. The
 *          create fields are left exactly as they are -- a file created on a
 *          PC keeps the PC's creation date.
 *
 * @param[in,out] entry 32-byte directory entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p entry is non-NULL and addresses 32 writable bytes.
 * @pre @p entry was read back from the volume (or is being assembled for it).
 * @post `DIR_WrtTime`, `DIR_WrtDate` and `DIR_LstAccDate` hold the current stamp.
 * @post The create fields at offsets 13..17 are unchanged.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fat_entry_stamp_write(uint8_t* entry);

/**
 * @brief Advance only a FAT directory entry's last-access date.
 *
 * @details What a rename gets. A rename changes the name, not the bytes, so
 *          advancing `DIR_WrtDate` would tell every `rsync`, backup and OTA
 *          "newest image" heuristic that the contents changed -- the exact
 *          class of false positive #601 exists to remove, only inverted. FAT
 *          has no separate metadata-change field, so the honest record of
 *          "this entry was touched" is the access date, which is also the only
 *          access field FAT has (there is no last-access TIME).
 *
 * @param[in,out] entry 32-byte directory entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p entry is non-NULL and addresses 32 writable bytes.
 * @pre @p entry was read back from the volume.
 * @post `DIR_LstAccDate` holds today's packed date.
 * @post Every other byte of @p entry is unchanged.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fat_entry_stamp_access(uint8_t* entry);

/**
 * @brief Stamp a fresh exFAT File entry's create, modify, and access fields.
 *
 * @details The exFAT counterpart of ::priv_fat_entry_stamp_create, and it has
 *          three more fields to get right than FAT does: the 10ms increments
 *          that recover the odd second the 2-second stamp granularity drops,
 *          and the UtcOffset bytes that say which zone the civil time belongs
 *          to. An offset the format cannot express (not a whole 15-minute
 *          step, or outside UTC-12:00..UTC+14:00) is recorded as
 *          ::k_fs_utc_unknown -- OffsetValid clear -- rather than guessed.
 *
 * @param[in,out] file_entry 32-byte exFAT File (0x85) entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p file_entry is non-NULL and addresses 32 writable bytes.
 * @pre @p file_entry is a File entry; the caller recomputes SetChecksum after.
 * @post All three stamps, both 10ms fields, and all three UtcOffset bytes are set.
 * @post No byte outside offsets 8..24 is modified.
 *
 * @warning The entry set's SetChecksum covers these bytes -- recompute it
 *          after stamping or the volume fails a host `fsck`.
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_file_stamp_create(uint8_t* file_entry);

/**
 * @brief Advance only an exFAT File entry's last-accessed stamp.
 *
 * @details The exFAT counterpart of ::priv_fat_entry_stamp_access, taken by
 *          the rename path for the same reason: the name changed, the bytes
 *          did not. Writes `LastAccessedTimestamp` and its UtcOffset byte;
 *          exFAT has no last-accessed 10ms field, so there is none to write.
 *
 * @param[in,out] file_entry 32-byte exFAT File (0x85) entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p file_entry is non-NULL and addresses 32 writable bytes.
 * @pre @p file_entry was read back from the volume as part of an entry set.
 * @post `LastAccessedTimestamp` and `LastAccessedUtcOffset` hold the current stamp.
 * @post The create and modify fields are unchanged.
 *
 * @warning The entry set's SetChecksum covers these bytes -- recompute it after.
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_file_stamp_access(uint8_t* file_entry);

/**
 * @brief Advance an exFAT File entry's modification (and access) stamps.
 *
 * @details The exFAT counterpart of ::priv_fat_entry_stamp_write, taken by
 *          every flush of a streaming write. It moves
 *          `LastModifiedTimestamp`, its 10 ms increment and its UtcOffset --
 *          the three fields that together say WHEN the bytes on the card were
 *          put there -- and `LastAccessedTimestamp` with them, because a write
 *          is an access. The creation fields are deliberately untouched: a
 *          truncate-in-place keeps a file's identity, and its birthday is part
 *          of that.
 *
 * @param[in,out] file_entry 32-byte exFAT File (0x85) entry to stamp in place.
 *
 * @return Nothing.
 *
 * @pre @p file_entry is non-NULL and addresses 32 writable bytes.
 * @pre @p file_entry was read back from the volume as part of an entry set.
 * @post The modified and accessed stamps name the current instant.
 * @post `CreateTimestamp`, `Create10msIncrement` and `CreateUtcOffset` are
 *       unchanged.
 *
 * @warning The entry set's SetChecksum covers these bytes -- recompute it
 *          after stamping or the volume fails a host `fsck`.
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_file_stamp_write(uint8_t* file_entry);
