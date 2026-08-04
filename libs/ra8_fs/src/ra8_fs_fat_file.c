/**
 * @file ra8_fs_fat_file.c
 * @brief FAT open / close path and directory resolution.
 *
 * @details
 * Directory resolution (parent/dir), file creation, and the public
 * open/close entry points for the `ra8_fs` FAT adapter.
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
 * Public API: open / close
 * =============================================================================
 */

/* `priv_entry_first_cluster()`: see header for the documented contract. */
uint32_t priv_entry_first_cluster(const uint8_t* entry)
{
  const uint32_t hi = priv_rd16(&entry[k_dir_off_fst_clus_hi]);
  const uint32_t lo = priv_rd16(&entry[k_dir_off_fst_clus_lo]);
  return (hi << k_shift_two_bytes) | lo;
}

/* `priv_entry_set_cluster_size()`: see header for the documented contract. */
void priv_entry_set_cluster_size(uint8_t* entry, uint32_t cluster, uint32_t size)
{
  priv_wr16(&entry[k_dir_off_fst_clus_hi],
            (uint16_t)((cluster >> k_shift_two_bytes) & k_word_mask));
  priv_wr16(&entry[k_dir_off_fst_clus_lo], (uint16_t)(cluster & k_word_mask));
  priv_wr32(&entry[k_dir_off_file_size], size);
}

