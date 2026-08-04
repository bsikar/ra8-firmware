/**
 * @file ra8_fs_fat_dir.c
 * @brief FAT listdir / unlink / rename directory-entry operations.
 *
 * @details
 * The verbs that read and rewrite entries in a directory that already exists:
 * listing it, deleting a file from it, and renaming one in place, dispatched
 * across FAT and exFAT volumes. `unlink` refuses a directory -- freeing the
 * cluster chain behind one orphans every file inside it (#604); removing a
 * directory is `rmdir`'s job, in `ra8_fs_fat_dirmk.c`.
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
    if (ent[k_dir_off_name] == k_dir_marker_dot) {
      priv_lfn_reset(lfn); /* synthetic "." / ".." -- not reported to the caller */
      continue;
    }
    /* +1 for the NUL: a maximal 8.3 name "12345678.123" is 12 chars, and
     * priv_83_to_str writes the terminator at index 12 -- without the slack
     * that store overflows the buffer and corrupts the adjacent `ent` pointer,
     * mismatching the LFN checksum so the long name is dropped (matches the
     * `+ 1U` sizing already used in ra8_fs_fat_file.c). */
    char short_name[(uint32_t)k_ra8_fs_short_name_len + 1U] = {};
    /* The NTRes case flags come along: an entry this driver wrote for
     * `data.log` stores `DATA    LOG` with both flags set, and reporting it as
     * `DATA.LOG` would lose the caller's own name for no reason (#600). */
    priv_83_to_str(&ent[k_dir_off_name], ent[k_dir_off_ntres], short_name);
    /* Report the VFAT long name when the preceding chain matches; else the 8.3. */
    const char*    lname = priv_lfn_name_for(lfn, &ent[k_dir_off_name]);
    const uint32_t size  = priv_rd32(&ent[k_dir_off_file_size]);
    cb((lname != nullptr) ? lname : short_name, ent[k_dir_off_attr], size, ctx);
    priv_lfn_reset(lfn);
  }
  return 0U;
}

