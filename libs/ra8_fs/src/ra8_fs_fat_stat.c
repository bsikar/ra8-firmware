/**
 * @file ra8_fs_fat_stat.c
 * @brief Path metadata lookup -- what is at this path, without opening it.
 *
 * @details
 * `stat` resolves a path exactly as `open` does and then stops at the
 * directory entry, reading the attribute byte, the size and the first cluster
 * straight out of it. Not opening anything is the whole point: a directory
 * opens perfectly well and reports `DIR_FileSize` 0, so an open-based `stat`
 * cannot tell a folder from an empty file -- and it spends one of the four
 * file-table slots to fail at it.
 *
 * Split out of `ra8_fs_fat_dir.c` for the 1000-line file-size cap.
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

/** @brief FAT/exFAT packed timestamp field geometry and validation bounds. */
typedef enum : uint16_t {
  k_stat_fat_year_shift     = 9U,      /**< FAT year-field shift.                 */
  k_stat_fat_month_shift    = 5U,      /**< FAT month-field shift.                */
  k_stat_fat_month_mask     = 0x0FU,   /**< FAT month-field mask.                 */
  k_stat_fat_day_mask       = 0x1FU,   /**< FAT day-field mask.                   */
  k_stat_fat_hour_shift     = 11U,     /**< FAT hour-field shift.                 */
  k_stat_fat_hour_mask      = 0x1FU,   /**< FAT hour-field mask.                  */
  k_stat_fat_minute_shift   = 5U,      /**< FAT minute-field shift.               */
  k_stat_fat_minute_mask    = 0x3FU,   /**< FAT minute-field mask.                */
  k_stat_fat_second_mask    = 0x1FU,   /**< FAT half-second-field mask.           */
  k_stat_tenths_per_second  = 100U,    /**< Ten-millisecond units per second.     */
  k_stat_month_max          = 12U,     /**< Largest civil month.                  */
  k_stat_hour_max           = 23U,     /**< Largest civil hour.                   */
  k_stat_minute_max         = 59U,     /**< Largest civil minute.                 */
  k_stat_second_max         = 59U,     /**< Largest civil second.                 */
  k_stat_creation_tenth_max = 199U,    /**< Largest legal FAT creation increment. */
  k_stat_packed_time_mask   = 0xFFFFU, /**< Low packed time word.                 */
  k_stat_utc_sign_bit       = 0x40U,   /**< Sign bit in the seven-bit UTC field.  */
  k_stat_utc_field_modulus  = 128U,    /**< Two's-complement UTC field modulus.   */
} fs_stat_field_t;

