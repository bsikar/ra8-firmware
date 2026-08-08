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
static bool priv_path_is_root(const char* path)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  return *p == '\0';
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
static void priv_stat_root(const ra8_fs_mount_t* m, ra8_fs_stat_t* out)
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
static void priv_entry_to_stat(const uint8_t* entry, ra8_fs_stat_t* out)
{
  const uint8_t attr = entry[k_dir_off_attr];
  out->attr          = attr;
  out->is_directory  = (attr & (uint8_t)k_ra8_fs_attr_directory) != 0U;
  out->first_cluster = priv_entry_first_cluster(entry);
  out->size_bytes    = priv_rd32(&entry[k_dir_off_file_size]);
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
static ra8_err_t priv_stat_exfat(const ra8_fs_mount_t* m, const char* path, ra8_fs_stat_t* out)
{
  uint8_t         strm[k_exfat_entry_bytes] = {};
  uint8_t         attr                      = 0U;
  const ra8_err_t err                       = priv_exfat_lookup(m, path, strm, &attr);
  if (err != k_ra8_ok) {
    return err;
  }
  out->attr          = attr;
  out->is_directory  = (attr & (uint8_t)k_ra8_fs_attr_directory) != 0U;
  out->first_cluster = priv_rd32(&strm[k_exfat_strm_off_clus]);
  out->size_bytes    = priv_rd32(&strm[k_exfat_strm_off_dlen]);
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
static ra8_err_t priv_stat_fat(const ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
{
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(handle, path, &parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  uint8_t       name83[k_max_8_3_name]          = {};
  const uint8_t have83                          = priv_path_to_83(leaf, name83);
  uint32_t      lba                             = 0U;
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
  priv_entry_to_stat(entry, out);
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
static ra8_err_t priv_stat_locked(ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
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
  if (priv_path_is_root(path)) {
    priv_stat_root(handle, out);
    return k_ra8_ok;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_stat_exfat(handle, path, out);
  }
  return priv_stat_fat(handle, path, out);
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_stat(ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_stat_locked(handle, path, out);
  priv_lock_release();
  return err;
}