/**
 * @brief Enumerate a directory -- the guarded body of ::ra8_fs_listdir().
 *
 * @details Resolves @p path to a directory -- `priv_resolve_dir` on FAT,
 *          `priv_exfat_resolve_dir` on exFAT -- and walks it, invoking @p cb
 *          once per visible entry. Both filesystems take `"/"` or a nested
 *          path. FAT's synthetic "." and ".." entries are not reported; exFAT
 *          has none to report.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   Directory path (`"/"` or a nested path).
 * @param[in]     cb     Per-entry callback.
 * @param[in]     ctx    Opaque pointer forwarded to `cb`.
 *
 * @return Error code.
 * @retval k_ra8_ok                  Directory walked successfully.
 * @retval k_ra8_err_null_ptr        Any required pointer was NULL.
 * @retval k_ra8_err_invalid_state   Mount not in use.
 * @retval k_ra8_err_not_found       A path component does not exist.
 * @retval k_ra8_err_invalid_arg     A path component names a file.
 * @retval k_ra8_err_*               Backend error.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle`, `path`, and `cb` are non-NULL.
 * @pre Mount is in use.
 * @post `cb` invoked once per visible directory entry.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
priv_listdir_locked(ra8_fs_mount_t* handle, const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  if (handle == nullptr || cb == nullptr || path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (handle->type == k_ra8_fs_type_exfat) {
    /* Any exFAT directory, not just the root (#605). A path that names nothing
     * reports not_found and one that names a FILE reports invalid_arg, which is
     * what the FAT side has always answered -- the two filesystems no longer
     * disagree about what "list this path" means. */
    exfat_dir_t     dir  = {};
    const ra8_err_t xerr = priv_exfat_resolve_dir(handle, path, &dir);
    if (xerr != k_ra8_ok) {
      return xerr;
    }
    return priv_exfat_listdir(handle, &dir, cb, ctx);
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
 * @brief Resolve a path to a deletable FILE's entry and first cluster.
 *
 * @details Resolves the parent, packs the leaf to 8.3, looks it up, and
 *          requires the matched entry NOT to carry `k_ra8_fs_attr_directory`.
 *          The mirror image of priv_rmdir_locate(): each verb refuses the kind
 *          of entry the other owns, and both refuse before anything is freed.
 *          Split out of `ra8_fs_unlink` so it stays inside the function-size
 *          gate.
 *
 * @param[in]  handle      Mounted FAT12/16/32 volume.
 * @param[in]  path        File path to delete.
 * @param[out] out         Receives the parent, the entry position and the entry.
 * @param[out] out_cluster The file's first cluster (0 when it has none).
 *
 * @return Error code.
 * @retval k_ra8_ok              Located; outputs populated.
 * @retval k_ra8_err_invalid_arg `path` is unstorable, or names a directory.
 * @retval k_ra8_err_not_found   No such entry.
 * @retval k_ra8_err_*           Backend error.
 *
 * @pre All pointers are non-NULL; `handle` is a mounted FAT volume.
 * @pre `handle->type` is not exFAT (the caller has dispatched already).
 * @post On success the outputs address a file entry on disk.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_unlink_locate(const ra8_fs_mount_t* handle,
                                    const char*           path,
                                    dir_target_t*         out,
                                    uint32_t*             out_cluster)
{
  const char*     leaf = nullptr;
  const ra8_err_t rerr = priv_resolve_parent(handle, path, &out->parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  const ra8_err_t err =
    priv_dir_lookup_any(handle, &out->parent, leaf, &out->lba, &out->off, out->entry);
  if (err != k_ra8_ok) {
    return err;
  }
  /* The lookup matches on the name alone, so a directory matches like any
   * other entry. Freeing its chain orphans every file inside it -- their
   * clusters stay allocated in the FAT with nothing referencing them, which
   * fsck.fat reports as lost clusters (#604). */
  if ((out->entry[k_dir_off_attr] & (uint8_t)k_ra8_fs_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg; /* a directory: ra8_fs_rmdir() is the verb */
  }
  *out_cluster = priv_entry_first_cluster(out->entry);
  return k_ra8_ok;
}

/**
 * @brief Delete a file -- the guarded body of ::ra8_fs_unlink().
 *
 * @details Frees the cluster chain (if any) and marks the dir entry
 *          deleted by writing 0xE5 to the first byte of the name field.
 *          Refuses a directory: use `ra8_fs_rmdir()` for those.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated path; only flat root names supported.
 *
 * @return Error code.
 * @retval k_ra8_ok                File deleted.
 * @retval k_ra8_err_null_ptr      Any pointer was NULL.
 * @retval k_ra8_err_invalid_state Mount not in use.
 * @retval k_ra8_err_invalid_arg   Path is not a valid 8.3 name, or it names a
 *                                 directory.
 * @retval k_ra8_err_not_found     No such file.
 * @retval k_ra8_err_*             Backend or FAT error.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use; no file handle currently references this entry.
 * @post On success, file's clusters are free and dir entry is deleted.
 * @post On failure, on-disk state may be partially updated.
 * @post A path naming a directory leaves the volume untouched.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_unlink_locked(ra8_fs_mount_t* handle, const char* path)
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
  dir_target_t    t       = {};
  uint32_t        cluster = 0;
  const ra8_err_t lerr    = priv_unlink_locate(handle, path, &t, &cluster);
  if (lerr != k_ra8_ok) {
    return lerr;
  }
  if (cluster >= (uint32_t)k_cluster_first_data) {
    const ra8_err_t ferr = priv_free_chain(handle, cluster);
    if (ferr != k_ra8_ok) {
      return ferr;
    }
  }
  /* The whole chain, not only the 8.3 entry: clearing one slot left the file's
   * long-name entries on disk with a checksum nothing matched, which fsck.fat
   * and chkdsk both report as orphans (#600). */
  return priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[k_dir_off_name]);
}

