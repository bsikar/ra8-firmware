/**
 * @file ra8_fs_fat_attr.c
 * @brief Set / clear a named entry's file attributes (`ra8_fs_set_attr()`).
 *
 * @details
 * The chmod-style primitive for the FAT/exFAT attribute byte. `ra8_fs_stat()`
 * already REPORTS the read-only / hidden / system / archive bits; this file is
 * how they are CHANGED. Its first job is the read-only bit: putting it on a file
 * makes every mutating path (`open` for writing, `write`, `write_file`,
 * `unlink`, `rename`) refuse the file with ::k_ra8_err_access_denied, and taking
 * it off restores ordinary access (#681).
 *
 * Only the four host-controlled bits are settable. The same byte also carries
 * the DIRECTORY, VOLUME_ID and long-name bits, which say what an entry IS rather
 * than how a host wants it treated -- patching those would reclassify the entry
 * and corrupt the volume -- so a mask naming any of them, or naming one bit in
 * both the set and clear masks, is rejected before a sector is touched.
 *
 * On FAT the byte is patched in the directory entry in place. On exFAT it is the
 * low byte of the File entry's FileAttributes field, and because the entry set's
 * SetChecksum covers it, the whole set is re-read, patched and re-checksummed --
 * exactly as `ra8_fs_utime()` does for the timestamp fields. exFAT operations
 * here are root-directory only, matching that sibling.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, section 7 "Directory Structure".
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
 * @brief Patch the attribute byte of a FAT12/16/32 entry.
 *
 * @details Resolves @p path to its directory entry the way ::ra8_fs_stat() does
 *          -- parent walk, then packed 8.3 lookup with a VFAT long-name fallback
 *          -- reads the sector holding the entry, rewrites the `DIR_Attr` byte to
 *          `(attr & ~clear_mask) | set_mask`, and writes the sector back. Only
 *          the attribute byte changes; the name, cluster, size and timestamps
 *          are untouched. A directory entry is patched as readily as a file's.
 *
 * @param[in] m          Mounted FAT volume.
 * @param[in] path       Path to the entry (never the volume root here).
 * @param[in] set_mask   Attribute bits to set.
 * @param[in] clear_mask Attribute bits to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok              The attribute byte was patched.
 * @retval k_ra8_err_invalid_arg A path component is not a valid 8.3 name.
 * @retval k_ra8_err_not_found   Nothing at @p path.
 * @retval k_ra8_err_*           Backend read/write failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is not exFAT.
 * @pre The mount is in use and the masks have already been validated.
 * @post On success the entry's attribute byte is `(old & ~clear) | set`.
 * @post On failure the entry is unchanged.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setattr_fat(const ra8_fs_mount_t* m,
                                      const char*           path,
                                      uint8_t               set_mask,
                                      uint8_t               clear_mask)
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
  priv_fat_entry_apply_attr(&sec[off], set_mask, clear_mask);
  return priv_write_sector(m, lba, sec);
}

/**
 * @brief Patch the FileAttributes byte of an exFAT root-level entry.
 *
 * @details Locates the file's directory-entry set, re-reads every entry back
 *          into a buffer (the SetChecksum covers them all), rewrites the low
 *          byte of the File entry's FileAttributes to `(attr & ~clear) | set`,
 *          recomputes the SetChecksum over the whole set, and writes the File
 *          entry back -- the only entry whose bytes changed. Root-directory
 *          namespace only, matching every other exFAT operation here.
 *
 * @param[in] m          Mounted exFAT volume.
 * @param[in] path       Root-level name.
 * @param[in] set_mask   Attribute bits to set.
 * @param[in] clear_mask Attribute bits to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok            The attribute byte was patched.
 * @retval k_ra8_err_not_found No such name in the root directory.
 * @retval k_ra8_err_*         Backend read/write failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is exFAT.
 * @pre The mount is in use and the masks have already been validated.
 * @post On success the File entry's FileAttributes low byte is `(old & ~clear) |
 *       set` and the set's SetChecksum is valid.
 * @post On failure the set is unchanged.
 *
 * @note Bounded loop (NASA Rule 2): the set's entry count, `<= k_exfat_set_max_entries`.
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_setattr_exfat(const ra8_fs_mount_t* m,
                                        const char*           path,
                                        uint8_t               set_mask,
                                        uint8_t               clear_mask)
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
  uint8_t eset[(uint32_t)k_exfat_set_max_entries * (uint32_t)k_exfat_entry_bytes] = {};
  for (uint32_t k = 0U; k < count; k++) {
    exfat_cursor_t one = {.cluster          = pos[k].cluster,
                          .entry_in_cluster = pos[k].index,
                          .scanned          = 0U,
                          .contig_end       = 0U};
    err = priv_exfat_next_entry(m, &one, &eset[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  const uint8_t attr = eset[k_exfat_off_file_attr];
  eset[k_exfat_off_file_attr] =
    (uint8_t)((uint8_t)(attr & (uint8_t)~clear_mask) | (uint8_t)set_mask);
  priv_wr16(&eset[k_exfat_off_file_csum],
            priv_exfat_set_checksum(eset, count * (uint32_t)k_exfat_entry_bytes));
  return priv_exfat_write_dir_set(m,
                                  pos[0].cluster,
                                  pos[0].index,
                                  &eset[0],
                                  (uint32_t)k_exfat_entry_bytes);
}

/**
 * @brief Set attributes -- the guarded body of ::ra8_fs_set_attr().
 *
 * @details Validates the arguments, refuses the volume root (no entry to
 *          patch), rejects a mask that names a non-settable bit or a bit in both
 *          masks, and dispatches to the FAT or exFAT patcher. Both masks 0 is a
 *          no-op success -- a set-attr that changes nothing changed nothing. The
 *          public ::ra8_fs_set_attr brackets this with the library lock; the
 *          full contract is documented there.
 *
 * @param[in]     handle     Mount handle.
 * @param[in]     path       Path to the entry.
 * @param[in]     set_mask   Attribute bits to set.
 * @param[in]     clear_mask Attribute bits to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok                Attribute byte patched (or both masks 0).
 * @retval k_ra8_err_null_ptr      @p handle or @p path is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   @p path names the volume root or is not a
 *                                 valid name; or a mask names a non-settable bit
 *                                 or a bit in both masks.
 * @retval k_ra8_err_not_found     Nothing at @p path.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle and @p path are non-NULL.
 * @post On success the requested bits hold their new state.
 * @post On failure the entry is unchanged.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_setattr_locked(const ra8_fs_mount_t* handle,
                                         const char*           path,
                                         uint8_t               set_mask,
                                         uint8_t               clear_mask)
{
  if (handle == nullptr || path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  /* Confine the change to the four host-controlled bits, and reject a bit told
   * to be both set and cleared. Any of the three catches makes the whole request
   * invalid_arg, so the DIRECTORY / VOLUME_ID / long-name bits can never be
   * flipped and a caller cannot give contradictory instructions. */
  const uint8_t settable = (uint8_t)k_ra8_fs_attr_settable;
  if (((set_mask & (uint8_t)~settable) != 0U) || ((clear_mask & (uint8_t)~settable) != 0U) ||
      ((set_mask & clear_mask) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  if (*p == '\0') {
    return k_ra8_err_invalid_arg; /* the volume root has no entry to patch */
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return internal_setattr_exfat(handle, path, set_mask, clear_mask);
  }
  return internal_setattr_fat(handle, path, set_mask, clear_mask);
}

/* =============================================================================
 * Public entry point -- the lock bracket
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_set_attr(const ra8_fs_mount_t* handle,
                          const char*           path,
                          uint8_t               set_mask,
                          uint8_t               clear_mask)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_setattr_locked(handle, path, set_mask, clear_mask);
  priv_lock_release();
  return err;
}
