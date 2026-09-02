/**
 * @file ra8_fs_fat_label.c
 * @brief Runtime volume-label read/set (`ra8_fs_get_label()` / `ra8_fs_set_label()`).
 *
 * @details
 * The human name of the medium, readable and changeable after format. On FAT
 * the label lives in two places kept in step: the boot sector's `BS_VolLab`
 * field, and a root-directory `ATTR_VOLUME_ID` entry (the copy a desktop shows
 * and edits). Setting a real label writes both -- creating the root entry when
 * absent -- so `fsck.fat` never sees a blank or a mismatched label; clearing the
 * label restores the `"NO NAME    "` sentinel in the boot sector and removes the
 * root entry. Reading prefers the root entry, falling back to `BS_VolLab`, and
 * reports the unlabelled sentinel as the empty string.
 *
 * The exFAT half of both operations lives in `ra8_fs_fat_exfat_label.c`; this
 * file owns the FAT path and the public lock-bracketed entry points that
 * dispatch to one or the other.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000, section 3.1 (`BS_VolLab`) and section 6
 *     (`ATTR_VOLUME_ID`).
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the root-directory walks terminate on end-of-directory or the
 *     walker's own cluster-cycle guard; the copy loops are bounded by the
 *     11-byte label field.
 *   - Rule 3: zero malloc; one 512-byte sector buffer on the stack.
 *   - Rule 7: every backend call is checked.
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
 * @brief Byte offset of `BS_VolLab` in this volume's boot sector.
 *
 * @details FAT32 carries its extended boot signature (and so its label) at a
 *          different offset than FAT12/16, because the FAT32 BPB is longer.
 *
 * @param[in] m Mounted FAT volume.
 *
 * @return The `BS_VolLab` byte offset.
 * @retval k_fmt_off_f32_label FAT32.
 * @retval k_fmt_off_f16_label FAT12 / FAT16.
 *
 * @pre @p m is non-NULL with `m->type` computed.
 * @pre `m->type` is FAT12/16/32 (the caller has excluded exFAT).
 * @post No state modified.
 * @post The result is purely a function of `m->type`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_fat_boot_label_off(const ra8_fs_mount_t* m)
{
  return (m->type == k_ra8_fs_type_fat32) ? (uint32_t)k_fmt_off_f32_label
                                          : (uint32_t)k_fmt_off_f16_label;
}

/**
 * @brief Decode an 11-byte packed FAT label into a trimmed ASCII string.
 *
 * @details Copies the field, strips trailing padding (spaces or NULs), and
 *          reports the unlabelled sentinel `"NO NAME    "` as the empty string
 *          -- the same "no label" meaning FAT gives it. The result is
 *          NUL-terminated and truncated to @p out_len.
 *
 * @param[in]  raw11   The 11-byte `BS_VolLab` / `ATTR_VOLUME_ID` name field.
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes (at least 1).
 *
 * @return Nothing.
 *
 * @pre @p raw11 and @p out are non-NULL; `out_len >= 1`.
 * @pre @p raw11 addresses 11 readable bytes.
 * @post @p out is NUL-terminated (possibly truncated).
 * @post No byte past @p out[out_len-1] is written.
 *
 * @note Bounded loop (NASA Rule 2): at most ::k_fmt_label_len iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_label_from_raw(const uint8_t* raw11, char* out, uint32_t out_len)
{
  static const uint8_t k_no_name[k_fmt_label_len] =
    {'N', 'O', ' ', 'N', 'A', 'M', 'E', ' ', ' ', ' ', ' '};
  if (priv_byte_equal(raw11, k_no_name, (uint32_t)k_fmt_label_len) != 0U) {
    out[0] = '\0';
    return;
  }
  /* BS_VolLab and the ATTR_VOLUME_ID name are space-padded by the FAT spec (and
   * by ::priv_fmt_label_field), so trailing spaces are the only padding to trim. */
  uint32_t n = (uint32_t)k_fmt_label_len;
  while (n > 0U) {
    if (raw11[n - 1U] != (uint8_t)' ') {
      break;
    }
    n--;
  }
  uint32_t w = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    if ((w + 1U) >= out_len) {
      break;
    }
    out[w] = (char)raw11[i];
    w++;
  }
  out[w] = '\0';
}

