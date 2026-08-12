/**
 * @file ra8_fs_fat_fileio.c
 * @brief FAT read / write / seek / tell / size I/O.
 *
 * @details
 * Cluster-chain skip/grow, sector-granular read and write streaming, and
 * the public read/write/seek/tell/size entry points.
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
 * Public API: read / write / seek / tell / size
 * =============================================================================
 */

/**
 * @brief Walk `n` clusters forward from `start` along the FAT chain.
 *
 * @details Used by the read path to position to the cluster covering
 *          `file->offset`. Stops early on EOC.
 *
 * @param[in]  m     Mount providing FAT access.
 * @param[in]  start First cluster of the chain.
 * @param[in]  n     Number of clusters to walk.
 * @param[out] out   Receives the cluster reached (or last before EOC).
 *
 * @return Error code.
 * @retval k_ra8_ok                Walked exactly `n` clusters.
 * @retval k_ra8_err_invalid_state Hit EOC before walking `n` clusters.
 * @retval k_ra8_err_*             Backend error.
 *
 * @pre `m` and `out` are non-NULL.
 * @pre `start` is a valid cluster number.
 * @post On success, `*out` is the destination cluster.
 * @post On EOC failure, `*out` is the last valid cluster reached.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_skip_clusters(const ra8_fs_mount_t* m, uint32_t start, uint32_t n, uint32_t* out)
{
  uint32_t cur = start;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t  next = 0;
    ra8_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      *out = cur;
      return k_ra8_err_invalid_state;
    }
    cur = next;
  }
  *out = cur;
  return k_ra8_ok;
}

/**
 * @brief Read up to one sector's worth of bytes at the file's current offset.
 *
 * @details Resolves the cluster covering `file->offset`, reads the
 *          containing sector, and copies the relevant slice into `buf`.
 *
 * @param[in,out] file      File handle providing offset and chain root.
 * @param[out]    buf       Destination of the byte slice.
 * @param[in]     remaining Maximum bytes the caller can accept.
 * @param[out]    out_take  Number of bytes actually copied.
 *
 * @return Error code.
 * @retval k_ra8_ok    Slice copied.
 * @retval k_ra8_err_* Backend or FAT error.
 *
 * @pre All pointers are non-NULL; file is in use.
 * @pre `file->offset < file->size_bytes`.
 * @post On success, `*out_take > 0` and `buf[0..*out_take]` populated.
 * @post `file->offset` is NOT advanced -- caller does that.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_read_one_chunk(ra8_fs_file_t* file, uint8_t* buf, uint32_t remaining, uint32_t* out_take)
{
  const uint32_t cluster_bytes   = priv_cluster_bytes(file->mount);
  const uint32_t cluster_idx_now = (uint32_t)(file->offset / cluster_bytes);
  uint32_t       target          = 0;
  /* exFAT contiguous files (NoFatChain) have no valid FAT chain: clusters are
   * sequential from the first. FAT files (no_fat_chain == 0) walk the chain. */
  if (file->no_fat_chain != 0U) {
    target = file->first_cluster + cluster_idx_now;
  } else {
    /* Read accelerator: a sequential read advances the offset one cluster at a
     * time, so resume the FAT walk from the cached forward waypoint instead of
     * re-walking from the chain head on every sector -- O(n) over a file, not the
     * O(n^2) a per-sector walk-from-head would cost. The cache holds a valid chain
     * position (walk_cache_cluster at walk_cache_idx); use it only when it is set
     * (a real data cluster) AND the target is at or ahead of it, else walk from
     * the head.
     *
     * @par MC/DC:
     * Decision: `(walk_cache_cluster >= k_cluster_first_data) && (cluster_idx_now
     *            >= walk_cache_idx)` (2 conditions)
     * - cache set, target ahead   -> true  (resume from waypoint)
     * - cache unset (cluster < 2)  -> false (first condition independent)
     * - cache set, target behind   -> false (second condition independent; a
     *                                 backward seek re-walks from the head)
     */
    uint32_t walk_from = file->first_cluster;
    uint32_t walk_n    = cluster_idx_now;
    if ((file->walk_cache_cluster >= (uint32_t)k_cluster_first_data) &&
        (cluster_idx_now >= file->walk_cache_idx)) {
      walk_from = file->walk_cache_cluster;
      walk_n    = cluster_idx_now - file->walk_cache_idx;
    }
    ra8_err_t werr = priv_skip_clusters(file->mount, walk_from, walk_n, &target);
    if (werr != k_ra8_ok) {
      return werr;
    }
    file->walk_cache_idx     = cluster_idx_now;
    file->walk_cache_cluster = target;
  }
  file->cur_cluster                 = target;
  const uint32_t  off_in_cluster    = (uint32_t)(file->offset % cluster_bytes);
  const uint32_t  sector_in_cluster = off_in_cluster / priv_bps(file->mount);
  const uint32_t  off_in_sector     = off_in_cluster % priv_bps(file->mount);
  const uint64_t  lba = priv_cluster_to_lba(file->mount, file->cur_cluster) + sector_in_cluster;
  uint8_t* const  sec = priv_sec_io();
  const ra8_err_t err = priv_read_sector(file->mount, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t take = priv_bps(file->mount) - off_in_sector;
  if (take > remaining) {
    take = remaining;
  }
  priv_byte_copy(buf, &sec[off_in_sector], take);
  *out_take = take;
  return k_ra8_ok;
}

