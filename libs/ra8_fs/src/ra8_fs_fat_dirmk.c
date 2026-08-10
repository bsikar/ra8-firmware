/**
 * @file ra8_fs_fat_dirmk.c
 * @brief FAT subdirectory creation and removal (`mkdir` / `rmdir`).
 *
 * @details
 * The two verbs that add and take away a directory, kept together because
 * they are inverses over the same on-disk shape: `mkdir` allocates a cluster,
 * stamps `.` and `..` into it and links an ATTR_DIRECTORY entry, and `rmdir`
 * undoes exactly that -- after proving the directory holds nothing but those
 * two dot entries.
 *
 * The public entry points at the bottom dispatch: exFAT has neither dot entries
 * nor a parent back-link, so its half lives in `ra8_fs_fat_exfat_dir.c` (#605)
 * rather than being bent into the FAT shape here.
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
  /* "." and ".." are ordinary directory entries to every host, and every mkfs
   * stamps them. Leaving them at zero would put an illegal date inside a
   * directory this firmware had just created (#601). */
  priv_fat_entry_stamp_create(ent);
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
  uint8_t* const buf = priv_sec_walk();
  priv_byte_fill(buf, 0U, priv_bps(m));
  priv_pack_dot_entry(&buf[0], 1U, new_cluster);
  priv_pack_dot_entry(&buf[k_ra8_fs_dir_entry_bytes], 2U, parent_cluster);
  const uint64_t base = priv_cluster_to_lba(m, new_cluster);
  ra8_err_t      err  = priv_write_sector(m, base, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t s = 1; s < m->sectors_per_cluster; s++) {
    err = priv_write_sector(m, base + s, k_zero_sector);
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
  uint64_t lba                             = 0;
  uint32_t off                             = 0;
  uint8_t  entry[k_ra8_fs_dir_entry_bytes] = {};
  /* By long name as well as by 8.3: a second `mkdir("/Reading List")` has to
   * be refused, and the alias probe behind `priv_dir_reserve()` would happily
   * file a second copy as `READIN~2` if it were not. */
  if (priv_dir_lookup_any(handle, &parent, leaf, &lba, &off, entry) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  dir_insert_t plan = {};
  ra8_err_t    err  = priv_dir_reserve(handle, &parent, leaf, &plan);
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
  uint8_t tmpl[k_ra8_fs_dir_entry_bytes] = {};
  tmpl[k_dir_off_attr]                   = (uint8_t)k_ra8_fs_attr_directory;
  priv_entry_set_cluster_size(tmpl, new_cluster, 0U);
  /* Same reasoning as the file case: a zero FAT date is illegal, and the
   * directory's own "." and ".." were stamped when the cluster was built, so
   * the entry in the parent must agree with them (#601). */
  priv_fat_entry_stamp_create(tmpl);
  err = priv_dir_commit(handle, &plan, tmpl, &lba, &off);
  if (err != k_ra8_ok) {
    (void)priv_free_chain(handle, new_cluster);
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Create a directory -- the guarded body of ::ra8_fs_mkdir().
 *
 * @details Validates arguments and the mount, then dispatches to the FAT or
 *          the exFAT directory creator (#605).
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to create.
 *
 * @return Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_null_ptr      `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state Mount not in use.
 * @retval k_ra8_err_*             See `priv_fat_mkdir` / `priv_exfat_mkdir`.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use.
 * @post On success a new empty directory exists at `path`.
 * @post On failure the volume is unchanged (a partial alloc is rolled back).
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_mkdir_locked(ra8_fs_mount_t* handle, const char* path)
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
    return priv_exfat_mkdir(handle, path);
  }
  return priv_fat_mkdir(handle, path);
}

/**
 * @enum dir_scan_t
 * @brief Verdict of scanning one directory sector for occupancy.
 *
 * @details Three-valued because "this sector held nothing" and "the directory
 *          ends here" are different answers: the first means keep walking, the
 *          second means stop and report empty.
 *
 * @invariant Exactly one value is returned per scanned sector.
 * @see priv_rmdir_scan_sector()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_dir_scan_more  = 0U, /**< Only skippable slots here; keep walking. */
  k_dir_scan_empty = 1U, /**< End-of-directory marker reached.         */
  k_dir_scan_used  = 2U, /**< A real entry was found; not empty.       */
} dir_scan_t;

/**
 * @brief Classify one already-loaded directory sector as empty / occupied / more.
 *
 * @details A slot does not count as an occupant when it is deleted (0xE5), an
 *          attr-0x0F long-name slot, or one of the synthetic "." / ".." dot
 *          entries. Long-name slots are skipped rather than counted because
 *          `ra8_fs_unlink()` clears only the 8.3 entry, so a removed file
 *          leaves its whole LFN chain behind; counting those remnants would
 *          make a genuinely empty directory permanently un-removable. A live
 *          LFN chain is always followed by its own 8.3 entry in the same
 *          directory, so nothing real is missed by skipping it.
 *
 * @param[in] m   Mounted volume (supplies the entries-per-sector bound).
 * @param[in] buf Whole-sector buffer holding directory entries.
 *
 * @return The sector's verdict.
 * @retval k_dir_scan_used  A non-skippable entry was seen.
 * @retval k_dir_scan_empty The end-of-directory marker was seen first.
 * @retval k_dir_scan_more  Sector exhausted with only skippable slots.
 *
 * @pre `buf` is non-NULL and holds a sector loaded from disk.
 * @pre `buf` addresses at least one whole directory sector.
 * @post No state is modified; `buf` is unchanged.
 * @post The scan visits at most one sector's worth of slots.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static dir_scan_t priv_rmdir_scan_sector(const ra8_fs_mount_t* m, const uint8_t* buf)
{
  for (uint32_t e = 0; e < priv_dir_eps(m); e++) {
    const uint8_t* ent = &buf[(size_t)e * (size_t)k_ra8_fs_dir_entry_bytes];
    if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
      return k_dir_scan_empty;
    }
    if (ent[k_dir_off_name] == k_dir_marker_free_used) {
      continue;
    }
    if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn) {
      continue;
    }
    if (ent[k_dir_off_name] == k_dir_marker_dot) {
      continue;
    }
    return k_dir_scan_used;
  }
  return k_dir_scan_more;
}

/**
 * @brief Decide whether a subdirectory holds any entry other than "." / "..".
 *
 * @details Walks the directory's cluster chain with the shared `dir_walk_t`
 *          iterator, classifying each sector via priv_rmdir_scan_sector() and
 *          stopping at the first definite answer.
 *
 * @param[in]  m         Mount providing geometry and backend.
 * @param[in]  cluster   The subdirectory's first cluster.
 * @param[out] out_empty Receives 1 when the directory is removable, else 0.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Verdict written to `*out_empty`.
 * @retval k_ra8_err_protocol_error Cluster-chain cycle detected (corrupt FAT).
 * @retval k_ra8_err_*              Backend read error.
 *
 * @pre `m` and `out_empty` are non-NULL; `cluster >= k_cluster_first_data`.
 * @pre The entry for `cluster` carries `k_ra8_fs_attr_directory`.
 * @post On success `*out_empty` is 0 or 1.
 * @post No on-disk state is modified.
 *
 * @note The walk is bounded: `priv_dir_walk_next_sector` fails a chain that
 *       revisits a cluster, so the loop cannot run away on a corrupt FAT
 *       (NASA Power of 10 Rule 2).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_dir_is_empty(const ra8_fs_mount_t* m, uint32_t cluster, uint8_t* out_empty)
{
  const dir_loc_t loc = {.is_root = 0U, .cluster = cluster};
  dir_walk_t      w   = {};
  priv_dir_walk_init_loc(m, &loc, &w);
  uint8_t        eod = 0;
  uint8_t* const buf = priv_sec_walk();
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    const dir_scan_t verdict = priv_rmdir_scan_sector(m, buf);
    if (verdict == k_dir_scan_used) {
      *out_empty = 0U;
      return k_ra8_ok;
    }
    if (verdict == k_dir_scan_empty) {
      *out_empty = 1U;
      return k_ra8_ok;
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  *out_empty = 1U; /* chain exhausted with no occupant and no 0x00 marker */
  return k_ra8_ok;
}

/**
 * @brief Resolve a path to a removable subdirectory's entry and first cluster.
 *
 * @details Resolves the parent, refuses the volume root, packs the leaf to 8.3,
 *          looks it up, and requires the matched entry to carry
 *          `k_ra8_fs_attr_directory` with a real first cluster. Split out of
 *          `priv_fat_rmdir` so both stay inside the function-size gate.
 *
 * @param[in]  handle      Mounted FAT12/16/32 volume.
 * @param[in]  path        Directory path to remove.
 * @param[out] out         Receives the parent, the entry position and the entry.
 * @param[out] out_cluster The directory's own first cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Located; outputs populated.
 * @retval k_ra8_err_invalid_arg    `path` is the root, is unstorable, or names
 *                                  a file rather than a directory.
 * @retval k_ra8_err_not_found      No such entry.
 * @retval k_ra8_err_protocol_error The entry claims no data cluster.
 * @retval k_ra8_err_*              Backend error.
 *
 * @pre All pointers are non-NULL; `handle` is a mounted FAT volume.
 * @pre `handle->type` is not exFAT (the caller has dispatched already).
 * @post On success the outputs address a directory entry on disk.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_rmdir_locate(const ra8_fs_mount_t* handle,
                                   const char*           path,
                                   dir_target_t*         out,
                                   uint32_t*             out_cluster)
{
  const char*     leaf = nullptr;
  const ra8_err_t rerr = priv_resolve_parent(handle, path, &out->parent, &leaf);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  if (leaf[0] == '\0') {
    return k_ra8_err_invalid_arg; /* "/" or a trailing slash: the root itself */
  }
  const ra8_err_t err =
    priv_dir_lookup_any(handle, &out->parent, leaf, &out->lba, &out->off, out->entry);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((out->entry[k_dir_off_attr] & (uint8_t)k_ra8_fs_attr_directory) == 0U) {
    return k_ra8_err_invalid_arg; /* a file: ra8_fs_unlink() is the verb for it */
  }
  *out_cluster = priv_entry_first_cluster(out->entry);
  if (*out_cluster < (uint32_t)k_cluster_first_data) {
    return k_ra8_err_protocol_error; /* a directory always owns a cluster */
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `priv_fat_rmdir()` -- remove one empty FAT directory.
 *
 * @details Locates the directory, proves it holds nothing but its own "." and
 *          ".." links, frees its cluster chain, then 0xE5-marks its entry in
 *          the parent. The emptiness proof runs before anything is freed, so a
 *          refused removal changes nothing on disk.
 *
 * @param[in] handle Mounted FAT12/16/32 volume.
 * @param[in] path   NUL-terminated directory path to remove.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Directory removed.
 * @retval k_ra8_err_invalid_arg    Root, not an 8.3 name, or not a directory.
 * @retval k_ra8_err_not_found      No such entry.
 * @retval k_ra8_err_not_empty      The directory still holds entries.
 * @retval k_ra8_err_protocol_error Corrupt chain or a directory with no cluster.
 * @retval k_ra8_err_*              Backend / FAT error.
 *
 * @pre `handle` and `path` are non-NULL; `handle` is a mounted FAT volume.
 * @pre No open file handle refers to an entry inside `path`.
 * @post On success the name no longer resolves and its cluster chain is free.
 * @post On any error other than a mid-free backend fault the volume is unchanged.
 *
 * @note An open handle to a file inside the directory cannot be orphaned: that
 *       file's entry is still present, so the emptiness check refuses first.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_rmdir(const ra8_fs_mount_t* handle, const char* path)
{
  dir_target_t t       = {};
  uint32_t     cluster = 0;
  ra8_err_t    err     = priv_rmdir_locate(handle, path, &t, &cluster);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t empty = 0;
  err           = priv_dir_is_empty(handle, cluster, &empty);
  if (err != k_ra8_ok) {
    return err;
  }
  if (empty == 0U) {
    return k_ra8_err_not_empty;
  }
  err = priv_free_chain(handle, cluster);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_dir_erase_chain(handle, &t.parent, t.lba, t.off, &t.entry[k_dir_off_name]);
}

/**
 * @brief Remove an empty directory -- the guarded body of ::ra8_fs_rmdir().
 *
 * @details Validates arguments and the mount, then dispatches to the FAT or
 *          the exFAT directory remover (#605). exFAT `rmdir` landed with exFAT
 *          `mkdir`, in one change: before it, a volume mounted here never
 *          contained a directory this driver had made, so the verb had no
 *          reachable subject and could only have been exercised against a
 *          hand-crafted fixture.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to remove.
 *
 * @return Error code.
 * @retval k_ra8_ok                Directory removed.
 * @retval k_ra8_err_null_ptr      `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state Mount not in use.
 * @retval k_ra8_err_*             See `priv_fat_rmdir` / `priv_exfat_rmdir`.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle` and `path` are non-NULL.
 * @pre Mount is in use.
 * @post On success `path` no longer resolves.
 * @post On failure the volume is unchanged.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_rmdir_locked(ra8_fs_mount_t* handle, const char* path)
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
    return priv_exfat_rmdir(handle, path);
  }
  return priv_fat_rmdir(handle, path);
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_mkdir(ra8_fs_mount_t* handle, const char* path)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_mkdir_locked(handle, path);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_rmdir(ra8_fs_mount_t* handle, const char* path)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_rmdir_locked(handle, path);
  priv_lock_release();
  return err;
}
