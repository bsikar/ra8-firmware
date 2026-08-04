/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fs_fat_dir.c
 * @brief FAT listdir / mkdir / unlink / rename directory operations.
 *
 * @details
 * Directory listing, subdirectory creation, and the unlink/rename
 * operations dispatched across FAT and exFAT volumes.
 *
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/* =============================================================================
 * Public API: listdir / unlink
 * =============================================================================
 */

/**
 * @brief Visit every visible entry in one already-loaded directory sector.
 *
 * @details Skips deleted (0xE5) and LFN (attr 0x0F) entries and the synthetic
 *          "." / ".." directory entries. Stops on the end-of-directory marker
 *          (0x00).
 *
 * @param[in]     buf 512-byte sector buffer holding directory entries.
 * @param[in,out] lfn LFN reassembly state carried across sectors.
 * @param[in]     cb  Caller-supplied per-entry callback.
 * @param[in]     ctx Opaque pointer forwarded to `cb`.
 *
 * @return 1 if end-of-directory marker hit (caller can stop), 0 otherwise.
 * @retval 1  End-of-directory reached; caller should stop.
 * @retval 0  Sector exhausted without end-of-directory.
 *
 * @pre `buf` and `cb` are non-NULL.
 * @pre `buf` holds a sector loaded from disk.
 * @post No state modified by this function (callback may modify `ctx`).
 * @post `cb` invoked once per visible entry.
 *
 * @note Thread-safety inherited from `cb`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t
priv_listdir_visit_sector(const uint8_t* buf, lfn_state_t* lfn, ra8_fs_listdir_cb_t cb, void* ctx)
{
  for (uint32_t e = 0; e < (uint32_t)k_dir_entries_per_sector; e++) {
    const uint8_t* ent = &buf[(size_t)e * (size_t)k_ra8_fs_dir_entry_bytes];
    if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
      return 1U;
    }
    if (ent[k_dir_off_name] == k_dir_marker_free_used) {
      priv_lfn_reset(lfn); /* a deleted slot breaks any in-progress chain */
      continue;
    }
    if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn) {
      priv_lfn_add(lfn, ent);
      continue;
    }
    if (ent[k_dir_off_name] == '.') {
      priv_lfn_reset(lfn); /* synthetic "." / ".." -- not reported to the caller */
      continue;
    }
    /* +1 for the NUL: a maximal 8.3 name "12345678.123" is 12 chars, and
     * priv_83_to_str writes the terminator at index 12 -- without the slack
     * that store overflows the buffer and corrupts the adjacent `ent` pointer,
     * mismatching the LFN checksum so the long name is dropped (matches the
     * `+ 1U` sizing already used in ra8_fs_fat_file.c). */
    char short_name[(uint32_t)k_ra8_fs_short_name_len + 1U] = {};
    priv_83_to_str(&ent[k_dir_off_name], short_name);
    /* Report the VFAT long name when the preceding chain matches; else the 8.3. */
    const char*    lname = priv_lfn_name_for(lfn, &ent[k_dir_off_name]);
    const uint32_t size  = priv_rd32(&ent[k_dir_off_file_size]);
    cb((lname != nullptr) ? lname : short_name, ent[k_dir_off_attr], size, ctx);
    priv_lfn_reset(lfn);
  }
  return 0U;
}