/**
 * @brief Read one span, serving zeros past exFAT's ValidDataLength.
 *
 * @details exFAT spec sec 7.4.5 splits a file in two: the prefix below
 *          `ValidDataLength` was written, and everything from there to
 *          `DataLength` was never initialised. Those bytes must read as zero,
 *          and the clusters behind them still hold whatever the previous
 *          tenant left, so serving them raw would hand a caller another file's
 *          data. A FAT handle has no such split -- `DIR_FileSize` IS the
 *          written length -- so it takes the same path with the two bounds
 *          equal and never reaches the hole arm.
 *
 * @param[in,out] file      Open file handle.
 * @param[out]    buf       Destination of the span.
 * @param[in]     remaining Maximum bytes the caller can accept.
 * @param[out]    out_take  Number of bytes actually produced.
 *
 * @return Error code.
 * @retval k_ra8_ok    Span produced; `*out_take > 0`.
 * @retval k_ra8_err_* Backend or FAT error.
 *
 * @pre All pointers are non-NULL; the file is in use.
 * @pre `file->offset < file->size_bytes` and `remaining > 0`.
 * @post On success `*out_take <= remaining`.
 * @post `file->offset` is NOT advanced -- the caller does that.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @note Two single-condition decisions, nested rather than joined. The first
 *       sends an exFAT handle positioned past the written prefix to the zero
 *       arm; every other read falls through. The second clips a read that
 *       would cross OUT of the prefix, so the next span is served zeros
 *       instead of the raw cluster tail.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_read_span(ra8_fs_file_t* file, uint8_t* buf, uint32_t remaining, uint32_t* out_take)
{
  const uint64_t valid =
    (file->mount->type == k_ra8_fs_type_exfat) ? file->valid_bytes : file->size_bytes;
  if (file->offset >= valid) {
    for (uint32_t i = 0U; i < remaining; i++) {
      buf[i] = 0U;
    }
    *out_take = remaining;
    return k_ra8_ok;
  }
  uint32_t       want = remaining;
  const uint64_t cap  = valid - file->offset;
  if ((uint64_t)want > cap) {
    want = (uint32_t)cap;
  }
  return priv_read_one_chunk(file, buf, want, out_take);
}

/**
 * @brief Read bytes -- the guarded body of ::ra8_fs_read().
 *
 * @details Loops on `priv_read_one_chunk`, advancing `file->offset`
 *          after each chunk. Stops at EOF or when `max_len` met.
 *
 * @param[in,out] file    Open file handle.
 * @param[out]    buf     Destination buffer.
 * @param[in]     max_len Maximum bytes to read.
 * @param[out]    got_len Bytes actually read.
 *
 * @return Error code.
 * @retval k_ra8_ok                Read completed (possibly short at EOF).
 * @retval k_ra8_err_null_ptr      Any pointer was NULL.
 * @retval k_ra8_err_invalid_state File is not open.
 * @retval k_ra8_err_*             Backend error.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `file`, `buf`, and `got_len` are non-NULL.
 * @pre File is in use.
 * @post On success, `*got_len` bytes were placed in `buf` and the
 *       file offset advanced by the same amount.
 * @post On failure, `*got_len` is 0.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
priv_read_locked(ra8_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  if (file == nullptr || buf == nullptr || got_len == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  *got_len = 0;
  if (file->offset >= file->size_bytes || max_len == 0U) {
    return k_ra8_ok;
  }
  const uint64_t left      = file->size_bytes - file->offset;
  uint32_t       remaining = max_len;
  if ((uint64_t)max_len > left) {
    remaining = (uint32_t)left;
  }
  uint32_t produced = 0;
  while (remaining > 0U) {
    uint32_t  take = 0;
    ra8_err_t err  = priv_read_span(file, &buf[produced], remaining, &take);
    if (err != k_ra8_ok) {
      return err;
    }
    produced += take;
    file->offset += take;
    remaining -= take;
  }
  *got_len = produced;
  return k_ra8_ok;
}

/* `priv_alloc_eoc_cluster()`: see header for the documented contract. */
ra8_err_t priv_alloc_eoc_cluster(const ra8_fs_mount_t* m, uint32_t* out_c)
{
  ra8_err_t err = priv_alloc_cluster(m, out_c);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_fat_set(m, *out_c, priv_eoc_write(m));
}