/**
 * @brief Does @p path name the volume root rather than an entry inside it?
 *
 * @details The root has no directory entry of its own, so it cannot be looked
 *          up; it is answered from the mount geometry instead. Any run of
 *          leading slashes with nothing after it is the root, which covers
 *          both `""` and `"/"`.
 *
 * @param[in] path NUL-terminated path.
 *
 * @return bool true when @p path names the root directory.
 * @retval true  Only slashes (or nothing at all).
 * @retval false At least one name character follows.
 *
 * @pre @p path is non-NULL.
 * @pre @p path is NUL-terminated.
 * @post No state modified.
 * @post The result is purely a function of @p path.
 *
 * @note Pure function; bounded by the string's own terminator.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_path_is_root(const char* path)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  return *p == '\0';
}

/**
 * @brief Is a decoded FAT civil date/time within every legal calendar bound?
 *
 * @details Checks month, day, hour, minute, second, and the optional
 *          creation-centisecond byte. There is deliberately no upper-bound
 *          check on the day: FAT stores it in five bits, so
 *          `date & k_stat_fat_day_mask` cannot exceed 31, which is already
 *          the largest civil day. Only `day == 0` is representable and
 *          illegal, and that is the guard below. Every check here is a
 *          single-condition `if`.
 *
 * @param[in] v          Decoded (not yet validated) civil fields.
 * @param[in] tenth      FAT 10-ms byte (creation only), or zero.
 * @param[in] have_tenth true when @p tenth belongs to the stamp.
 *
 * @return bool Whether every checked field is within its legal civil range.
 * @retval true  Every field is legal.
 * @retval false At least one field is out of range.
 *
 * @pre @p v is non-NULL.
 * @pre @p v was decoded from a raw packed FAT date/time pair.
 * @post No state is modified.
 * @post The result is purely a function of @p v, @p tenth, and @p have_tenth.
 *
 * @note Pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_stat_fat_time_valid(const ra8_fs_datetime_t* v, uint8_t tenth, bool have_tenth)
{
  if (v->month == 0U) {
    return false;
  }
  if (v->month > (uint8_t)k_stat_month_max) {
    return false;
  }
  if (v->day == 0U) {
    return false;
  }
  if (v->hour > (uint8_t)k_stat_hour_max) {
    return false;
  }
  if (v->minute > (uint8_t)k_stat_minute_max) {
    return false;
  }
  if (v->second > (uint8_t)k_stat_second_max) {
    return false;
  }
  if (have_tenth) {
    if (tenth > (uint8_t)k_stat_creation_tenth_max) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Decode one FAT-format packed date/time pair into a public timestamp.
 *
 * @details Reconstructs FAT's even-second field and optional creation
 *          centiseconds, then rejects every out-of-range calendar component
 *          through ::internal_stat_fat_time_valid.
 *
 * @param[in]  date       Packed FAT date word.
 * @param[in]  time       Packed FAT time word (zero for date-only access time).
 * @param[in]  tenth      FAT 10-ms byte (creation only), or zero.
 * @param[in]  have_tenth true when @p tenth belongs to the stamp.
 * @param[out] out        Decoded timestamp; invalid when the fields are illegal.
 *
 * @return Nothing.
 * @pre @p out is non-NULL.
 * @pre @p date and @p time are raw little-endian values already read as host integers.
 * @post `out->valid` is true exactly when every packed field is legal.
 * @post `out->utc_offset_valid` is false because FAT stores no zone.
 * @note Pure decode; FAT carries no UTC-offset field.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stat_decode_fat(uint16_t            date,
                                     uint16_t            time,
                                     uint8_t             tenth,
                                     bool                have_tenth,
                                     ra8_fs_timestamp_t* out)
{
  ra8_fs_datetime_t v = {};
  v.year   = (uint16_t)((uint32_t)k_fs_time_epoch_year + (date >> k_stat_fat_year_shift));
  v.month  = (uint8_t)((date >> k_stat_fat_month_shift) & (uint16_t)k_stat_fat_month_mask);
  v.day    = (uint8_t)(date & (uint16_t)k_stat_fat_day_mask);
  v.hour   = (uint8_t)((time >> k_stat_fat_hour_shift) & (uint16_t)k_stat_fat_hour_mask);
  v.minute = (uint8_t)((time >> k_stat_fat_minute_shift) & (uint16_t)k_stat_fat_minute_mask);
  v.second = (uint8_t)((time & (uint16_t)k_stat_fat_second_mask) * 2U);
  if (have_tenth) {
    v.second      = (uint8_t)(v.second + ((tenth >= (uint8_t)k_stat_tenths_per_second) ? 1U : 0U));
    v.centisecond = (uint8_t)(tenth % (uint8_t)k_stat_tenths_per_second);
  }
  if (!internal_stat_fat_time_valid(&v, tenth, have_tenth)) {
    *out = (ra8_fs_timestamp_t){};
    return;
  }
  out->value            = v;
  out->valid            = true;
  out->utc_offset_valid = false;
}

/**
 * @brief Decode one exFAT packed stamp, 10-ms increment, and UTC-offset byte.
 *
 * @details Uses the FAT-compatible packed civil fields, then decodes exFAT's
 *          signed 15-minute UTC offset only when its validity bit is set.
 *
 * @param[in]  packed     exFAT 32-bit date/time value.
 * @param[in]  tenth      Optional 10-ms increment.
 * @param[in]  have_tenth true for create/modify, false for access.
 * @param[in]  utc        exFAT UtcOffset byte.
 * @param[out] out        Decoded timestamp.
 *
 * @return Nothing.
 * @pre @p out is non-NULL.
 * @pre @p packed, @p tenth, and @p utc are raw fields from one File entry.
 * @post Invalid calendar bytes produce an all-zero, invalid result.
 * @post A clear/invalid UTC field never masquerades as UTC+00:00.
 * @note Pure decode.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stat_decode_exfat(uint32_t            packed,
                                       uint8_t             tenth,
                                       bool                have_tenth,
                                       uint8_t             utc,
                                       ra8_fs_timestamp_t* out)
{
  const uint16_t date = (uint16_t)(packed >> 16U);
  const uint16_t time = (uint16_t)(packed & (uint32_t)k_stat_packed_time_mask);
  internal_stat_decode_fat(date, time, tenth, have_tenth, out);
  if (!out->valid) {
    return;
  }
  if ((utc & (uint8_t)k_fs_utc_valid_bit) == 0U) {
    return;
  }
  int32_t steps = (int32_t)(utc & (uint8_t)k_fs_utc_field_mask);
  if ((steps & (int32_t)k_stat_utc_sign_bit) != 0) {
    steps -= (int32_t)k_stat_utc_field_modulus;
  }
  const int32_t minutes = steps * (int32_t)k_fs_utc_step_min;
  if (minutes < (int32_t)k_fs_utc_span_min) {
    return;
  }
  if (minutes > (int32_t)k_fs_utc_span_max) {
    return;
  }
  out->value.utc_offset_min = (int16_t)minutes;
  out->utc_offset_valid     = true;
}

/**
 * @brief Decode all three timestamp fields from one FAT directory entry.
 *
 * @details Maps creation, modification, and date-only access fields into the
 *          corresponding public stat timestamps without mutating the entry.
 *
 * @param[in]  entry FAT directory entry of at least 32 bytes.
 * @param[out] out   Stat result receiving the three timestamps.
 *
 * @return Nothing.
 * @pre @p entry is non-NULL and readable for one directory entry.
 * @pre @p out is non-NULL and writable.
 * @post All three timestamp members have been assigned.
 * @post No non-timestamp stat member is modified.
 * @note Pure decode; invalid foreign fields remain representable as invalid.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stat_fat_times(const uint8_t* entry, ra8_fs_stat_t* out)
{
  internal_stat_decode_fat(priv_rd16(&entry[k_dir_off_crt_date]),
                           priv_rd16(&entry[k_dir_off_crt_time]),
                           entry[k_dir_off_crt_time_tenth],
                           true,
                           &out->created);
  internal_stat_decode_fat(priv_rd16(&entry[k_dir_off_wrt_date]),
                           priv_rd16(&entry[k_dir_off_wrt_time]),
                           0U,
                           false,
                           &out->modified);
  internal_stat_decode_fat(priv_rd16(&entry[k_dir_off_lst_acc_date]),
                           0U,
                           0U,
                           false,
                           &out->accessed);
}

/**
 * @brief Decode all three timestamp fields from one exFAT File entry.
 *
 * @details Maps creation, modification, and access stamps plus their UTC
 *          markers into the corresponding public stat timestamps.
 *
 * @param[in]  entry exFAT File entry of at least 32 bytes.
 * @param[out] out   Stat result receiving the three timestamps.
 *
 * @return Nothing.
 * @pre @p entry is non-NULL and readable for one File entry.
 * @pre @p out is non-NULL and writable.
 * @post All three timestamp members have been assigned.
 * @post No non-timestamp stat member is modified.
 * @note Pure decode; an unknown UTC offset never becomes a fabricated zero offset.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stat_exfat_times(const uint8_t* entry, ra8_fs_stat_t* out)
{
  internal_stat_decode_exfat(priv_rd32(&entry[k_exfat_off_file_ctime]),
                             entry[k_exfat_off_file_c10ms],
                             true,
                             entry[k_exfat_off_file_cutc],
                             &out->created);
  internal_stat_decode_exfat(priv_rd32(&entry[k_exfat_off_file_mtime]),
                             entry[k_exfat_off_file_m10ms],
                             true,
                             entry[k_exfat_off_file_mutc],
                             &out->modified);
  internal_stat_decode_exfat(priv_rd32(&entry[k_exfat_off_file_atime]),
                             0U,
                             false,
                             entry[k_exfat_off_file_autc],
                             &out->accessed);
}

/**
 * @brief Fill in @p out for the volume root itself.
 *
 * @details The root always exists and is always a directory. Its
 *          `first_cluster` is the FAT32 / exFAT root cluster, and 0 on
 *          FAT12/16 where the root is a fixed sector region with no chain --
 *          which is the same "no chain" value an empty file carries.
 *
 * @param[in]  m   Mounted volume supplying the root cluster.
 * @param[out] out Receives the metadata of the root.
 *
 * @return Nothing.
 *
 * @pre @p m and @p out are non-NULL.
 * @pre @p m is in use.
 * @post `out->is_directory` is true and `out->size_bytes` is 0.
 * @post No volume state is modified.
 *
 * @note Reads only cached geometry; touches no sector.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stat_root(const ra8_fs_mount_t* m, ra8_fs_stat_t* out)
{
  out->size_bytes    = 0U;
  out->first_cluster = m->root_cluster;
  out->attr          = (uint8_t)k_ra8_fs_attr_directory;
  out->is_directory  = true;
}

/**
 * @brief Translate a 32-byte FAT directory entry into a ::ra8_fs_stat_t.
 *
 * @details Copies the attribute byte through verbatim -- read-only, hidden and
 *          system survive, where the old VFS `stat` invented `archive` for
 *          every entry -- and derives `is_directory` from the ATTR_DIRECTORY
 *          bit rather than asserting it is clear. A directory's `DIR_FileSize`
 *          is 0 by specification; it is forced to 0 here anyway so a volume
 *          written by some other implementation cannot report a directory with
 *          a non-zero length.
 *
 * @param[in]  entry 32 bytes of on-disk directory entry.
 * @param[out] out   Receives the decoded metadata.
 *
 * @return Nothing.
 *
 * @pre @p entry addresses 32 readable bytes.
 * @pre @p out is non-NULL.
 * @post `out->is_directory` matches the entry's ATTR_DIRECTORY bit.
 * @post `out->size_bytes` is 0 whenever `out->is_directory` is true.
 *
 * @note Pure decode; touches no backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_entry_to_stat(const uint8_t* entry, ra8_fs_stat_t* out)
{
  const uint8_t attr = entry[k_dir_off_attr];
  out->attr          = attr;
  out->is_directory  = (attr & (uint8_t)k_ra8_fs_attr_directory) != 0U;
  out->first_cluster = priv_entry_first_cluster(entry);
  out->size_bytes    = priv_rd32(&entry[k_dir_off_file_size]);
  internal_stat_fat_times(entry, out);
  if (out->is_directory) {
    out->size_bytes = 0U;
  }
}

/**
 * @brief `stat` a name on an exFAT volume, at any depth.
 *
 * @details Reuses ::priv_exfat_lookup, which resolves the path's intermediate
 *          components and hands back the whole Stream entry plus the File
 *          entry's attribute byte, so a directory is told from a file by the
 *          bit that says so and the lengths come out of the same 32 bytes.
 *
 * @param[in]  m    Mounted exFAT volume.
 * @param[in]  path Path, UTF-8, nested or root-level.
 * @param[out] out  Receives the metadata of the entry.
 *
 * @return Error code.
 * @retval k_ra8_ok              Entry found; @p out populated.
 * @retval k_ra8_err_not_found   No such name, or a component is missing.
 * @retval k_ra8_err_invalid_arg An intermediate component names a file.
 * @retval k_ra8_err_*           Backend read failure.
 *
 * @pre @p m, @p path and @p out are non-NULL; `m->type` is exFAT.
 * @pre The volume is mounted.
 * @post On success `out->attr` is the entry's own FileAttributes low byte.
 * @post No volume state is modified.
 *
 * @note The volume root is answered by the caller, before this is reached.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stat_exfat(const ra8_fs_mount_t* m, const char* path, ra8_fs_stat_t* out)
{
  exfat_dir_t     parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t pe     = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (pe != k_ra8_ok) {
    return pe;
  }
  exfat_setpos_t  pos[k_exfat_set_max_entries] = {};
  uint32_t        count                        = 0U;
  uint8_t         file_e[k_exfat_entry_bytes]  = {};
  uint8_t         strm[k_exfat_entry_bytes]    = {};
  const ra8_err_t err = priv_exfat_find_set(m,
                                            &parent,
                                            leaf,
                                            pos,
                                            (uint32_t)k_exfat_set_max_entries,
                                            &count,
                                            file_e,
                                            strm);
  if (err != k_ra8_ok) {
    return err;
  }
  out->attr          = file_e[k_exfat_off_file_attr];
  out->is_directory  = (out->attr & (uint8_t)k_ra8_fs_attr_directory) != 0U;
  out->first_cluster = priv_rd32(&strm[k_exfat_strm_off_clus]);
  out->size_bytes    = priv_rd64(&strm[k_exfat_strm_off_dlen]);
  internal_stat_exfat_times(file_e, out);
  if (out->is_directory) {
    out->size_bytes = 0U;
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve one name in a FAT12/16/32 directory and decode its entry.
 *
 * @details The leaf is matched by packed 8.3 name and, failing that, by VFAT
 *          long name -- the same two-step ::priv_open_locked() uses, so `stat`
 *          and `open` agree about what exists. Split out of
 *          ::priv_stat_locked() so each stays inside the function-size gate.
 *
 * @param[in]  handle Mounted FAT volume.
 * @param[in]  path   NUL-terminated path; never the volume root.
 * @param[out] out    Receives the metadata of the entry.
 *
 * @return Error code.
 * @retval k_ra8_ok              Entry found; @p out populated.
 * @retval k_ra8_err_not_found   Nothing at @p path.
 * @retval k_ra8_err_invalid_arg A component is not a valid 8.3 name.
 * @retval k_ra8_err_*           Backend read failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle, @p path and @p out are non-NULL; the mount is in use.
 * @post On success @p out describes the entry at @p path.
 * @post No volume state is modified and no file slot is consumed.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
internal_stat_fat(const ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
{
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(handle, path, &parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  uint8_t       name83[k_max_8_3_name]          = {};
  const uint8_t have83                          = priv_path_to_83(leaf, name83);
  uint64_t      lba                             = 0U;
  uint32_t      off                             = 0U;
  uint8_t       entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t     err                             = k_ra8_err_not_found;
  if (have83 != 0U) {
    err = priv_dir_find(handle, &parent, name83, &lba, &off, entry);
  }
  if (err == k_ra8_err_not_found) {
    /* Same long-name fallback as priv_open_locked: a leaf that is not
     * 8.3-representable still resolves through its VFAT chain. */
    err = priv_dir_find_long(handle, &parent, leaf, &lba, &off, entry);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  internal_entry_to_stat(entry, out);
  return k_ra8_ok;
}