/**
 * @brief Truncate an existing file's chain and zero its dir-entry size.
 *
 * @details Frees the cluster chain, resets the in-memory file state,
 *          then writes a fresh dir entry with cluster=0 and size=0.
 *
 * @param[in,out] handle Mount providing FAT access.
 * @param[in,out] f      File state to reset.
 * @param[in]     lba    Sector LBA holding the directory entry.
 * @param[in]     off    Byte offset of the entry within the sector.
 *
 * @return Error code.
 * @retval k_ra8_ok    File truncated successfully.
 * @retval k_ra8_err_* Backend or FAT error.
 *
 * @pre All pointers are non-NULL; mount/file are in use.
 * @pre `lba`/`off` identify the file's directory entry.
 * @post On success, the file occupies zero clusters and its size is 0.
 * @post On failure, on-disk state may be partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_truncate_existing(ra8_fs_mount_t* handle, ra8_fs_file_t* f, uint32_t lba, uint32_t off)
{
  if (f->first_cluster >= k_cluster_first_data) {
    ra8_err_t err = priv_free_chain(handle, f->first_cluster);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  f->first_cluster                         = 0;
  f->cur_cluster                           = 0;
  f->walk_cache_idx                        = 0;
  f->walk_cache_cluster                    = 0; /* < 2: no read cache for a fresh file */
  f->size_bytes                            = 0;
  f->offset                                = 0;
  uint8_t   buf[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t err                            = priv_read_sector(handle, lba, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_entry_set_cluster_size(&buf[off], 0, 0);
  return priv_write_sector(handle, lba, buf);
}

/**
 * @brief Populate a fresh file handle from an existing on-disk dir entry.
 *
 * @details Allocates a file slot, copies the entry's first cluster /
 *          size into it, sets the requested mode, and applies
 *          truncate/append behaviour for write/append modes.
 *
 * @param[in,out] handle   Mount on which the file lives.
 * @param[in]     entry    32-byte directory entry already on disk.
 * @param[in]     lba      Sector LBA holding the directory entry.
 * @param[in]     off      Byte offset of the entry within the sector.
 * @param[in]     mode     Open mode (read / write / append).
 * @param[out]    out_file Receives the populated file handle.
 *
 * @return Error code.
 * @retval k_ra8_ok              File handle ready.
 * @retval k_ra8_err_invalid_arg `entry` carries ATTR_DIRECTORY.
 * @retval k_ra8_err_no_mem      File table is full.
 * @retval k_ra8_err_*           Backend error during truncation.
 *
 * @pre All pointers are non-NULL; mount is in use.
 * @pre `entry` came from a successful `priv_dir_find` for `lba`/`off`.
 * @post On success, `*out_file` is in use and configured for `mode`.
 * @post On failure, the file slot is marked free again.
 * @post A directory entry is rejected before any slot or chain is touched.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_open_existing(ra8_fs_mount_t* handle,
                                    const uint8_t*  entry,
                                    uint32_t        lba,
                                    uint32_t        off,
                                    ra8_fs_mode_t   mode,
                                    ra8_fs_file_t** out_file)
{
  /* A directory is not a file (#604). Without this the write path ran
   * priv_truncate_existing() on it -- freeing the chain that held every child
   * and stamping cluster 0 / size 0 into an entry still flagged ATTR_DIRECTORY
   * -- and the read path handed back a zero-byte handle, because a directory's
   * DIR_FileSize is 0 by specification. Rejected for every mode, before the
   * file slot is allocated, so a refused open consumes nothing. */
  if ((entry[k_dir_off_attr] & (uint8_t)k_ra8_fs_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  f->mount              = handle;
  f->first_cluster      = priv_entry_first_cluster(entry);
  f->cur_cluster        = f->first_cluster;
  f->walk_cache_idx     = 0U; /* read accelerator seeded at the chain head */
  f->walk_cache_cluster = f->first_cluster;
  f->size_bytes         = priv_rd32(&entry[k_dir_off_file_size]);
  f->dir_entry_lba      = lba;
  f->dir_entry_idx      = off;
  f->mode               = mode;
  f->no_fat_chain       = 0U;
  f->in_use             = 1;
  if (mode == k_ra8_fs_mode_write) {
    ra8_err_t err = priv_truncate_existing(handle, f, lba, off);
    if (err != k_ra8_ok) {
      f->in_use = 0;
      return err;
    }
  } else if (mode == k_ra8_fs_mode_append) {
    f->offset = f->size_bytes;
  } else {
    f->offset = 0;
  }
  *out_file = f;
  return k_ra8_ok;
}

/* `priv_write_new_dir_entry()`: see header for the documented contract. */
ra8_err_t priv_write_new_dir_entry(ra8_fs_mount_t* handle,
                                   const uint8_t*  name83,
                                   uint8_t         attr,
                                   uint32_t        first_cluster,
                                   uint32_t        free_lba,
                                   uint32_t        free_off)
{
  uint8_t   buf[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t err                            = priv_read_sector(handle, free_lba, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t* ent = &buf[free_off];
  for (uint32_t i = 0; i < (uint32_t)k_ra8_fs_dir_entry_bytes; i++) {
    ent[i] = 0;
  }
  priv_byte_copy(&ent[k_dir_off_name], name83, k_dir_name_field_len);
  ent[k_dir_off_attr] = attr;
  priv_entry_set_cluster_size(ent, first_cluster, 0U);
  return priv_write_sector(handle, free_lba, buf);
}

/**
 * @brief Populate a freshly allocated file slot for an empty new file.
 *
 * @details Sets every field of @p f to the empty-file initial state: no
 *          clusters, zero size/offset, the dir-entry location, the open mode,
 *          and the in-use flag. Extracted from `priv_create_new`.
 *
 * @param[out] f        File slot to initialise.
 * @param[in]  handle   Owning mount.
 * @param[in]  mode     Open mode to record.
 * @param[in]  free_lba Sector of the file's directory entry.
 * @param[in]  free_off Byte offset of the directory entry within the sector.
 *
 * @return Nothing.
 *
 * @pre @p f and @p handle are non-NULL.
 * @pre @p free_lba and @p free_off identify an already-written directory entry slot.
 * @post @p f->in_use is 1 and every cluster, size, offset, and position field is zero.
 * @post @p f->dir_entry_lba and @p f->dir_entry_idx reflect the on-disk slot location.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_init_new_file(ra8_fs_file_t*  f,
                               ra8_fs_mount_t* handle,
                               ra8_fs_mode_t   mode,
                               uint32_t        free_lba,
                               uint32_t        free_off)
{
  f->mount              = handle;
  f->first_cluster      = 0;
  f->cur_cluster        = 0;
  f->walk_cache_idx     = 0;
  f->walk_cache_cluster = 0; /* < 2: no read cache for a fresh file */
  f->size_bytes         = 0;
  f->offset             = 0;
  f->dir_entry_lba      = free_lba;
  f->dir_entry_idx      = free_off;
  f->mode               = mode;
  f->no_fat_chain       = 0U;
  f->in_use             = 1;
}

/** @brief Path-resolution caps (statically bound the component walk). */
typedef enum : uint32_t {
  k_path_max_depth = 32U, /**< Max nested directory components per path. */
} ra8_fs_path_cap_t;

/**
 * @brief Descend into the named directory component within @p cur.
 *
 * @details Packs the `len`-byte component to an 8.3 name, looks it up in @p cur,
 *          requires the matched entry to carry the directory attribute, and
 *          returns its first cluster as a subdirectory location.
 *
 * @param[in]  m    Mount providing geometry and backend.
 * @param[in]  cur  Directory the component is looked up in.
 * @param[in]  comp Pointer to the component characters (not NUL-terminated).
 * @param[in]  len  Number of component characters.
 * @param[out] out  Receives the subdirectory location on success.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Component resolved to a subdirectory.
 * @retval k_ra8_err_invalid_arg    Component is not 8.3 or is not a directory.
 * @retval k_ra8_err_not_found      No such entry in @p cur.
 * @retval k_ra8_err_protocol_error Directory entry has no data cluster.
 * @retval k_ra8_err_*              Backend error.
 *
 * @pre `m`, `cur`, `comp`, and `out` are non-NULL.
 * @pre `len` is the exact component length (no trailing slash).
 * @post On success `out` locates the subdirectory.
 * @post On failure `out` is unmodified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_enter_subdir(const ra8_fs_mount_t* m,
                                   const dir_loc_t*      cur,
                                   const char*           comp,
                                   uint32_t              len,
                                   dir_loc_t*            out)
{
  if (len > (uint32_t)k_ra8_fs_short_name_len) {
    return k_ra8_err_invalid_arg;
  }
  char namebuf[(uint32_t)k_ra8_fs_short_name_len + 1U] = {};
  for (uint32_t i = 0; i < len; i++) {
    namebuf[i] = comp[i];
  }
  namebuf[len]                   = '\0';
  uint8_t name83[k_max_8_3_name] = {};
  if (priv_path_to_83(namebuf, name83) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t  lba                             = 0;
  uint32_t  off                             = 0;
  uint8_t   entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t err                             = priv_dir_find(m, cur, name83, &lba, &off, entry);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((entry[k_dir_off_attr] & k_ra8_fs_attr_directory) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t cl = priv_entry_first_cluster(entry);
  if (cl < (uint32_t)k_cluster_first_data) {
    return k_ra8_err_protocol_error;
  }
  out->is_root = 0U;
  out->cluster = cl;
  return k_ra8_ok;
}

/* `priv_resolve_parent()`: see header for the documented contract. */
ra8_err_t priv_resolve_parent(const ra8_fs_mount_t* m,
                              const char*           path,
                              dir_loc_t*            out_parent,
                              const char**          out_leaf)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  dir_loc_t cur = {.is_root = 1U, .cluster = 0U};
  for (uint32_t depth = 0; depth < (uint32_t)k_path_max_depth; depth++) {
    const char* end = p;
    while (*end != '\0') {
      if (*end == '/') {
        break;
      }
      end++;
    }
    if (*end == '\0') {
      *out_parent = cur;
      *out_leaf   = p;
      return k_ra8_ok;
    }
    dir_loc_t       next = {};
    const ra8_err_t err  = priv_enter_subdir(m, &cur, p, (uint32_t)(end - p), &next);
    if (err != k_ra8_ok) {
      return err;
    }
    cur = next;
    p   = end;
    while (*p == '/') {
      p++;
    }
  }
  return k_ra8_err_invalid_arg; /* deeper than k_path_max_depth */
}

/* `priv_resolve_dir()`: see header for the documented contract. */
ra8_err_t priv_resolve_dir(const ra8_fs_mount_t* m, const char* path, dir_loc_t* out)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  if (*p == '\0') {
    out->is_root = 1U;
    out->cluster = 0U;
    return k_ra8_ok;
  }
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t err    = priv_resolve_parent(m, path, &parent, &leaf);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t len = 0;
  while (leaf[len] != '\0') {
    len++;
  }
  if (len == 0U) {
    *out = parent; /* trailing slash: the parent is the directory */
    return k_ra8_ok;
  }
  return priv_enter_subdir(m, &parent, leaf, len, out);
}

/**
 * @brief Carve a fresh directory entry for @p name83 and populate a file handle.
 *
 * @details Locates a free directory slot in @p parent via priv_dir_find_free(),
 *          allocates a file slot from the static pool, writes the 8.3 name plus
 *          an archive attribute via priv_write_new_dir_entry(), and initialises
 *          the slot to an empty file via priv_init_new_file(). On any failure
 *          after slot allocation the slot is left unreleased (the file is not
 *          in-use, so it remains available for the next allocation attempt).
 *
 * @param[in,out] handle   Mount on which to create the file.
 * @param[in]     parent   Directory in which to create the entry.
 * @param[in]     name83   Packed 11-byte 8.3 short name.
 * @param[in]     mode     Open mode to record into the returned handle.
 * @param[out]    out_file Receives the populated file handle on success.
 *
 * @return Error code.
 * @retval k_ra8_ok          New file created and opened; @p out_file populated.
 * @retval k_ra8_err_no_mem  No free directory slot or the file table is full.
 * @retval k_ra8_err_*       Backend read/write error.
 *
 * @pre All pointers are non-NULL; @p handle is a mounted volume.
 * @pre @p name83 was produced by a successful priv_path_to_83() call.
 * @post On k_ra8_ok, @p *out_file is in-use with the directory entry on disk.
 * @post On failure, no valid directory entry is written for @p name83.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_create_new(ra8_fs_mount_t*  handle,
                                 const dir_loc_t* parent,
                                 const uint8_t*   name83,
                                 ra8_fs_mode_t    mode,
                                 ra8_fs_file_t**  out_file)
{
  uint32_t  free_lba = 0;
  uint32_t  free_off = 0;
  ra8_err_t err      = priv_dir_find_free(handle, parent, &free_lba, &free_off);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  err = priv_write_new_dir_entry(handle, name83, k_ra8_fs_attr_archive, 0U, free_lba, free_off);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_init_new_file(f, handle, mode, free_lba, free_off);
  *out_file = f;
  return k_ra8_ok;
}

/* `priv_open_locked()`: see header for the documented contract. */
ra8_err_t priv_open_locked(ra8_fs_mount_t* handle,
                           const char*     path,
                           ra8_fs_mode_t   mode,
                           ra8_fs_file_t** out_file)
{
  if (handle == nullptr || path == nullptr || out_file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_open(handle, path, mode, out_file);
  }
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(handle, path, &parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  uint8_t       name83[k_max_8_3_name] = {};
  const uint8_t have83                 = priv_path_to_83(leaf, name83);

  uint32_t  lba                             = 0;
  uint32_t  off                             = 0;
  uint8_t   entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t err                             = k_ra8_err_not_found;
  if (have83 != 0U) {
    err = priv_dir_find(handle, &parent, name83, &lba, &off, entry);
  }
  if (err == k_ra8_err_not_found) {
    /* VFAT long-name fallback: a name that is not 8.3-representable (e.g. a
     * 4-char ".epub" extension), or an 8.3 lookup that missed, can still match a
     * file's long name. */
    err = priv_dir_find_long(handle, &parent, leaf, &lba, &off, entry);
  }
  if (err == k_ra8_ok) {
    return priv_open_existing(handle, entry, lba, off, mode, out_file);
  }
  if (err != k_ra8_err_not_found) {
    return err;
  }
  if (mode == k_ra8_fs_mode_read) {
    return k_ra8_err_not_found;
  }
  /* Creation needs an 8.3-representable name (LFN write is out of scope, #101). */
  if (have83 == 0U) {
    return k_ra8_err_invalid_arg;
  }
  return priv_create_new(handle, &parent, name83, mode, out_file);
}

/* `priv_close_locked()`: see header for the documented contract. */
ra8_err_t priv_close_locked(ra8_fs_file_t* file)
{
  if (file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  file->in_use = 0;
  file->mount  = nullptr;
  return k_ra8_ok;
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t
ra8_fs_open(ra8_fs_mount_t* handle, const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_open_locked(handle, path, mode, out_file);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_close(ra8_fs_file_t* file)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_close_locked(file);
  priv_lock_release();
  return err;
}