/**
 * @brief Find the root directory's `ATTR_VOLUME_ID` entry.
 *
 * @details Walks the root directory (a FAT12/16 fixed region or a FAT32 root
 *          cluster chain) for the volume-label entry: an in-use entry whose
 *          attribute byte has the `ATTR_VOLUME_ID` bit but is not the `0x0F`
 *          long-name marker. Stops at end-of-directory.
 *
 * @param[in]  m         Mounted FAT volume.
 * @param[out] out_lba   Sector holding the entry.
 * @param[out] out_off   Byte offset of the entry within the sector.
 * @param[out] out_entry The 32-byte entry.
 *
 * @return Error code.
 * @retval k_ra8_ok            Entry found; outputs populated.
 * @retval k_ra8_err_not_found No volume-label entry in the root directory.
 * @retval k_ra8_err_*         Backend read failure.
 *
 * @pre All pointers are non-NULL; `m->type` is FAT12/16/32.
 * @pre The mount is in use.
 * @post On k_ra8_ok the outputs address the entry on disk.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_find_vol_id(const ra8_fs_mount_t* m,
                                          uint64_t*             out_lba,
                                          uint32_t*             out_off,
                                          uint8_t*              out_entry)
{
  const dir_loc_t loc = {.is_root = 1U, .cluster = 0U};
  dir_walk_t      w   = {};
  priv_dir_walk_init_loc(m, &loc, &w);
  uint8_t        eod = 0U;
  uint8_t* const buf = priv_sec_walk();
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    for (uint32_t e = 0U; e < priv_dir_eps(m); e++) {
      const uint32_t off   = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
      const uint8_t  name0 = buf[off + (uint32_t)k_dir_off_name];
      const uint8_t  attr  = buf[off + (uint32_t)k_dir_off_attr];
      if (name0 == (uint8_t)k_dir_marker_free_perm) {
        return k_ra8_err_not_found;
      }
      if (name0 == (uint8_t)k_dir_marker_free_used) {
        continue;
      }
      if (attr == (uint8_t)k_ra8_fs_attr_lfn) {
        continue;
      }
      if ((attr & (uint8_t)k_ra8_fs_attr_volume_id) != 0U) {
        *out_lba = w.cur_lba;
        *out_off = off;
        priv_byte_copy(out_entry, &buf[off], (uint32_t)k_ra8_fs_dir_entry_bytes);
        return k_ra8_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Find the first free slot in the root directory.
 *
 * @details Walks the root for the first entry whose name field is 0x00 (never
 *          used, end-of-directory) or 0xE5 (deleted) -- where a fresh
 *          volume-label entry can be written.
 *
 * @param[in]  m       Mounted FAT volume.
 * @param[out] out_lba Sector holding the free slot.
 * @param[out] out_off Byte offset of the slot within the sector.
 *
 * @return Error code.
 * @retval k_ra8_ok         Free slot found; outputs populated.
 * @retval k_ra8_err_no_mem The root directory is full.
 * @retval k_ra8_err_*      Backend read failure.
 *
 * @pre All pointers are non-NULL; `m->type` is FAT12/16/32.
 * @pre The mount is in use.
 * @post On k_ra8_ok the outputs address a writable slot.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_fat_find_free_root(const ra8_fs_mount_t* m, uint64_t* out_lba, uint32_t* out_off)
{
  const dir_loc_t loc = {.is_root = 1U, .cluster = 0U};
  dir_walk_t      w   = {};
  priv_dir_walk_init_loc(m, &loc, &w);
  uint8_t        eod = 0U;
  uint8_t* const buf = priv_sec_walk();
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    for (uint32_t e = 0U; e < priv_dir_eps(m); e++) {
      const uint32_t off   = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
      const uint8_t  name0 = buf[off + (uint32_t)k_dir_off_name];
      if ((name0 == (uint8_t)k_dir_marker_free_perm) ||
          (name0 == (uint8_t)k_dir_marker_free_used)) {
        *out_lba = w.cur_lba;
        *out_off = off;
        return k_ra8_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Write the boot sector's `BS_VolLab` field.
 *
 * @details Read-modify-write of sector 0: the label field is replaced with the
 *          padded @p label (or the `"NO NAME    "` sentinel when @p label is
 *          NULL / empty, via ::priv_fmt_label_field) and every other byte is
 *          preserved.
 *
 * @param[in] m     Mounted FAT volume.
 * @param[in] label New label, or NULL / "" for the unlabelled sentinel.
 *
 * @return Error code.
 * @retval k_ra8_ok    Boot sector updated.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m is non-NULL; `m->type` is FAT12/16/32.
 * @pre The mount is in use.
 * @post On k_ra8_ok the boot sector's `BS_VolLab` reflects @p label.
 * @post No byte outside the label field changes.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_boot_set_label(const ra8_fs_mount_t* m, const char* label)
{
  uint8_t* const  boot = priv_sec_walk();
  const ra8_err_t err  = priv_read_sector(m, 0U, boot);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fmt_label_field(&boot[internal_fat_boot_label_off(m)], label);
  return priv_write_sector(m, 0U, boot);
}

/**
 * @brief Mark the root directory entry at @p lba / @p off deleted.
 *
 * @details Writes the 0xE5 deleted marker to the entry's first name byte,
 *          preserving the rest of the sector.
 *
 * @param[in] m   Mounted FAT volume.
 * @param[in] lba Sector holding the entry.
 * @param[in] off Byte offset of the entry within the sector.
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry deleted.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m is non-NULL; @p lba / @p off came from ::priv_fat_find_vol_id.
 * @pre The mount is in use.
 * @post On k_ra8_ok the entry no longer resolves.
 * @post No other directory byte changes.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_del_entry(const ra8_fs_mount_t* m, uint64_t lba, uint32_t off)
{
  uint8_t* const  sec = priv_sec_walk();
  const ra8_err_t err = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  sec[off + (uint32_t)k_dir_off_name] = (uint8_t)k_dir_marker_free_used;
  return priv_write_sector(m, lba, sec);
}

/**
 * @brief Write a root-directory `ATTR_VOLUME_ID` entry at @p lba / @p off.
 *
 * @details Read-modify-write of the sector: a @p fresh slot is zeroed first and
 *          stamped with a create time; an existing entry keeps its create fields
 *          and gets a write-time stamp. Either way the 11-byte name becomes the
 *          padded @p label and the attribute byte becomes `ATTR_VOLUME_ID`.
 *
 * @param[in] m     Mounted FAT volume.
 * @param[in] lba   Sector holding the slot.
 * @param[in] off   Byte offset of the slot within the sector.
 * @param[in] label Label to store (non-empty).
 * @param[in] fresh true to build a brand-new entry, false to rewrite one.
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry written.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m and @p label are non-NULL; @p lba / @p off address a writable slot.
 * @pre The mount is in use.
 * @post On k_ra8_ok the slot holds a stamped volume-label entry naming @p label.
 * @post No byte outside the entry changes.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_put_vol_id(const ra8_fs_mount_t* m,
                                         uint64_t              lba,
                                         uint32_t              off,
                                         const char*           label,
                                         bool                  fresh)
{
  uint8_t* const  sec = priv_sec_walk();
  const ra8_err_t err = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t* e = &sec[off];
  if (fresh) {
    for (uint32_t i = 0U; i < (uint32_t)k_ra8_fs_dir_entry_bytes; i++) {
      e[i] = 0U;
    }
  }
  priv_fmt_label_field(&e[k_dir_off_name], label);
  e[k_dir_off_attr] = (uint8_t)k_ra8_fs_attr_volume_id;
  if (fresh) {
    priv_fat_entry_stamp_create(e);
  } else {
    priv_fat_entry_stamp_write(e);
  }
  return priv_write_sector(m, lba, sec);
}

/**
 * @brief Read a FAT volume's label -- root `ATTR_VOLUME_ID`, else `BS_VolLab`.
 *
 * @details Prefers the root-directory volume-label entry (the copy a desktop
 *          shows); when the root carries none it falls back to the boot sector's
 *          `BS_VolLab`. The 11-byte field is decoded by ::priv_label_from_raw.
 *
 * @param[in]  m       Mounted FAT volume.
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes (at least 1).
 *
 * @return Error code.
 * @retval k_ra8_ok    @p out holds the label (possibly empty).
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p m and @p out are non-NULL; `m->type` is FAT12/16/32; `out_len >= 1`.
 * @pre The mount is in use.
 * @post @p out is NUL-terminated.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_get_label_fat(const ra8_fs_mount_t* m, char* out, uint32_t out_len)
{
  uint8_t         raw[k_fmt_label_len]            = {};
  uint64_t        lba                             = 0U;
  uint32_t        off                             = 0U;
  uint8_t         entry[k_ra8_fs_dir_entry_bytes] = {};
  const ra8_err_t ferr                            = internal_fat_find_vol_id(m, &lba, &off, entry);
  if (ferr == k_ra8_ok) {
    priv_byte_copy(raw, &entry[k_dir_off_name], (uint32_t)k_fmt_label_len);
  } else if (ferr == k_ra8_err_not_found) {
    uint8_t* const  boot = priv_sec_walk();
    const ra8_err_t berr = priv_read_sector(m, 0U, boot);
    if (berr != k_ra8_ok) {
      return berr;
    }
    priv_byte_copy(raw, &boot[internal_fat_boot_label_off(m)], (uint32_t)k_fmt_label_len);
  } else {
    return ferr;
  }
  internal_label_from_raw(raw, out, out_len);
  return k_ra8_ok;
}

/**
 * @brief Set a FAT volume's label -- boot sector plus root entry, kept in step.
 *
 * @details Writes `BS_VolLab` (the sentinel when clearing), then reconciles the
 *          root-directory volume-label entry: a real label rewrites the existing
 *          entry or creates one in a free slot; clearing deletes it. Keeping the
 *          two copies consistent is what keeps `fsck.fat` quiet.
 *
 * @param[in] m     Mounted FAT volume.
 * @param[in] label New label, or NULL / "" to clear it.
 *
 * @return Error code.
 * @retval k_ra8_ok         Label written.
 * @retval k_ra8_err_no_mem The root directory has no free slot for a new entry.
 * @retval k_ra8_err_*      Backend read/write failure.
 *
 * @pre @p m is non-NULL; `m->type` is FAT12/16/32; the mount is in use.
 * @pre The library lock is held, and @p label (when non-NULL) is <= 11 chars
 *      (the caller ::priv_set_label_locked has checked both).
 * @post On k_ra8_ok a later ::priv_get_label_fat reports @p label.
 * @post The boot-sector and root-directory labels agree.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_set_label_fat(const ra8_fs_mount_t* m, const char* label)
{
  const bool      clearing = (label == nullptr) || (label[0] == '\0');
  const ra8_err_t berr     = internal_fat_boot_set_label(m, label);
  if (berr != k_ra8_ok) {
    return berr;
  }
  uint64_t        lba                             = 0U;
  uint32_t        off                             = 0U;
  uint8_t         entry[k_ra8_fs_dir_entry_bytes] = {};
  const ra8_err_t ferr                            = internal_fat_find_vol_id(m, &lba, &off, entry);
  if ((ferr != k_ra8_ok) && (ferr != k_ra8_err_not_found)) {
    return ferr;
  }
  if (clearing) {
    return (ferr == k_ra8_ok) ? internal_fat_del_entry(m, lba, off) : k_ra8_ok;
  }
  if (ferr == k_ra8_ok) {
    return internal_fat_put_vol_id(m, lba, off, label, false);
  }
  const ra8_err_t serr = internal_fat_find_free_root(m, &lba, &off);
  if (serr != k_ra8_ok) {
    return serr;
  }
  return internal_fat_put_vol_id(m, lba, off, label, true);
}

/**
 * @brief Read the volume label -- the guarded body of ::ra8_fs_get_label().
 *
 * @details Validates the arguments, then dispatches to the FAT or exFAT reader.
 *          The public ::ra8_fs_get_label brackets this with the library lock;
 *          the full contract is documented there.
 *
 * @param[in]  handle  Mount handle.
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok                Label written.
 * @retval k_ra8_err_null_ptr      @p handle or @p out is NULL.
 * @retval k_ra8_err_invalid_arg   @p out_len is 0.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle and @p out are non-NULL.
 * @post On k_ra8_ok @p out is NUL-terminated.
 * @post No volume state is modified.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 * @note The null-pointer guard's MC/DC vectors live in `test_ra8_fs_label.c`
 *       (test_label_get_null_guard).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
internal_get_label_locked(const ra8_fs_mount_t* handle, char* out, uint32_t out_len)
{
  if (handle == nullptr || out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  out[0] = '\0';
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_get_label(handle, out, out_len);
  }
  return internal_get_label_fat(handle, out, out_len);
}

/**
 * @brief Set the volume label -- the guarded body of ::ra8_fs_set_label().
 *
 * @details Validates the arguments (rejecting a label longer than the 11-byte
 *          field), then dispatches to the FAT or exFAT writer. The public
 *          ::ra8_fs_set_label brackets this with the library lock; the full
 *          contract is documented there.
 *
 * @param[in] handle Mount handle.
 * @param[in]     label  New label (<= 11 characters), or NULL / "" to clear it.
 *
 * @return Error code.
 * @retval k_ra8_ok                Label written.
 * @retval k_ra8_err_null_ptr      @p handle is NULL.
 * @retval k_ra8_err_invalid_arg   @p label is longer than 11 characters.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_no_mem        No free root slot for a new label entry.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle is non-NULL.
 * @post On k_ra8_ok a later ::ra8_fs_get_label reports @p label.
 * @post On an argument error the volume is unchanged.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 * @note The over-long-label guard's MC/DC vectors live in `test_ra8_fs_label.c`
 *       (test_label_set_guard).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_set_label_locked(const ra8_fs_mount_t* handle, const char* label)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if ((label != nullptr) && (priv_strlen(label) > (uint32_t)k_fmt_label_len)) {
    return k_ra8_err_invalid_arg;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_set_label(handle, label);
  }
  return internal_set_label_fat(handle, label);
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_get_label(const ra8_fs_mount_t* handle, char* out, uint32_t out_len)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_get_label_locked(handle, out, out_len);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_set_label(const ra8_fs_mount_t* handle, const char* label)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_set_label_locked(handle, label);
  priv_lock_release();
  return err;
}
