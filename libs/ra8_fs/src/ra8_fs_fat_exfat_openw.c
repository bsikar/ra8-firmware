/**
 * @file ra8_fs_fat_exfat_openw.c
 * @brief exFAT open-for-write: create, truncate, append, and the survey between.
 *
 * @details
 * `ra8_fs_open()` used to answer `k_ra8_err_not_supported` for both writing
 * modes on an exFAT volume, so the one way to put bytes on such a card was a
 * whole-file creator that needed the payload in RAM and one contiguous run on
 * disk (#602). This file is the other half of the fix: it turns a name into an
 * open, writable handle, in the three shapes the API promises.
 *
 *   - **Create.** The name does not exist, so a zero-length entry set is laid
 *     down before a single byte is written -- `FirstCluster` 0, both lengths 0,
 *     `NoFatChain` clear, which is what exFAT spec sec 7.4.4 says an empty file
 *     looks like. The handle is usable immediately, and a caller that opens and
 *     closes without writing leaves exactly that empty file, as FAT does.
 *   - **Truncate.** The name exists and the mode is write, so the clusters go
 *     back to the allocation bitmap and the entry set is rewritten empty --
 *     IN PLACE. Keeping the set is what keeps the creation timestamp, and it is
 *     why replacing a file no longer costs it its identity.
 *   - **Append.** The name exists and the mode is append, so the file's real
 *     allocation is surveyed (arithmetic for a contiguous run, a bounded FAT
 *     walk for a chained one) and the offset is parked at `DataLength`.
 *
 * A path that resolves to a DIRECTORY is refused in both modes: a truncate
 * would hand the clusters holding its contents back to the volume and orphan
 * every file inside it (#604).
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, March 2021.
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
 * @brief Zero every field of a freshly claimed handle and bind it to its set.
 *
 * @details One place decides what "an open exFAT file that owns nothing" is,
 *          so a later field cannot be added to ::ra8_fs_file_t and left
 *          uninitialised on one of the three open paths.
 *
 * @param[out] file  Slot to initialise.
 * @param[in]  m     Owning mount.
 * @param[in]  head  Position of the set's File entry.
 * @param[in]  count Entry count of the set (1 + SecondaryCount).
 * @param[in]  mode  Open mode to record.
 *
 * @return Nothing.
 *
 * @pre @p file, @p m and @p head are non-NULL.
 * @pre @p head came from ::priv_exfat_find_set or ::priv_exfat_link.
 * @post @p file is in use, owns no clusters, and is positioned at byte 0.
 * @post `no_fat_chain` is 1: an empty allocation is trivially contiguous, so
 *       the first cluster taken can keep the fast path.
 *
 * @note The FAT-only `dir_entry_*` fields are zeroed, not repurposed.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exfat_seed_handle(ra8_fs_file_t*        file,
                                   ra8_fs_mount_t*       m,
                                   const exfat_setpos_t* head,
                                   uint32_t              count,
                                   ra8_fs_mode_t         mode)
{
  file->mount              = m;
  file->first_cluster      = 0U;
  file->cur_cluster        = 0U;
  file->walk_cache_idx     = 0U;
  file->walk_cache_cluster = 0U;
  file->size_bytes         = 0U;
  file->offset             = 0U;
  file->dir_entry_lba      = 0U;
  file->dir_entry_idx      = 0U;
  file->valid_bytes        = 0U;
  file->entry_set_cluster  = head->cluster;
  file->entry_set_index    = head->index;
  file->entry_set_count    = count;
  file->alloc_clusters     = 0U;
  file->tail_cluster       = 0U;
  file->mode               = mode;
  file->in_use             = 1U;
  file->no_fat_chain       = 1U;
  file->dirty              = 0U;
}

/**
 * @brief Count a file's clusters and find its tail.
 *
 * @details An append has to know where the allocation ENDS before it can add
 *          to it, and the entry set does not record that: a contiguous file
 *          implies it from `DataLength`, and a chained one only reveals it by
 *          walking the FAT. The walk is bounded by the volume's cluster count,
 *          so a corrupted chain that loops terminates instead of hanging.
 *
 *          A chained file may own MORE clusters than its length needs (another
 *          implementation may have pre-allocated), and the walk finds them, so
 *          an append reuses that space instead of asking the bitmap for more.
 *
 * @param[in,out] file Handle whose stream fields are already populated.
 *
 * @return Error code.
 * @retval k_ra8_ok                 `alloc_clusters` and `tail_cluster` are set.
 * @retval k_ra8_err_protocol_error The chain points below the first data cluster.
 * @retval k_ra8_err_*              FAT read failure.
 *
 * @pre @p file is non-NULL with `first_cluster`, `size_bytes` and
 *      `no_fat_chain` taken from the on-disk Stream entry.
 * @pre The mount is an exFAT volume.
 * @post On success `alloc_clusters == 0` exactly when the file owns nothing.
 * @post On success `tail_cluster` is the last cluster of the allocation.
 *
 * @note The owns-nothing test is one compound decision of two conditions --
 *       a file with no first cluster, or one with no length. The second is
 *       load-bearing: without it the contiguous branch computes a cluster
 *       count of 0 and a tail one BELOW the first cluster. Its vectors live
 *       with the tests that drive it, cited as
 *       `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_survey_alloc`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_survey_alloc(ra8_fs_file_t* file)
{
  const ra8_fs_mount_t* m      = file->mount;
  const uint32_t        cbytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  if ((file->first_cluster < (uint32_t)k_cluster_first_data) || (file->size_bytes == 0U)) {
    file->first_cluster  = 0U;
    file->alloc_clusters = 0U;
    file->tail_cluster   = 0U;
    file->no_fat_chain   = 1U;
    return k_ra8_ok;
  }
  if (file->no_fat_chain != 0U) {
    file->alloc_clusters = (file->size_bytes + cbytes - 1U) / cbytes;
    file->tail_cluster   = file->first_cluster + file->alloc_clusters - 1U;
    return k_ra8_ok;
  }
  uint32_t clus  = file->first_cluster;
  uint32_t owned = 1U;
  for (uint32_t guard = 0U; guard < m->count_of_clusters; guard++) {
    uint32_t        next = 0U;
    const ra8_err_t e    = priv_fat_get(m, clus, &next);
    if (e != k_ra8_ok) {
      return e;
    }
    if (priv_is_eoc(m, next) != 0U) {
      break;
    }
    if (next < (uint32_t)k_cluster_first_data) {
      return k_ra8_err_protocol_error;
    }
    clus = next;
    owned++;
  }
  file->alloc_clusters = owned;
  file->tail_cluster   = clus;
  return k_ra8_ok;
}

/**
 * @brief Release a file's clusters and rewrite its entry set empty, in place.
 *
 * @details What `k_ra8_fs_mode_write` means on an existing name. The entry set
 *          is kept -- so the file's creation stamp and its position in the
 *          directory survive -- and only its allocation and both lengths go.
 *
 * @param[in,out] file Open handle to empty.
 * @param[in]     strm The file's Stream entry as read from the volume.
 *
 * @return Error code.
 * @retval k_ra8_ok    The file is zero-length and owns no clusters.
 * @retval k_ra8_err_* Bitmap or backend failure.
 *
 * @pre @p file and @p strm are non-NULL; the handle is in use.
 * @pre The caller holds the library lock.
 * @post On success the freed clusters read as free in the allocation bitmap.
 * @post On success the on-disk entry set reports `DataLength` 0.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_truncate(ra8_fs_file_t* file, const uint8_t* strm)
{
  const ra8_err_t e = priv_exfat_free_clusters(file->mount, strm);
  if (e != k_ra8_ok) {
    return e;
  }
  file->first_cluster      = 0U;
  file->cur_cluster        = 0U;
  file->walk_cache_idx     = 0U;
  file->walk_cache_cluster = 0U;
  file->size_bytes         = 0U;
  file->valid_bytes        = 0U;
  file->offset             = 0U;
  file->alloc_clusters     = 0U;
  file->tail_cluster       = 0U;
  file->no_fat_chain       = 1U;
  return priv_exfat_flush_set(file);
}

/**
 * @brief Reject an entry set this adapter cannot correctly rewrite.
 *
 * @details Three refusals, all of them honest limits rather than guesses.
 *
 *          A set SHORTER than ::k_exfat_set_min_entries is not a file: exFAT
 *          spec sec 7.4 requires a File entry, a Stream entry and at least one
 *          Name entry, and the flush rewrites the first two by position. A set
 *          claiming no secondaries would send that rewrite at the entry BEFORE
 *          the Stream entry -- on a fresh volume, the allocation-bitmap entry
 *          -- so the shape has to be checked, not assumed.
 *
 *          A set LONGER than ::k_exfat_set_writable carries a name past this
 *          adapter's cap, and rewriting only the part that fits would leave a
 *          SetChecksum computed over the wrong number of entries.
 *
 *          A `DataLength` whose high word is non-zero is a file above 4 GiB,
 *          which every length in this API is too narrow to carry -- silently
 *          taking the low word would truncate somebody's file on the first
 *          flush.
 *
 * @param[in] count Entry count of the set (1 + SecondaryCount).
 * @param[in] strm  The file's 32-byte Stream-extension entry.
 *
 * @return Error code.
 * @retval k_ra8_ok                 The set is one this adapter can own.
 * @retval k_ra8_err_protocol_error The set is too short to be a file.
 * @retval k_ra8_err_not_supported  The set has more entries than it can rewrite.
 * @retval k_ra8_err_invalid_size   The file is larger than 4 GiB - 1.
 *
 * @pre @p strm is non-NULL and holds a Stream-extension entry.
 * @pre @p count came from the File entry's SecondaryCount.
 * @post No state is modified on any path.
 * @post A success means the shape, the rewrite and the length all fit.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_writable_set(uint32_t count, const uint8_t* strm)
{
  if (count < (uint32_t)k_exfat_set_min_entries) {
    return k_ra8_err_protocol_error;
  }
  if (count > (uint32_t)k_exfat_set_writable) {
    return k_ra8_err_not_supported;
  }
  if (priv_rd32(&strm[k_exfat_strm_off_dlen_hi]) != 0U) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Open an existing name for writing: truncate it, or park at its end.
 *
 * @details Refuses a directory before anything else, claims a file slot, seeds
 *          it from the Stream entry, and then branches on the mode. On any
 *          failure after the slot is claimed the slot is handed back, so a
 *          refused open never costs one of the four handles. A
 *          `ValidDataLength` above `DataLength` is clamped rather than
 *          believed: it is not a prefix length if it is longer than the file.
 *
 * @param[in,out] handle   Mounted exFAT volume.
 * @param[in]     mode     ::k_ra8_fs_mode_write or ::k_ra8_fs_mode_append.
 * @param[in]     head     Position of the set's File entry.
 * @param[in]     count    Entry count of the set.
 * @param[in]     file_e   The set's 32-byte File entry.
 * @param[in]     strm     The set's 32-byte Stream-extension entry.
 * @param[out]    out_file Receives the open handle.
 *
 * @return Error code.
 * @retval k_ra8_ok                The handle is open and positioned.
 * @retval k_ra8_err_invalid_arg   The name is a directory.
 * @retval k_ra8_err_no_mem        The file table is full.
 * @retval k_ra8_err_not_supported The set is larger than this adapter rewrites.
 * @retval k_ra8_err_invalid_size  The file is larger than 4 GiB - 1.
 * @retval k_ra8_err_*             Bitmap, FAT, or backend failure.
 *
 * @pre Every pointer is non-NULL; the mount is an exFAT volume.
 * @pre The caller holds the library lock.
 * @post On success `*out_file` is in use and its offset suits @p mode.
 * @post On failure no file slot is left marked in use.
 *
 * @note The mode test is a single condition; both arms are driven by the two
 *       writing modes.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_open_found(ra8_fs_mount_t*       handle,
                                       ra8_fs_mode_t         mode,
                                       const exfat_setpos_t* head,
                                       uint32_t              count,
                                       const uint8_t*        file_e,
                                       const uint8_t*        strm,
                                       ra8_fs_file_t**       out_file)
{
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t e = priv_exfat_writable_set(count, strm);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  priv_exfat_seed_handle(f, handle, head, count, mode);
  f->first_cluster = priv_rd32(&strm[k_exfat_strm_off_clus]);
  f->size_bytes    = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  f->valid_bytes   = priv_rd32(&strm[k_exfat_off_strm_valid]);
  f->no_fat_chain =
    ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  /* exFAT spec sec 7.4.5 makes ValidDataLength a PREFIX of DataLength, and the
   * handle's invariant says so too. Another implementation can still leave the
   * pair inverted, and taking it at face value would have the flush write the
   * inversion back -- so it is clamped on the way in, where it costs one
   * comparison, rather than believed. */
  if (f->valid_bytes > f->size_bytes) {
    f->valid_bytes = f->size_bytes;
  }
  if (mode == k_ra8_fs_mode_write) {
    e = priv_exfat_truncate(f, strm);
  } else {
    e                     = priv_exfat_survey_alloc(f);
    f->cur_cluster        = f->first_cluster;
    f->walk_cache_cluster = f->first_cluster;
    f->offset             = f->size_bytes;
  }
  if (e != k_ra8_ok) {
    f->in_use = 0U;
    f->mount  = nullptr;
    return e;
  }
  *out_file = f;
  return k_ra8_ok;
}