/**
 * @brief Enumerate the entries in a directory (root or a subdirectory).
 *
 * @details Resolves @p path to a directory via `priv_resolve_dir` and walks it,
 *          invoking @p cb once per visible entry. FAT12/16/32 support any path
 *          (`"/"` or nested); exFAT remains root-only. The synthetic "." and
 *          ".." entries are not reported.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   Directory path (`"/"` or a nested path on FAT).
 * @param[in]     cb     Per-entry callback.
 * @param[in]     ctx    Opaque pointer forwarded to `cb`.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Directory walked successfully.
 * @retval k_ra8_err_null_ptr        Any required pointer was NULL.
 * @retval k_ra8_err_invalid_state   Mount not in use.
 * @retval k_ra8_err_not_found       A path component does not exist.
 * @retval k_ra8_err_not_supported   exFAT path other than `"/"`.
 * @retval k_ra8_err_*               Backend error.
 *
 * @pre `handle`, `path`, and `cb` are non-NULL.
 * @pre Mount is in use.
 * @post `cb` invoked once per visible directory entry.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra8_err_t
ra8_fs_listdir(ra8_fs_mount_t* handle, const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  if (handle == nullptr || cb == nullptr || path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    /* exFAT directory listing is root-only for now. */
    if (path[0] != '/') {
      return k_ra8_err_not_supported;
    }
    if (path[1] != '\0') {
      return k_ra8_err_not_supported;
    }
    return priv_exfat_listdir(handle, cb, ctx);
  }
  dir_loc_t       loc  = {};
  const ra8_err_t rerr = priv_resolve_dir(handle, path, &loc);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  dir_walk_t w = {};
  priv_dir_walk_init_loc(handle, &loc, &w);
  lfn_state_t lfn = {}; /* persists across sectors -- LFN chains can straddle them */
  priv_lfn_reset(&lfn);
  uint8_t eod                            = 0;
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(handle, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_listdir_visit_sector(buf, &lfn, cb, ctx) != 0U) {
      return k_ra8_ok;
    }
    err = priv_dir_walk_next_sector(handle, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Pack a "." or ".." dot entry into a 32-byte directory slot.
 *
 * @details Writes the FAT self/parent link: a space-padded name of @p dots
 *          dots, the directory attribute, and @p cluster as the first cluster
 *          (size 0). A parent that is the volume root is recorded as cluster 0
 *          per the FAT specification.
 *
 * @param[out] ent     32-byte slot to populate (zeroed by this function).
 * @param[in]  dots    1 for ".", 2 for "..".
 * @param[in]  cluster Self cluster ("."), or parent cluster ("..", 0 if root).
 *
 * @return Nothing.
 *
 * @pre `ent` addresses 32 writable bytes.
 * @pre `dots` is 1 or 2.
 * @post `ent` holds a directory dot entry pointing at `cluster`.
 * @post Bytes after the name/attr/cluster fields are zero.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_pack_dot_entry(uint8_t* ent, uint32_t dots, uint32_t cluster)
{
  for (uint32_t i = 0; i < (uint32_t)k_ra8_fs_dir_entry_bytes; i++) {
    ent[i] = 0;
  }
  for (uint32_t i = 0; i < (uint32_t)k_dir_name_field_len; i++) {
    ent[i] = ' ';
  }
  for (uint32_t i = 0; i < dots; i++) {
    ent[i] = '.';
  }
  ent[k_dir_off_attr] = k_ra8_fs_attr_directory;
  priv_entry_set_cluster_size(ent, cluster, 0U);
}

/**
 * @brief Initialise a freshly allocated directory cluster ("." + ".." + zeros).
 *
 * @details Writes the self ("."), parent ("..") links into the first sector and
 *          zeroes every remaining sector of the cluster so the directory has a
 *          clean end-of-directory marker for subsequent entries.
 *
 * @param[in] m              Mount providing geometry and backend.
 * @param[in] new_cluster    The directory's own first cluster.
 * @param[in] parent_cluster Parent's first cluster (0 when the parent is root).
 *
 * @return Error code.
 * @retval k_ra8_ok    Cluster initialised on disk.
 * @retval k_ra8_err_* Backend write error.
 *
 * @pre `m` is non-NULL; `new_cluster >= k_cluster_first_data`.
 * @pre `m->sectors_per_cluster >= 1`.
 * @post On success the cluster holds "." and ".." then zeros.
 * @post On failure the cluster may be partially written.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_dir_cluster_init(const ra8_fs_mount_t* m, uint32_t new_cluster, uint32_t parent_cluster)
{
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  priv_pack_dot_entry(&buf[0], 1U, new_cluster);
  priv_pack_dot_entry(&buf[k_ra8_fs_dir_entry_bytes], 2U, parent_cluster);
  const uint32_t base = priv_cluster_to_lba(m, new_cluster);
  ra8_err_t      err  = priv_write_sector(m, base, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t zero[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t s = 1; s < m->sectors_per_cluster; s++) {
    err = priv_write_sector(m, base + s, zero);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `priv_fat_mkdir()` -- create one FAT directory.
 *
 * @details Resolves the parent, rejects an existing name, finds a free parent
 *          slot, allocates and initialises a directory cluster ("." / ".."),
 *          then writes the parent's directory entry. On any post-allocation
 *          failure the new cluster is freed so the volume is not leaked.
 *
 * @param[in,out] handle Mounted FAT12/16/32 volume.
 * @param[in]     path   NUL-terminated directory path to create.
 *
 * @return Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_invalid_arg   Leaf is not an 8.3 name.
 * @retval k_ra8_err_exists        The name already exists in the parent.
 * @retval k_ra8_err_no_mem        Parent directory full or volume full.
 * @retval k_ra8_err_not_found     An intermediate path component is missing.
 * @retval k_ra8_err_*             Backend / FAT error.
 *
 * @pre `handle` and `path` are non-NULL; mount is a FAT volume.
 * @pre The parent path exists.
 * @post On success the new directory has "." and ".." and an empty body.
 * @post On failure no cluster is leaked (a partial allocation is freed).
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_mkdir(ra8_fs_mount_t* handle, const char* path)
{
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(handle, path, &parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  uint8_t name83[k_max_8_3_name] = {};
  if (priv_path_to_83(leaf, name83) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t lba                             = 0;
  uint32_t off                             = 0;
  uint8_t  entry[k_ra8_fs_dir_entry_bytes] = {};
  if (priv_dir_find(handle, &parent, name83, &lba, &off, entry) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  uint32_t  free_lba = 0;
  uint32_t  free_off = 0;
  ra8_err_t err      = priv_dir_find_free(handle, &parent, &free_lba, &free_off);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t new_cluster = 0;
  err                  = priv_alloc_eoc_cluster(handle, &new_cluster);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t parent_cluster = (parent.is_root != 0U) ? 0U : parent.cluster;
  err                           = priv_dir_cluster_init(handle, new_cluster, parent_cluster);
  if (err != k_ra8_ok) {
    (void)priv_free_chain(handle, new_cluster);
    return err;
  }
  err = priv_write_new_dir_entry(handle,
                                 name83,
                                 k_ra8_fs_attr_directory,
                                 new_cluster,
                                 free_lba,
                                 free_off);
  if (err != k_ra8_ok) {
    (void)priv_free_chain(handle, new_cluster);
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Create a directory on a mounted volume (public API).
 *
 * @details Validates arguments and the mount, then dispatches to the FAT
 *          directory creator. exFAT directory creation is not yet supported.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to create.
 *
 * @return Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_null_ptr      `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state Mount not in use.
 * @retval k_ra8_err_not_supported The volume is exFAT.
 * @retval k_ra8_err_*             See `priv_fat_mkdir`.
 *
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use.
 * @post On success a new empty directory exists at `path`.
 * @post On failure the volume is unchanged (a partial alloc is rolled back).
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_mkdir(ra8_fs_mount_t* handle, const char* path)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return k_ra8_err_not_supported;
  }
  return priv_fat_mkdir(handle, path);
}

/**
 * @brief Delete a file from the root directory.
 *
 * @details Frees the cluster chain (if any) and marks the dir entry
 *          deleted by writing 0xE5 to the first byte of the name field.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated path; only flat root names supported.
 *
 * @return Error code.
 * @retval k_ra8_ok                File deleted.
 * @retval k_ra8_err_null_ptr      Any pointer was NULL.
 * @retval k_ra8_err_invalid_state Mount not in use.
 * @retval k_ra8_err_invalid_arg   Path is not a valid 8.3 name.
 * @retval k_ra8_err_not_found     No such file.
 * @retval k_ra8_err_*             Backend or FAT error.
 *
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use; no file handle currently references this entry.
 * @post On success, file's clusters are free and dir entry is deleted.
 * @post On failure, on-disk state may be partially updated.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_unlink(ra8_fs_mount_t* handle, const char* path)
{
  if (handle == nullptr || path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_unlink(handle, path);
  }
  dir_loc_t       parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t rerr   = priv_resolve_parent(handle, path, &parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  uint8_t name83[k_max_8_3_name] = {};
  if (priv_path_to_83(leaf, name83) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t  lba                             = 0;
  uint32_t  off                             = 0;
  uint8_t   entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t err = priv_dir_find(handle, &parent, name83, &lba, &off, entry);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t first_cluster = priv_entry_first_cluster(entry);
  if (first_cluster >= k_cluster_first_data) {
    err = priv_free_chain(handle, first_cluster);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  err                                    = priv_read_sector(handle, lba, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  buf[off + k_dir_off_name] = k_dir_marker_free_used;
  return priv_write_sector(handle, lba, buf);
}

/**
 * @brief Resolve a rename's old/new paths to a shared parent + packed names.
 *
 * @details Resolves both parents, requires they are the same directory (an
 *          in-place rename cannot move an entry between directories), then packs
 *          both leaf components to 8.3. Extracted from `priv_fat_rename` so that
 *          function stays under the size/complexity gate.
 *
 * @param[in]  handle   Mounted FAT volume.
 * @param[in]  old_path Existing path.
 * @param[in]  new_path Replacement path (same directory).
 * @param[out] out_parent Receives the shared parent directory location.
 * @param[out] old83    Packed 8.3 name of the existing leaf.
 * @param[out] new83    Packed 8.3 name of the replacement leaf.
 *
 * @return Error code.
 * @retval k_ra8_ok                Resolved; outputs populated.
 * @retval k_ra8_err_not_supported The two paths are in different directories.
 * @retval k_ra8_err_invalid_arg   A leaf is not an 8.3 name.
 * @retval k_ra8_err_*             Resolution / backend error.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre `old83` and `new83` each address `k_max_8_3_name` bytes.
 * @post On success both names are packed and `out_parent` is set.
 * @post On failure the outputs are unspecified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_rename_prepare(const ra8_fs_mount_t* handle,
                                     const char*           old_path,
                                     const char*           new_path,
                                     dir_loc_t*            out_parent,
                                     uint8_t               old83[k_max_8_3_name],
                                     uint8_t               new83[k_max_8_3_name])
{
  dir_loc_t   op = {};
  const char* ol = nullptr;
  ra8_err_t   e1 = priv_resolve_parent(handle, old_path, &op, &ol);
  if (e1 != k_ra8_ok) {
    return e1;
  }
  dir_loc_t   np = {};
  const char* nl = nullptr;
  ra8_err_t   e2 = priv_resolve_parent(handle, new_path, &np, &nl);
  if (e2 != k_ra8_ok) {
    return e2;
  }
  if (op.is_root != np.is_root) {
    return k_ra8_err_not_supported;
  }
  if (op.is_root == 0U) {
    if (op.cluster != np.cluster) {
      return k_ra8_err_not_supported;
    }
  }
  if (priv_path_to_83(ol, old83) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_path_to_83(nl, new83) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  *out_parent = op;
  return k_ra8_ok;
}

/**
 * @brief Rename a file on a FAT12/16/32 volume by rewriting the 8.3 name in place.
 *
 * @details Prepares both paths via priv_rename_prepare() (resolves parents, requires
 *          same-directory, packs both names to 8.3). Checks that @p new_path does not
 *          already exist (priv_dir_find() on new83 must fail). Locates the existing
 *          entry for @p old_path via priv_dir_find(), reads the sector, overwrites the
 *          11-byte name field with the new packed name, and writes the sector back.
 *          Cluster chains, file sizes, and attributes are not modified.
 *
 * @param[in] handle   Mounted FAT12/16/32 volume.
 * @param[in] old_path Existing path (must resolve to an 8.3-named entry).
 * @param[in] new_path Replacement path (must not yet exist; same directory as old_path).
 *
 * @return Error code.
 * @retval k_ra8_ok                File renamed; old name is gone, new name resolves.
 * @retval k_ra8_err_invalid_arg   A leaf component does not convert to 8.3 format.
 * @retval k_ra8_err_not_found     @p old_path does not exist.
 * @retval k_ra8_err_exists        @p new_path already resolves to an entry.
 * @retval k_ra8_err_not_supported The two paths are in different directories.
 * @retval k_ra8_err_*             Backend read/write error.
 *
 * @pre @p handle and both path pointers are non-NULL; @p handle is a mounted FAT volume.
 * @pre Neither @p old_path nor @p new_path is currently held open.
 * @post On k_ra8_ok, @p new_path resolves to the same directory entry as @p old_path did.
 * @post On k_ra8_ok, @p old_path no longer resolves; no cluster or FAT data is changed.
 *
 * @note Not thread-safe; callers serialise access to the mount.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_fat_rename(const ra8_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  dir_loc_t parent                = {};
  uint8_t   old83[k_max_8_3_name] = {};
  uint8_t   new83[k_max_8_3_name] = {};
  ra8_err_t perr = priv_rename_prepare(handle, old_path, new_path, &parent, old83, new83);
  if (perr != k_ra8_ok) {
    return perr;
  }
  uint32_t dup_lba                       = 0U;
  uint32_t dup_off                       = 0U;
  uint8_t  dup[k_ra8_fs_dir_entry_bytes] = {};
  if (priv_dir_find(handle, &parent, new83, &dup_lba, &dup_off, dup) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  uint32_t  lba                             = 0U;
  uint32_t  off                             = 0U;
  uint8_t   entry[k_ra8_fs_dir_entry_bytes] = {};
  ra8_err_t err = priv_dir_find(handle, &parent, old83, &lba, &off, entry);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t sec[k_ra8_fs_bytes_per_sector] = {};
  err                                    = priv_read_sector(handle, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_byte_copy(&sec[off], new83, (uint32_t)k_max_8_3_name);
  return priv_write_sector(handle, lba, sec);
}

/**
 * @brief Implementation of `ra8_fs_rename()`.
 * @details See the public header for the documented contract; dispatches
 *          to the in-place FAT 8.3 rewrite or the exFAT entry-set rewrite.
 * @param[in] handle See header.
 * @param[in] old_path See header.
 * @param[in] new_path See header.
 * @return Result code.
 * @retval k_ra8_ok File renamed.
 * @pre Module state is consistent.
 * @pre The volume is mounted.
 * @post On success the new name resolves to the same data.
 * @post On failure the directory is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_fs_rename(ra8_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (old_path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (new_path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_rename(handle, old_path, new_path);
  }
  return priv_fat_rename(handle, old_path, new_path);
}