/**
 * @brief Walk to cluster index `idx` from `start`, growing the chain as needed.
 *
 * @details Like `priv_skip_clusters` but extends the chain (with new
 *          EOC clusters) when EOC is encountered before reaching `idx`.
 *
 * @param[in]  m           Mount providing FAT access.
 * @param[in]  start       First cluster of the chain.
 * @param[in]  idx         Index to walk to.
 * @param[out] out_cluster Receives the cluster at index `idx`.
 *
 * @return Error code.
 * @retval k_ra8_ok    Reached / created cluster at `idx`.
 * @retval k_ra8_err_* Backend, FAT, or no-mem error.
 *
 * @pre `m` and `out_cluster` are non-NULL.
 * @pre `start` is a valid cluster.
 * @post On success, `*out_cluster` is the cluster at offset `idx`.
 * @post On failure, FAT may have been partially extended.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_walk_grow(const ra8_fs_mount_t* m, uint32_t start, uint32_t idx, uint32_t* out_cluster)
{
  uint32_t cur = start;
  for (uint32_t i = 0; i < idx; i++) {
    uint32_t  next = 0;
    ra8_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      uint32_t newc = 0;
      err           = priv_alloc_eoc_cluster(m, &newc);
      if (err != k_ra8_ok) {
        return err;
      }
      err = priv_fat_set(m, cur, newc);
      if (err != k_ra8_ok) {
        return err;
      }
      next = newc;
    }
    cur = next;
  }
  *out_cluster = cur;
  return k_ra8_ok;
}

/** @brief Implementation of `priv_write_into_sector()` -- one read-modify-write. */
ra8_err_t priv_write_into_sector(const ra8_fs_mount_t* m,
                                 uint64_t              lba,
                                 uint32_t              off_in_sector,
                                 const uint8_t*        src,
                                 uint32_t              put)
{
  uint8_t* const sec = priv_sec_io();
  ra8_err_t      err = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_byte_copy(&sec[off_in_sector], src, put);
  return priv_write_sector(m, lba, sec);
}