/**
 * @brief Resolve a rename's old/new paths to a shared parent and two leaves.
 *
 * @details Resolves both parents and requires them to be the same directory: an
 *          in-place rename cannot move an entry between directories. The leaf
 *          components are handed back unpacked, because whether either of them
 *          is an 8.3 name is no longer the rename's business -- the reservation
 *          decides that, and both sides may be long names.
 *
 * @param[in]  handle     Mounted FAT volume.
 * @param[in]  old_path   Existing path.
 * @param[in]  new_path   Replacement path (same directory).
 * @param[out] out_parent Receives the shared parent directory location.
 * @param[out] out_old    Receives a pointer into @p old_path at its leaf.
 * @param[out] out_new    Receives a pointer into @p new_path at its leaf.
 *
 * @return Error code.
 * @retval k_ra8_ok                Resolved; outputs populated.
 * @retval k_ra8_err_not_supported The two paths are in different directories.
 * @retval k_ra8_err_*             Resolution / backend error.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre Both paths outlive the outputs, which point into them.
 * @post On success both leaves and `out_parent` are set.
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
                                     const char**          out_old,
                                     const char**          out_new)
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
  *out_parent = op;
  *out_old    = ol;
  *out_new    = nl;
  return k_ra8_ok;
}

/**
 * @brief Rename by writing the new entry first, then taking the old one away.
 *
 * @details Rewriting the 11-byte name field in place, which is what this used
 *          to do, cannot be right once long names exist: the entry may be the
 *          tail of a VFAT chain that still spells the OLD name, and the new
 *          name may not fit eleven bytes at all. So the entry is re-filed --
 *          the old 32 bytes are carried across verbatim as the template, so
 *          the cluster, the size and the attributes survive, and only the name
 *          and its `DIR_NTRes` flags change.
 *
 *          The order is deliberate. Committing the new entry BEFORE erasing
 *          the old one means a failure between them leaves the file reachable
 *          under both names -- untidy, but nothing is lost. The reverse order
 *          would put a window in the middle of the operation where the file
 *          has no name at all.
 *
 * @param[in] handle   Mounted FAT12/16/32 volume.
 * @param[in] old_path Existing path.
 * @param[in] new_path Replacement path (must not exist; same directory).
 *
 * @return Error code.
 * @retval k_ra8_ok                Renamed; the old name is gone, the new resolves.
 * @retval k_ra8_err_invalid_arg   The new leaf cannot be stored under any encoding.
 * @retval k_ra8_err_not_found     @p old_path does not exist.
 * @retval k_ra8_err_exists        @p new_path already resolves to an entry.
 * @retval k_ra8_err_not_supported The two paths are in different directories.
 * @retval k_ra8_err_no_mem        The directory has no room for the new entry.
 * @retval k_ra8_err_*             Backend read/write error.
 *
 * @pre @p handle and both path pointers are non-NULL; @p handle is a mounted FAT volume.
 * @pre Neither @p old_path nor @p new_path is currently held open.
 * @post On k_ra8_ok, @p new_path resolves to the same cluster, size and attributes.
 * @post On k_ra8_ok, @p old_path no longer resolves and its chain is gone with it.
 *
 * @note Not thread-safe; callers serialise access to the mount.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_fat_rename(const ra8_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  dir_target_t t    = {};
  const char*  ol   = nullptr;
  const char*  nl   = nullptr;
  ra8_err_t    perr = priv_rename_prepare(handle, old_path, new_path, &t.parent, &ol, &nl);
  if (perr != k_ra8_ok) {
    return perr;
  }
  uint32_t dup_lba                       = 0U;
  uint32_t dup_off                       = 0U;
  uint8_t  dup[k_ra8_fs_dir_entry_bytes] = {};
  if (priv_dir_lookup_any(handle, &t.parent, nl, &dup_lba, &dup_off, dup) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  ra8_err_t err = priv_dir_lookup_any(handle, &t.parent, ol, &t.lba, &t.off, t.entry);
  if (err != k_ra8_ok) {
    return err;
  }
  dir_insert_t plan = {};
  err               = priv_dir_reserve(handle, &t.parent, nl, &plan);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Access date only, deliberately. A rename changes the name, not the bytes,
   * so advancing DIR_WrtDate would tell every rsync, backup and "newest image"
   * OTA heuristic that the contents changed -- the same class of false signal
   * #601 exists to remove, only inverted. FAT has no metadata-change field, so
   * the access date is the honest record that the entry was touched. It goes on
   * the template BEFORE the commit, because since #600 a rename writes a new
   * entry rather than editing the old one in place. */
  priv_fat_entry_stamp_access(t.entry);
  uint32_t new_lba = 0U;
  uint32_t new_off = 0U;
  err              = priv_dir_commit(handle, &plan, t.entry, &new_lba, &new_off);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[k_dir_off_name]);
}

/**
 * @brief Rename -- the guarded body of ::ra8_fs_rename().
 * @details See the public header for the documented contract; dispatches
 *          to the FAT re-filing path or the exFAT entry-set rewrite.
 * @param[in] handle See header.
 * @param[in] old_path See header.
 * @param[in] new_path See header.
 * @return Result code.
 * @retval k_ra8_ok File renamed.
 * @pre The library lock is held (or none is installed).
 * @pre The volume is mounted.
 * @post On success the new name resolves to the same data.
 * @post On failure the directory is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
priv_rename_locked(ra8_fs_mount_t* handle, const char* old_path, const char* new_path)
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

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t
ra8_fs_listdir(ra8_fs_mount_t* handle, const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_listdir_locked(handle, path, cb, ctx);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_unlink(ra8_fs_mount_t* handle, const char* path)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_unlink_locked(handle, path);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_rename(ra8_fs_mount_t* handle, const char* old_path, const char* new_path)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_rename_locked(handle, old_path, new_path);
  priv_lock_release();
  return err;
}