/**
 * @brief Create a name that does not exist and open the empty file it names.
 *
 * @details Claims the file slot BEFORE writing the directory entry, so a full
 *          file table is discovered while the directory is still untouched --
 *          the other order leaves a real, empty file behind on a call that
 *          returned an error.
 *
 * @param[in,out] handle   Mounted exFAT volume.
 * @param[in]     dir      Directory the name is created in.
 * @param[in]     path     Leaf name (ASCII, no leading '/').
 * @param[in]     nlen     Length of @p path.
 * @param[in]     mode     Writing mode to record on the handle.
 * @param[out]    out_file Receives the open handle.
 *
 * @return Error code.
 * @retval k_ra8_ok         The name exists as an empty file and is open.
 * @retval k_ra8_err_no_mem The file table is full, or the root directory has no
 *                          run of free slots big enough for the set.
 * @retval k_ra8_err_*      Backend read/write failure.
 *
 * @pre Every pointer is non-NULL; the mount is an exFAT volume.
 * @pre `priv_strlen(path) == nlen` and @p nlen is within the name cap.
 * @post On success @p dir holds a zero-length set for @p path.
 * @post On failure no file slot is left marked in use.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_open_created(ra8_fs_mount_t*    handle,
                                         const exfat_dir_t* dir,
                                         const char*        path,
                                         uint32_t           nlen,
                                         ra8_fs_mode_t      mode,
                                         ra8_fs_file_t**    out_file)
{
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  exfat_setpos_t  head  = {};
  uint32_t        count = 0U;
  const ra8_err_t e     = priv_exfat_link(handle, dir, path, nlen, &head, &count);
  if (e != k_ra8_ok) {
    f->in_use = 0U;
    f->mount  = nullptr;
    return e;
  }
  priv_exfat_seed_handle(f, handle, &head, count, mode);
  *out_file = f;
  return k_ra8_ok;
}

/* `priv_exfat_open_write()`: see header for the documented contract. */
ra8_err_t priv_exfat_open_write(ra8_fs_mount_t* handle,
                                const char*     path,
                                ra8_fs_mode_t   mode,
                                ra8_fs_file_t** out_file)
{
  /* Resolve the directory the name lives in, rather than treating the whole
   * path as one root-level name (#605). Leading slashes fall out of the walk,
   * which is what used to make "/name" resolve (#93); "/logs/name" now lands in
   * "/logs" instead of creating a file literally called "logs/name" that no
   * matcher could ever find again. */
  exfat_dir_t     parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t pe     = priv_exfat_resolve_parent(handle, path, &parent, &leaf);
  if (pe != k_ra8_ok) {
    return pe;
  }
  const uint32_t nlen = priv_strlen(leaf);
  if (nlen == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (nlen > (uint32_t)k_exfat_name_cap) {
    return k_ra8_err_invalid_arg;
  }
  exfat_setpos_t  pos[k_exfat_set_max_entries] = {};
  uint32_t        count                        = 0U;
  uint8_t         file_e[k_exfat_entry_bytes]  = {};
  uint8_t         strm[k_exfat_entry_bytes]    = {};
  const ra8_err_t e = priv_exfat_find_set(handle,
                                          &parent,
                                          leaf,
                                          pos,
                                          (uint32_t)k_exfat_set_max_entries,
                                          &count,
                                          file_e,
                                          strm);
  if (e == k_ra8_ok) {
    return priv_exfat_open_found(handle, mode, &pos[0], count, file_e, strm, out_file);
  }
  if (e != k_ra8_err_not_found) {
    return e;
  }
  return priv_exfat_open_created(handle, &parent, leaf, nlen, mode, out_file);
}
