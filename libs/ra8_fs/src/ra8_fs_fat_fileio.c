/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fs_fat_fileio.c
 * @brief FAT read / write / seek / tell / size I/O.
 *
 * @details
 * Cluster-chain skip/grow, sector-granular read and write streaming, and
 * the public read/write/seek/tell/size entry points.
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
  const uint32_t cluster_bytes   = file->mount->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  const uint32_t cluster_idx_now = file->offset / cluster_bytes;
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
  const uint32_t  off_in_cluster    = file->offset % cluster_bytes;
  const uint32_t  sector_in_cluster = off_in_cluster / k_ra8_fs_bytes_per_sector;
  const uint32_t  off_in_sector     = off_in_cluster % k_ra8_fs_bytes_per_sector;
  const uint32_t  lba = priv_cluster_to_lba(file->mount, file->cur_cluster) + sector_in_cluster;
  uint8_t         sec[k_ra8_fs_bytes_per_sector] = {};
  const ra8_err_t err                            = priv_read_sector(file->mount, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t take = k_ra8_fs_bytes_per_sector - off_in_sector;
  if (take > remaining) {
    take = remaining;
  }
  priv_byte_copy(buf, &sec[off_in_sector], take);
  *out_take = take;
  return k_ra8_ok;
}

/**
 * @brief Read bytes from an open file.
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
ra8_err_t ra8_fs_read(ra8_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
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
  uint32_t remaining = file->size_bytes - file->offset;
  if (remaining > max_len) {
    remaining = max_len;
  }
  uint32_t produced = 0;
  while (remaining > 0U) {
    uint32_t  take = 0;
    ra8_err_t err  = priv_read_one_chunk(file, &buf[produced], remaining, &take);
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

/**
 * @brief Write `put` bytes into one sector at `lba` starting at `off_in_sector`.
 *
 * @details Read-modify-write of a single sector. Used by the write
 *          path so partial-sector updates do not destroy neighbouring
 *          file data.
 *
 * @param[in] m             Mount providing the backend.
 * @param[in] lba           Sector to update.
 * @param[in] off_in_sector Byte offset within the sector.
 * @param[in] src           Source bytes.
 * @param[in] put           Number of bytes to write.
 *
 * @return Error code.
 * @retval k_ra8_ok    Sector updated.
 * @retval k_ra8_err_* Backend read or write failure.
 *
 * @pre `m` and `src` are non-NULL.
 * @pre `off_in_sector + put <= k_ra8_fs_bytes_per_sector`.
 * @post On success, the sector reflects the merged content.
 * @post On failure, the sector content is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_write_into_sector(const ra8_fs_mount_t* m,
                                        uint32_t              lba,
                                        uint32_t              off_in_sector,
                                        const uint8_t*        src,
                                        uint32_t              put)
{
  uint8_t   sec[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t err                            = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_byte_copy(&sec[off_in_sector], src, put);
  return priv_write_sector(m, lba, sec);
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
  const uint32_t  cluster_bytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  uint32_t        consumed      = 0;
  /* A write may allocate/grow the chain, invalidating any cached read waypoint. */
  file->walk_cache_cluster = 0;
  while (consumed < len) {
    if (file->first_cluster < k_cluster_first_data) {
      uint32_t  c   = 0;
      ra8_err_t err = priv_alloc_eoc_cluster(m, &c);
      if (err != k_ra8_ok) {
        return err;
      }
      file->first_cluster = c;
      file->cur_cluster   = c;
    }
    const uint32_t cluster_idx_now = file->offset / cluster_bytes;
    uint32_t       cur             = 0;
    ra8_err_t      err             = priv_walk_grow(m, file->first_cluster, cluster_idx_now, &cur);
    if (err != k_ra8_ok) {
      return err;
    }
    file->cur_cluster                = cur;
    const uint32_t off_in_cluster    = file->offset % cluster_bytes;
    const uint32_t sector_in_cluster = off_in_cluster / k_ra8_fs_bytes_per_sector;
    const uint32_t off_in_sector     = off_in_cluster % k_ra8_fs_bytes_per_sector;
    const uint32_t lba = priv_cluster_to_lba(m, file->cur_cluster) + sector_in_cluster;
    uint32_t       put = k_ra8_fs_bytes_per_sector - off_in_sector;
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
 * @brief Write bytes to an open file.
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
 * @pre `file` and `buf` are non-NULL.
 * @pre `file->mode != k_ra8_fs_mode_read`.
 * @post On success, the file's dir entry on disk reflects new size.
 * @post On failure, partial bytes may already be on disk.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_write(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len)
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
  ra8_err_t err = priv_write_stream(file, buf, len);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_mount_t* m                                 = file->mount;
  uint8_t         dirsec[k_ra8_fs_bytes_per_sector] = {};
  err = priv_read_sector(m, file->dir_entry_lba, dirsec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_entry_set_cluster_size(&dirsec[file->dir_entry_idx], file->first_cluster, file->size_bytes);
  return priv_write_sector(m, file->dir_entry_lba, dirsec);
}

ra8_err_t
ra8_fs_write_file(ra8_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len)
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
  if (handle->type == k_ra8_fs_type_exfat) {
    return priv_exfat_create(handle, path, data, len);
  }
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(handle, path, k_ra8_fs_mode_write, &f);
  if (e != k_ra8_ok) {
    return e;
  }
  e                    = ra8_fs_write(f, data, len);
  const ra8_err_t cerr = ra8_fs_close(f);
  if (e != k_ra8_ok) {
    return e;
  }
  return cerr;
}

/**
 * @brief Move the file's read/write cursor.
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
 * @pre `file` is non-NULL.
 * @pre File is in use.
 * @post `file->offset == min(offset_bytes, file->size_bytes)`.
 * @post No on-disk state modified.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_seek(ra8_fs_file_t* file, uint32_t offset_bytes)
{
  if (file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  uint32_t target = offset_bytes;
  if (target > file->size_bytes) {
    target = file->size_bytes;
  }
  file->offset = target;
  return k_ra8_ok;
}

/**
 * @brief Report the file's current cursor position.
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
 * @pre `file` and `out_offset` are non-NULL.
 * @pre File is in use.
 * @post `*out_offset == file->offset`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_tell(const ra8_fs_file_t* file, uint32_t* out_offset)
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
 * @brief Report the file's size in bytes.
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
 * @pre `file` and `out_bytes` are non-NULL.
 * @pre File is in use.
 * @post `*out_bytes == file->size_bytes`.
 * @post No state modified.
 *
 * @note Thread-safety: single-word read; safe vs other readers.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_fs_size(const ra8_fs_file_t* file, uint32_t* out_bytes)
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