/**
 * @struct write_walk_t
 * @brief Forward waypoint carried across one write's grow-walks.
 *
 * @details Without it every sector of a long write re-walked the chain from
 *          the head, which is O(K^2) FAT lookups over a K-cluster file -- the
 *          same quadratic as the allocation scan and worth the same fix
 *          (#607). One write's offset only ever moves forward, so the
 *          waypoint is always at or behind the cluster being written.
 *
 * @invariant `index` is the chain index of `cluster`.
 * @see priv_write_position()
 * @since 0.1.0
 */
typedef struct {
  uint32_t cluster; /**< Cluster the last walk reached. */
  uint32_t index;   /**< Chain index of `cluster`.      */
} write_walk_t;

/**
 * @brief Position the write cursor at chain index @p idx, growing as needed.
 *
 * @details Allocates the file's first cluster when it has none, then walks
 *          forward from the waypoint -- not from the chain head -- extending
 *          the chain if the walk runs off the end. The waypoint is advanced to
 *          wherever the walk landed.
 *
 * @param[in,out] file        File handle whose chain is being extended.
 * @param[in,out] way         Forward waypoint, advanced on success.
 * @param[in]     idx         Chain index the write needs.
 * @param[out]    out_cluster Receives the cluster at @p idx.
 *
 * @return Error code.
 * @retval k_ra8_ok         Cursor positioned; `*out_cluster` is valid.
 * @retval k_ra8_err_no_mem The volume has no free cluster to grow into.
 * @retval k_ra8_err_*      Backend or FAT error.
 *
 * @pre `file`, `way` and `out_cluster` are non-NULL.
 * @pre `way->index <= idx` (a write only moves forward).
 * @post On success the waypoint sits at @p idx.
 * @post On failure the chain may have been partially extended.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_write_position(ra8_fs_file_t* file, write_walk_t* way, uint32_t idx, uint32_t* out_cluster)
{
  ra8_fs_mount_t* m = file->mount;
  if (file->first_cluster < k_cluster_first_data) {
    uint32_t        c   = 0;
    const ra8_err_t err = priv_alloc_eoc_cluster(m, &c);
    if (err != k_ra8_ok) {
      return err;
    }
    file->first_cluster = c;
    file->cur_cluster   = c;
    way->cluster        = c;
    way->index          = idx;
  }
  uint32_t        cur = 0;
  const ra8_err_t err = priv_walk_grow(m, way->cluster, idx - way->index, &cur);
  if (err != k_ra8_ok) {
    return err;
  }
  way->cluster = cur;
  way->index   = idx;
  *out_cluster = cur;
  return k_ra8_ok;
}

/**
 * @brief Inner write loop: stream `len` bytes from `buf` into the file's chain.
 *
 * @details Allocates the first cluster on demand, then walks/grows
 *          the chain as needed and forwards each sector slice through
 *          `priv_write_into_sector`.
 *
 * @param[in,out] file File handle providing chain root and offset.
 * @param[in]     buf  Source buffer.
 * @param[in]     len  Number of bytes to write.
 *
 * @return Error code.
 * @retval k_ra8_ok    All `len` bytes written.
 * @retval k_ra8_err_* Backend, FAT, or no-mem error.
 *
 * @pre All pointers are non-NULL; file is open for writing.
 * @pre `len > 0`.
 * @post On success, `file->offset` advanced by `len`; size grew if needed.
 * @post On failure, partial bytes may already be on disk.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_write_stream(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  ra8_fs_mount_t* m             = file->mount;
  const uint32_t  cluster_bytes = priv_cluster_bytes(m);
  uint32_t        consumed      = 0;
  /* A write may allocate/grow the chain, invalidating any cached read waypoint. */
  file->walk_cache_cluster = 0;
  write_walk_t way         = {.cluster = file->first_cluster, .index = 0U};
  while (consumed < len) {
    const uint32_t cluster_idx_now = (uint32_t)(file->offset / cluster_bytes);
    uint32_t       cur             = 0;
    ra8_err_t      err             = priv_write_position(file, &way, cluster_idx_now, &cur);
    if (err != k_ra8_ok) {
      return err;
    }
    file->cur_cluster                = cur;
    const uint32_t off_in_cluster    = (uint32_t)(file->offset % cluster_bytes);
    const uint32_t sector_in_cluster = off_in_cluster / priv_bps(m);
    const uint32_t off_in_sector     = off_in_cluster % priv_bps(m);
    const uint64_t lba = priv_cluster_to_lba(m, file->cur_cluster) + sector_in_cluster;
    uint32_t       put = priv_bps(m) - off_in_sector;
    if (put > (len - consumed)) {
      put = len - consumed;
    }
    err = priv_write_into_sector(m, lba, off_in_sector, &buf[consumed], put);
    if (err != k_ra8_ok) {
      return err;
    }
    consumed += put;
    file->offset += put;
    if (file->offset > file->size_bytes) {
      file->size_bytes = file->offset;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Write bytes -- the guarded body of ::ra8_fs_write().
 *
 * @details Forwards to `priv_write_stream`, then patches the on-disk
 *          dir entry with the updated first-cluster and file size.
 *
 * @param[in,out] file File handle.
 * @param[in]     buf  Source buffer.
 * @param[in]     len  Bytes to write.
 *
 * @return Error code.
 * @retval k_ra8_ok                All bytes written.
 * @retval k_ra8_err_null_ptr      `file` or `buf` was NULL.
 * @retval k_ra8_err_invalid_state File not open or opened read-only.
 * @retval k_ra8_err_*             Backend, FAT, or no-mem error.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `file` and `buf` are non-NULL.
 * @pre `file->mode != k_ra8_fs_mode_read`.
 * @post On success, the file's dir entry on disk reflects new size.
 * @post On failure, partial bytes may already be on disk.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_write_locked(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  if (file == nullptr || buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U || file->mode == k_ra8_fs_mode_read) {
    return k_ra8_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  if (file->mount->type == k_ra8_fs_type_exfat) {
    const ra8_err_t xe = priv_exfat_write_stream(file, buf, len);
    if (xe != k_ra8_ok) {
      return xe;
    }
    file->dirty = 1U;
    return priv_exfat_flush_set(file);
  }
  /* The FAT boundary of #676: `DIR_FileSize` is 32-bit, so a write that would
   * push the file past 4 GiB - 1 cannot be recorded and is refused whole --
   * truncating the count silently would corrupt the entry on the next flush.
   * exFAT never reaches this test (its branch returned above). */
  if (len > ((uint64_t)k_ra8_fs_fat_max_file_bytes - file->offset)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = priv_write_stream(file, buf, len);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_mount_t* m      = file->mount;
  uint8_t* const  dirsec = priv_sec_walk();
  err                    = priv_read_sector(m, file->dir_entry_lba, dirsec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_entry_set_cluster_size(&dirsec[file->dir_entry_idx],
                              file->first_cluster,
                              (uint32_t)file->size_bytes);
  /* The contents just changed, so the modification time has to move with them
   * (#601). Doing it here as well as at close means a writer that is cut off
   * mid-stream still leaves an mtime describing what is actually on the card,
   * rather than whatever a PC wrote when it created the file. */
  priv_fat_entry_stamp_write(&dirsec[file->dir_entry_idx]);
  /* Same reason the archive attribute rides along: the file was modified, which
   * is exactly what the bit records (#681). */
  priv_fat_entry_apply_attr(&dirsec[file->dir_entry_idx], (uint8_t)k_ra8_fs_attr_archive, 0U);
  file->dirty = 1U;
  return priv_write_sector(m, file->dir_entry_lba, dirsec);
}

/**
 * @brief Create a whole file -- the guarded body of ::ra8_fs_write_file().
 *
 * @details Drives the *unlocked* open / write / close bodies -- taking the
 *          library lock again here would deadlock a non-recursive mutex. The
 *          public ::ra8_fs_write_file() is the wrapper that holds the lock
 *          across all three, so the whole creation is one atomic operation
 *          rather than three.
 *
 *          There is no longer a second, exFAT-only path here: exFAT streams
 *          through the same three calls now that ::ra8_fs_open accepts a
 *          writing mode on it (#602), which is the point -- two
 *          implementations of one verb are two places for it to mean
 *          different things.
 *
 * @param[in,out] handle Mounted volume.
 * @param[in]     path   Root-level file name, UTF-8.
 * @param[in]     data   File contents.
 * @param[in]     len    Byte count.
 *
 * @return Error code.
 * @retval k_ra8_ok                File created and written.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             As documented for ::ra8_fs_write_file().
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle`, `path` and `data` are non-NULL.
 * @post On success @p path resolves to @p len bytes of @p data.
 * @post No file slot stays in use on any return path.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
priv_write_file_locked(ra8_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = priv_open_locked(handle, path, k_ra8_fs_mode_write, &f);
  if (e != k_ra8_ok) {
    return e;
  }
  e                    = priv_write_locked(f, data, len);
  const ra8_err_t cerr = priv_close_locked(f);
  if (e != k_ra8_ok) {
    return e;
  }
  return cerr;
}

/**
 * @brief Move the cursor -- the guarded body of ::ra8_fs_seek().
 *
 * @details Clamps the requested offset to the current file size; the
 *          driver does not implement sparse files.
 *
 * @param[in,out] file         Open file handle.
 * @param[in]     offset_bytes Desired cursor position.
 *
 * @return Error code.
 * @retval k_ra8_ok                Cursor moved (possibly clamped).
 * @retval k_ra8_err_null_ptr      `file` was NULL.
 * @retval k_ra8_err_invalid_state File not open.
 *
 * @pre The library lock is held (or none is installed).
 * @pre File is in use.
 * @post `file->offset == min(offset_bytes, file->size_bytes)`.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_seek_locked(ra8_fs_file_t* file, uint64_t offset_bytes)
{
  if (file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  uint64_t target = offset_bytes;
  if (target > file->size_bytes) {
    target = file->size_bytes;
  }
  file->offset = target;
  return k_ra8_ok;
}

/**
 * @brief Report the cursor -- the guarded body of ::ra8_fs_tell().
 *
 * @details Reads `file->offset`.
 *
 * @param[in]  file       Open file handle.
 * @param[out] out_offset Receives the current offset in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok                Position returned.
 * @retval k_ra8_err_null_ptr      Any pointer was NULL.
 * @retval k_ra8_err_invalid_state File not open.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `file` and `out_offset` are non-NULL.
 * @pre File is in use.
 * @post `*out_offset == file->offset`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_tell_locked(const ra8_fs_file_t* file, uint64_t* out_offset)
{
  if (file == nullptr || out_offset == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  *out_offset = file->offset;
  return k_ra8_ok;
}

/**
 * @brief Report the size -- the guarded body of ::ra8_fs_size().
 *
 * @details Reads `file->size_bytes`.
 *
 * @param[in]  file      Open file handle.
 * @param[out] out_bytes Receives the file size in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok                Size returned.
 * @retval k_ra8_err_null_ptr      Any pointer was NULL.
 * @retval k_ra8_err_invalid_state File not open.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `file` and `out_bytes` are non-NULL.
 * @pre File is in use.
 * @post `*out_bytes == file->size_bytes`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_size_locked(const ra8_fs_file_t* file, uint64_t* out_bytes)
{
  if (file == nullptr || out_bytes == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  *out_bytes = file->size_bytes;
  return k_ra8_ok;
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_read(ra8_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_read_locked(file, buf, max_len, got_len);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_write(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_write_locked(file, buf, len);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t
ra8_fs_write_file(ra8_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_write_file_locked(handle, path, data, len);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_seek(ra8_fs_file_t* file, uint64_t offset_bytes)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_seek_locked(file, offset_bytes);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_tell(const ra8_fs_file_t* file, uint64_t* out_offset)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_tell_locked(file, out_offset);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_size(const ra8_fs_file_t* file, uint64_t* out_bytes)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_size_locked(file, out_bytes);
  priv_lock_release();
  return err;
}