/**
 * @brief Look a path up -- the guarded body of ::ra8_fs_stat().
 *
 * @details Validates the arguments, answers the volume root out of the mount
 *          geometry (the root has no directory entry to read), and otherwise
 *          hands off to the exFAT or the FAT lookup. Nothing is opened on any
 *          path, so no file-table slot is consumed and a directory never has
 *          to be mistaken for a zero-byte file to be reported at all.
 *
 * @param[in]  handle Mount handle.
 * @param[in]  path   NUL-terminated path.
 * @param[out] out    Receives the metadata of the entry.
 *
 * @return Error code.
 * @retval k_ra8_ok                Entry found; @p out populated.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_not_found     Nothing at @p path.
 * @retval k_ra8_err_*             As documented for ::ra8_fs_stat().
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle`, `path` and `out` are non-NULL.
 * @post On success `out` describes the entry at @p path.
 * @post No volume state is modified and no file slot is consumed.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
internal_stat_locked(const ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  *out = (ra8_fs_stat_t){};
  if (internal_path_is_root(path)) {
    internal_stat_root(handle, out);
    return k_ra8_ok;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return internal_stat_exfat(handle, path, out);
  }
  return internal_stat_fat(handle, path, out);
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_stat(const ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_stat_locked(handle, path, out);
  priv_lock_release();
  return err;
}
