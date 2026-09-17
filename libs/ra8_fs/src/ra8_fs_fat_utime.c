/**
 * @file ra8_fs_fat_utime.c
 * @brief Set a named entry's create / modify / access timestamps (`ra8_fs_utime()`).
 *
 * @details
 * The `touch` / `utime` primitive: unlike the close-time and write-time stamps,
 * which come from the injected clock (`ra8_fs_set_clock()`), these are
 * caller-chosen, so a backup or sync restore can put a file's ORIGINAL
 * create / modify time back instead of the moment of the restore -- which is
 * what every "newest wins" incremental heuristic keys on.
 *
 * FAT stores a create date+time, a modify date+time and an access DATE only
 * (there is no access time). exFAT stores all three as full timestamps plus the
 * 10 ms increments and UtcOffset bytes, and its File-entry SetChecksum covers
 * the timestamp fields, so it is recomputed over the whole entry set after the
 * patch. The actual field packing lives in `ra8_fs_fat_time.c`
 * (::priv_fat_entry_set_times / ::priv_exfat_file_set_times); this file only
 * resolves the entry, applies the patch, and (on exFAT) fixes the checksum.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, section 6 "Directory Structure".
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the exFAT re-read loop is bounded by the set's entry count, itself
 *     `<= k_exfat_set_max_entries`.
 *   - Rule 3: zero malloc; the entry-set buffer is a bounded stack array.
 *   - Rule 7: every backend and resolver call is checked.
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
#include "ra8_fs_meta.h"

/**
 * @brief Set chosen timestamps on a FAT12/16/32 entry.
 *
 * @details Resolves @p path to its directory entry the way ::ra8_fs_stat() does
 *          -- parent walk, then packed 8.3 lookup with a VFAT long-name fallback
 *          -- reads the sector holding the entry, applies the requested
 *          timestamp fields in place, and writes the sector back. A directory
 *          entry is stamped as readily as a file's; only the volume root (which
 *          has no entry) cannot be, and the caller has already rejected it.
 *
 * @param[in] m      Mounted FAT volume.
 * @param[in] path   Path to the entry (never the volume root here).
 * @param[in] create Create stamp, or NULL to leave it unchanged.
 * @param[in] modify Modify stamp, or NULL to leave it unchanged.
 * @param[in] access Access stamp, or NULL to leave it unchanged.
 *
 * @return Error code.
 * @retval k_ra8_ok              Requested stamps written.
 * @retval k_ra8_err_invalid_arg A path component is not a valid 8.3 name.
 * @retval k_ra8_err_not_found   Nothing at @p path.
 * @retval k_ra8_err_*           Backend read/write failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is not exFAT.
 * @pre The mount is in use.
 * @post On success each non-NULL reading's fields hold that instant.
 * @post On failure the entry is unchanged.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_utime_fat(const ra8_fs_mount_t*    m,
                                    const char*              path,
                                    const ra8_fs_datetime_t* create,
                                    const ra8_fs_datetime_t* modify,
                                    const ra8_fs_datetime_t* access)
{
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(m, path, &parent, &leaf);
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
    err = priv_dir_find(m, &parent, name83, &lba, &off, entry);
  }
  if (err == k_ra8_err_not_found) {
    err = priv_dir_find_long(m, &parent, leaf, &lba, &off, entry);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t* const sec = priv_sec_walk();
  err                = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_entry_set_times(&sec[off], create, modify, access);
  return priv_write_sector(m, lba, sec);
}

/**
 * @brief Set chosen timestamps on an exFAT root-level entry.
 *
 * @details Locates the file's directory-entry set, re-reads every entry back
 *          into a buffer (the SetChecksum covers them all), patches the File
 *          entry's timestamps in place, recomputes the SetChecksum over the
 *          whole set, and writes the File entry back -- the only entry whose
 *          bytes changed. Root-directory namespace only, matching every other
 *          exFAT operation here.
 *
 * @param[in] m      Mounted exFAT volume.
 * @param[in] path   Root-level name.
 * @param[in] create Create stamp, or NULL to leave it unchanged.
 * @param[in] modify Modify stamp, or NULL to leave it unchanged.
 * @param[in] access Access stamp, or NULL to leave it unchanged.
 *
 * @return Error code.
 * @retval k_ra8_ok            Requested stamps written.
 * @retval k_ra8_err_not_found No such name in the root directory.
 * @retval k_ra8_err_no_mem    The entry set is longer than the driver rewrites.
 * @retval k_ra8_err_*         Backend read/write failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is exFAT.
 * @pre The mount is in use.
 * @post On success each non-NULL reading's fields hold that instant and the
 *       set's SetChecksum is valid.
 * @post On failure the set is unchanged.
 *
 * @note Bounded loop (NASA Rule 2): the set's entry count, `<= 19`.
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_utime_exfat(const ra8_fs_mount_t*    m,
                                      const char*              path,
                                      const ra8_fs_datetime_t* create,
                                      const ra8_fs_datetime_t* modify,
                                      const ra8_fs_datetime_t* access)
{
  exfat_dir_t root = {};
  priv_exfat_dir_root(m, &root);
  exfat_setpos_t pos[k_exfat_set_max_entries] = {};
  uint32_t       count                        = 0U;
  uint8_t        file_e[k_exfat_entry_bytes]  = {};
  uint8_t        strm_e[k_exfat_entry_bytes]  = {};
  ra8_err_t      err = priv_exfat_find_set(m,
                                           &root,
                                           path,
                                           pos,
                                           (uint32_t)k_exfat_set_max_entries,
                                           &count,
                                           file_e,
                                           strm_e);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t set[(uint32_t)k_exfat_set_max_entries * (uint32_t)k_exfat_entry_bytes] = {};
  for (uint32_t k = 0U; k < count; k++) {
    exfat_cursor_t one = {.cluster          = pos[k].cluster,
                          .entry_in_cluster = pos[k].index,
                          .scanned          = 0U,
                          .contig_end       = 0U};
    err = priv_exfat_next_entry(m, &one, &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  priv_exfat_file_set_times(&set[0], create, modify, access);
  priv_wr16(&set[k_exfat_off_file_csum],
            priv_exfat_set_checksum(set, count * (uint32_t)k_exfat_entry_bytes));
  return priv_exfat_write_dir_set(m,
                                  pos[0].cluster,
                                  pos[0].index,
                                  &set[0],
                                  (uint32_t)k_exfat_entry_bytes);
}

/**
 * @brief Set entry timestamps -- the guarded body of ::ra8_fs_utime().
 *
 * @details Validates the arguments, refuses the volume root (no entry to
 *          stamp), and dispatches to the FAT or exFAT patcher. All-NULL readings
 *          are a no-op success -- a `utime` that changes nothing changed
 *          nothing. The public ::ra8_fs_utime brackets this with the library
 *          lock; the full contract is documented there.
 *
 * @param[in] handle Mount handle.
 * @param[in]     path   Path to the entry.
 * @param[in]     create Create stamp, or NULL.
 * @param[in]     modify Modify stamp, or NULL.
 * @param[in]     access Access stamp, or NULL.
 *
 * @return Error code.
 * @retval k_ra8_ok                Requested stamps written (or none requested).
 * @retval k_ra8_err_null_ptr      @p handle or @p path is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   @p path names the volume root, or is not a
 *                                 valid name for this filesystem.
 * @retval k_ra8_err_not_found     Nothing at @p path.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle and @p path are non-NULL.
 * @post On success the requested stamps hold their (clamped) instants.
 * @post On failure the entry is unchanged.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_utime_locked(const ra8_fs_mount_t*    handle,
                                       const char*              path,
                                       const ra8_fs_datetime_t* create,
                                       const ra8_fs_datetime_t* modify,
                                       const ra8_fs_datetime_t* access)
{
  if (handle == nullptr || path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  if (*p == '\0') {
    return k_ra8_err_invalid_arg; /* the volume root has no entry to stamp */
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return internal_utime_exfat(handle, path, create, modify, access);
  }
  return internal_utime_fat(handle, path, create, modify, access);
}

/* =============================================================================
 * Public entry point -- the lock bracket
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_utime(const ra8_fs_mount_t*    handle,
                       const char*              path,
                       const ra8_fs_datetime_t* create,
                       const ra8_fs_datetime_t* modify,
                       const ra8_fs_datetime_t* access)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_utime_locked(handle, path, create, modify, access);
  priv_lock_release();
  return err;
}
