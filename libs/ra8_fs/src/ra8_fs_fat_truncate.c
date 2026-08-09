/**
 * @file ra8_fs_fat_truncate.c
 * @brief ra8_fs_truncate: shrink or grow an open file to an arbitrary length.
 *
 * @details
 * The `ftruncate()` verb, in both directions, on both filesystems (#680). Until
 * now the only truncation was truncate-to-zero on `open(write)`, and a file
 * could not be given a length between "zero" and "everything a writer streamed
 * into it": `ra8_fs_seek()` clamps to the current size, so a cursor cannot be
 * placed past EOF to extend, and there was no verb to trim to N > 0 bytes.
 *
 * The shrink half returns the tail clusters to the volume and lowers the
 * recorded length. The grow half is where the two filesystems differ, and both
 * differences are the format speaking:
 *
 *   - **FAT** has one length. The bytes between the old end and the new one are
 *     read straight off the clusters that back them, so they have to BE zero on
 *     disk -- the fresh clusters are zero-filled, and so is the slack of the
 *     last old cluster.
 *   - **exFAT** has two lengths (spec sec 7.4.5). Growing sets `DataLength` to
 *     the new size and leaves `ValidDataLength` at the written length, so the
 *     gap is a valid unwritten PREFIX suffix the read path already serves as
 *     zero -- no bytes are written for it. A grow that runs past the volume's
 *     contiguous space converts the run from `NoFatChain` to a real FAT chain,
 *     reusing the identical streaming-write machinery (#602).
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, March 2021.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: every loop is bounded by the file's cluster count, the byte
 *     range being zeroed, or the sectors in one cluster.
 *   - Rule 3: zero malloc; the largest buffer is one sector on the stack.
 *   - Rule 7: every bitmap, FAT and backend call is checked.
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

/* =============================================================================
 * Shared helpers
 * =============================================================================
 */

/**
 * @brief Clusters needed to hold @p bytes at @p cbytes bytes per cluster.
 *
 * @details Ceiling division, and 0 for a zero-length file -- an empty file owns
 *          no clusters on either filesystem, so the caller frees the whole
 *          allocation rather than keeping one.
 *
 * @param[in] bytes  Byte length to size.
 * @param[in] cbytes Bytes per cluster (a power of two >= 512).
 *
 * @return Cluster count.
 * @retval 0            @p bytes is 0.
 * @retval >0           ceil(@p bytes / @p cbytes).
 *
 * @pre @p cbytes is non-zero.
 * @pre @p bytes + @p cbytes does not overflow (true: both derive from a 32-bit
 *      file length and a small cluster size).
 * @post No state is modified.
 * @post The result depends only on the inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_trunc_clusters(uint32_t bytes, uint32_t cbytes)
{
  return (bytes + cbytes - 1U) / cbytes;
}

/**
 * @brief Walk @p idx clusters forward along a chain from @p start.
 *
 * @details The shared chain walker for the shrink paths: a FAT file's tail is
 *          only reachable by following its FAT links, and an exFAT chained file
 *          is the same. A chain that ends before @p idx is a corrupt file, not
 *          a short one, so it is reported rather than clamped.
 *
 * @param[in]  m     Mount providing FAT access.
 * @param[in]  start First cluster of the chain.
 * @param[in]  idx   Clusters to advance (0 returns @p start).
 * @param[out] out   Receives the cluster reached.
 *
 * @return Error code.
 * @retval k_ra8_ok                 `*out` is the cluster at index @p idx.
 * @retval k_ra8_err_protocol_error The chain ended before @p idx.
 * @retval k_ra8_err_*              Backend or FAT error.
 *
 * @pre @p m and @p out are non-NULL; @p start is a real data cluster.
 * @pre @p idx is within the file's real cluster count.
 * @post On success `*out` is a real data cluster.
 * @post No volume state is modified.
 *
 * @note The loop is bounded by @p idx, itself bounded by the file's length.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_trunc_walk(const ra8_fs_mount_t* m, uint32_t start, uint32_t idx, uint32_t* out)
{
  uint32_t cur = start;
  for (uint32_t i = 0U; i < idx; i++) {
    uint32_t        next = 0U;
    const ra8_err_t e    = priv_fat_get(m, cur, &next);
    if (e != k_ra8_ok) {
      return e;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra8_err_protocol_error;
    }
    cur = next;
  }
  *out = cur;
  return k_ra8_ok;
}

/**
 * @brief Write real zeros over the byte range `[lo, hi)` inside one cluster.
 *
 * @details Sector by sector, read-modify-write: every sector the range touches
 *          is read, the covered bytes are cleared, and it is written back. That
 *          preserves neighbouring bytes on the two edge sectors -- the file data
 *          below @p lo in the first, whatever sits above @p hi in the last --
 *          which is exactly what a FAT grow needs when the old length ends
 *          mid-sector. Zeroing a whole fresh cluster is the same call with
 *          `lo == 0` and `hi == cluster bytes`.
 *
 * @param[in] m       Mount providing the backend.
 * @param[in] cluster Cluster to zero within.
 * @param[in] lo      First byte to clear (offset within the cluster).
 * @param[in] hi      One past the last byte to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok    The range reads as zero (or `lo >= hi`, nothing to do).
 * @retval k_ra8_err_* Backend read or write failure.
 *
 * @pre @p m is non-NULL; `hi` is within one cluster.
 * @pre @p cluster is an allocated data cluster.
 * @post On success bytes `[lo, hi)` of @p cluster read as zero.
 * @post Bytes outside `[lo, hi)` are unchanged.
 *
 * @note The loop is bounded by the sectors the range spans (at most one
 *       cluster's worth).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_trunc_zero_span(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t lo, uint32_t hi)
{
  if (lo >= hi) {
    return k_ra8_ok;
  }
  const uint32_t base_lba = priv_cluster_to_lba(m, cluster);
  uint32_t       off      = lo;
  while (off < hi) {
    const uint32_t sec_idx   = off / (uint32_t)k_ra8_fs_bytes_per_sector;
    const uint32_t sec_start = sec_idx * (uint32_t)k_ra8_fs_bytes_per_sector;
    const uint32_t in_sector = off - sec_start;
    uint32_t       n         = (uint32_t)k_ra8_fs_bytes_per_sector - in_sector;
    if (n > (hi - off)) {
      n = hi - off;
    }
    uint8_t         sec[k_ra8_fs_bytes_per_sector] = {};
    const ra8_err_t re                             = priv_read_sector(m, base_lba + sec_idx, sec);
    if (re != k_ra8_ok) {
      return re;
    }
    for (uint32_t i = 0U; i < n; i++) {
      sec[in_sector + i] = 0U;
    }
    const ra8_err_t we = priv_write_sector(m, base_lba + sec_idx, sec);
    if (we != k_ra8_ok) {
      return we;
    }
    off += n;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * FAT12/16/32 truncate
 * =============================================================================
 */

/**
 * @brief Free the cluster tail past @p new_size on a FAT file.
 *
 * @details Below the new cluster count the chain is left alone; at and above it
 *          the survivor's tail is re-capped with an end-of-chain marker and the
 *          rest is freed. Shrinking to zero drops the whole chain and clears the
 *          file's first cluster, so a later grow starts from nothing.
 *
 * @param[in,out] file     File being shrunk; `first_cluster` and `size_bytes`
 *                         are updated in place.
 * @param[in]     new_size New length in bytes (`< size_bytes`).
 * @param[in]     cbytes   Bytes per cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    The file owns exactly the clusters @p new_size needs.
 * @retval k_ra8_err_* Backend or FAT error.
 *
 * @pre @p file is non-NULL, in use, and on a FAT12/16/32 volume.
 * @pre `new_size < file->size_bytes`.
 * @post On success `size_bytes == new_size` and the freed clusters read free.
 * @post On success the survivor's tail FAT entry is an end-of-chain marker.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_trunc_shrink(ra8_fs_file_t* file, uint32_t new_size, uint32_t cbytes)
{
  ra8_fs_mount_t* m         = file->mount;
  const uint32_t  new_nclus = priv_trunc_clusters(new_size, cbytes);
  if (new_nclus == 0U) {
    if (file->first_cluster >= (uint32_t)k_cluster_first_data) {
      const ra8_err_t e = priv_free_chain(m, file->first_cluster);
      if (e != k_ra8_ok) {
        return e;
      }
    }
    file->first_cluster = 0U;
    file->size_bytes    = new_size;
    return k_ra8_ok;
  }
  uint32_t        new_tail = 0U;
  const ra8_err_t we       = priv_trunc_walk(m, file->first_cluster, new_nclus - 1U, &new_tail);
  if (we != k_ra8_ok) {
    return we;
  }
  uint32_t        succ = 0U;
  const ra8_err_t ge   = priv_fat_get(m, new_tail, &succ);
  if (ge != k_ra8_ok) {
    return ge;
  }
  if (priv_is_eoc(m, succ) == 0U) {
    ra8_err_t e = priv_fat_set(m, new_tail, priv_eoc_write(m));
    if (e != k_ra8_ok) {
      return e;
    }
    e = priv_free_chain(m, succ);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  file->size_bytes = new_size;
  return k_ra8_ok;
}

/**
 * @brief Append zeroed clusters to a FAT file until it owns @p new_nclus.
 *
 * @details Allocates the first cluster when the file has none, then links a
 *          fresh end-of-chain cluster on at a time, zeroing each as it is
 *          created so the grown region reads back as zero. One forward pass, so
 *          the cost is linear in the clusters added, not in the file's length.
 *
 * @param[in,out] file      File being grown; `first_cluster` is set when empty.
 * @param[in]     tail      Current tail cluster (ignored when @p have is 0).
 * @param[in]     have      Clusters the file already owns.
 * @param[in]     new_nclus Clusters the file must own on return.
 * @param[in]     cbytes    Bytes per cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok         The file owns @p new_nclus zeroed clusters.
 * @retval k_ra8_err_no_mem The volume ran out of free clusters.
 * @retval k_ra8_err_*      Backend or FAT error.
 *
 * @pre @p file is non-NULL and on a FAT12/16/32 volume.
 * @pre `have <= new_nclus`; when `have > 0`, @p tail is the current tail.
 * @post On success every cluster from index @p have up reads as zero.
 * @post On failure any cluster taken is at worst leaked, reclaimable by fsck.
 *
 * @note The loop is bounded by @p new_nclus, itself bounded by the volume.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_trunc_extend(ra8_fs_file_t* file,
                                       uint32_t       tail,
                                       uint32_t       have,
                                       uint32_t       new_nclus,
                                       uint32_t       cbytes)
{
  ra8_fs_mount_t* m   = file->mount;
  uint32_t        cur = tail;
  if (have == 0U) {
    ra8_err_t e = priv_alloc_eoc_cluster(m, &cur);
    if (e != k_ra8_ok) {
      return e;
    }
    file->first_cluster = cur;
    e                   = priv_trunc_zero_span(m, cur, 0U, cbytes);
    if (e != k_ra8_ok) {
      return e;
    }
    have = 1U;
  }
  while (have < new_nclus) {
    uint32_t  c = 0U;
    ra8_err_t e = priv_alloc_eoc_cluster(m, &c);
    if (e != k_ra8_ok) {
      return e;
    }
    e = priv_fat_set(m, cur, c);
    if (e != k_ra8_ok) {
      return e;
    }
    e = priv_trunc_zero_span(m, c, 0U, cbytes);
    if (e != k_ra8_ok) {
      return e;
    }
    cur = c;
    have++;
  }
  return k_ra8_ok;
}

/**
 * @brief Grow a FAT file to @p new_size, zero-filling `[old_size, new_size)`.
 *
 * @details Two things must read as zero: the slack of the last old cluster
 *          past the old length, and every newly allocated cluster. The slack is
 *          cleared first (a read-modify-write that keeps the real file data
 *          below it), then the chain is extended from that same cluster so it is
 *          walked only once.
 *
 * @param[in,out] file     File being grown; `first_cluster` / `size_bytes` move.
 * @param[in]     new_size New length in bytes (`> size_bytes`).
 * @param[in]     cbytes   Bytes per cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok         The file is @p new_size bytes and the gap reads zero.
 * @retval k_ra8_err_no_mem The volume ran out of free clusters.
 * @retval k_ra8_err_*      Backend or FAT error.
 *
 * @pre @p file is non-NULL, in use, and on a FAT12/16/32 volume.
 * @pre `new_size > file->size_bytes`.
 * @post On success `size_bytes == new_size`.
 * @post On success a read of `[old_size, new_size)` returns zeros.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_trunc_grow(ra8_fs_file_t* file, uint32_t new_size, uint32_t cbytes)
{
  ra8_fs_mount_t* m         = file->mount;
  const uint32_t  old_size  = file->size_bytes;
  const uint32_t  old_nclus = priv_trunc_clusters(old_size, cbytes);
  const uint32_t  new_nclus = priv_trunc_clusters(new_size, cbytes);
  uint32_t        tail      = 0U;
  if (old_nclus > 0U) {
    const ra8_err_t we = priv_trunc_walk(m, file->first_cluster, old_nclus - 1U, &tail);
    if (we != k_ra8_ok) {
      return we;
    }
    const uint32_t  base    = (old_nclus - 1U) * cbytes;
    const uint32_t  cap     = old_nclus * cbytes;
    const uint32_t  span_hi = (new_size < cap) ? new_size : cap;
    const ra8_err_t ze      = priv_trunc_zero_span(m, tail, old_size - base, span_hi - base);
    if (ze != k_ra8_ok) {
      return ze;
    }
  }
  const ra8_err_t ee = priv_fat_trunc_extend(file, tail, old_nclus, new_nclus, cbytes);
  if (ee != k_ra8_ok) {
    return ee;
  }
  file->size_bytes = new_size;
  return k_ra8_ok;
}

/**
 * @brief Rewrite a FAT file's directory entry after its length changed.
 *
 * @details Patches the entry's first cluster and size, and stamps the write
 *          time -- a truncate is a content change even when no byte is written,
 *          so an mtime that still described the old contents would be a lie
 *          (#601). Leaves the handle dirty so close flushes FSInfo.
 *
 * @param[in,out] file File whose entry must match its new length.
 *
 * @return Error code.
 * @retval k_ra8_ok    The on-disk entry reports the new first cluster and size.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p file is non-NULL; `dir_entry_lba` / `dir_entry_idx` locate its entry.
 * @pre The mount is a FAT12/16/32 volume.
 * @post On success the entry's size equals `file->size_bytes`.
 * @post On success `file->dirty` is 1.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_trunc_commit(ra8_fs_file_t* file)
{
  ra8_fs_mount_t* m                              = file->mount;
  uint8_t         sec[k_ra8_fs_bytes_per_sector] = {};
  const ra8_err_t re                             = priv_read_sector(m, file->dir_entry_lba, sec);
  if (re != k_ra8_ok) {
    return re;
  }
  priv_entry_set_cluster_size(&sec[file->dir_entry_idx], file->first_cluster, file->size_bytes);
  priv_fat_entry_stamp_write(&sec[file->dir_entry_idx]);
  const ra8_err_t we = priv_write_sector(m, file->dir_entry_lba, sec);
  if (we != k_ra8_ok) {
    return we;
  }
  file->dirty = 1U;
  return k_ra8_ok;
}

/**
 * @brief Truncate a FAT file to @p new_size and commit its directory entry.
 *
 * @details Dispatches to the shrink or grow branch, resets the read
 *          accelerator (a shrink can free the cluster it cached, a grow moves
 *          the tail), and rewrites the directory entry once.
 *
 * @param[in,out] file     Open FAT file handle.
 * @param[in]     new_size Desired length in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok    The file is @p new_size bytes and its entry matches.
 * @retval k_ra8_err_* Backend, FAT, or no-memory error.
 *
 * @pre @p file is non-NULL, in use, and opened for writing on a FAT volume.
 * @pre `file->mount` is a mounted FAT12/16/32 volume.
 * @post On success `size_bytes == new_size` and the entry is consistent.
 * @post The read accelerator is invalidated.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat_trunc(ra8_fs_file_t* file, uint32_t new_size)
{
  const uint32_t cbytes = file->mount->sectors_per_cluster * (uint32_t)k_ra8_fs_bytes_per_sector;
  ra8_err_t      e      = k_ra8_ok;
  if (new_size < file->size_bytes) {
    e = priv_fat_trunc_shrink(file, new_size, cbytes);
  } else if (new_size > file->size_bytes) {
    e = priv_fat_trunc_grow(file, new_size, cbytes);
  }
  if (e != k_ra8_ok) {
    return e;
  }
  file->cur_cluster        = file->first_cluster;
  file->walk_cache_idx     = 0U;
  file->walk_cache_cluster = file->first_cluster;
  return priv_fat_trunc_commit(file);
}

/* =============================================================================
 * exFAT truncate
 * =============================================================================
 */

/**
 * @brief Release a cluster run by describing it as a Stream-extension entry.
 *
 * @details ::priv_exfat_free_clusters already frees either a contiguous run or a
 *          FAT chain given a file's Stream entry, so the shrink path hands it a
 *          Stream entry describing exactly the SUB-run to release: the first
 *          cluster, a byte length whose ceiling is the run's cluster count, and
 *          the `NoFatChain` flag that selects contiguous-count vs chain-walk
 *          freeing. Nothing on disk is a real entry -- it is a 32-byte argument.
 *
 * @param[in] m      Mounted exFAT volume.
 * @param[in] first  First cluster of the run to free.
 * @param[in] nbytes Byte length whose cluster ceiling is the run length (> 0).
 * @param[in] nofat  1 to free a contiguous run, 0 to walk a FAT chain.
 *
 * @return Error code.
 * @retval k_ra8_ok    The run's clusters read as free in the allocation bitmap.
 * @retval k_ra8_err_* Bitmap or backend failure.
 *
 * @pre @p m is non-NULL; @p first is a real data cluster; @p nbytes > 0.
 * @pre When @p nofat is 0 the chain from @p first terminates at an EOC marker.
 * @post On success the described run reads as free.
 * @post FAT contents of the freed clusters are left as-is (bitmap is authority).
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_free_run(const ra8_fs_mount_t* m, uint32_t first, uint32_t nbytes, uint8_t nofat)
{
  uint8_t strm[k_exfat_entry_bytes] = {};
  priv_wr32(&strm[k_exfat_strm_off_clus], first);
  priv_wr32(&strm[k_exfat_strm_off_dlen], nbytes);
  strm[k_exfat_strm_off_flags] =
    (nofat != 0U) ? (uint8_t)k_exfat_secflag_alloc : (uint8_t)k_exfat_secflag_poss;
  return priv_exfat_free_clusters(m, strm);
}

/**
 * @brief Trim a FAT-chained exFAT file to @p new_nclus clusters.
 *
 * @details The chained arm of the shrink, split out so ::priv_exfat_trunc_shrink
 *          stays inside the statement budget. Walks to the survivor's new tail,
 *          caps it with an end-of-chain marker, and frees the remainder off the
 *          allocation bitmap by walking the FAT from the tail's old successor.
 *          The freed clusters keep their stale FAT links -- the bitmap is exFAT's
 *          authority (spec sec 7.1) and nothing follows a free cluster's chain.
 *
 * @param[in,out] file      Chained file being shrunk; `tail_cluster` is updated.
 * @param[in]     new_nclus Clusters to keep (>= 1, < the current allocation).
 * @param[in]     cbytes    Bytes per cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    The survivor ends at a fresh EOC and the tail is freed.
 * @retval k_ra8_err_* Bitmap, FAT, or backend error.
 *
 * @pre @p file is non-NULL, chained (`no_fat_chain == 0`), on an exFAT volume.
 * @pre `1 <= new_nclus < file->alloc_clusters`.
 * @post On success `tail_cluster` is the survivor's last cluster.
 * @post On success the freed clusters read as free in the allocation bitmap.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_shrink_chain_tail(ra8_fs_file_t* file, uint32_t new_nclus, uint32_t cbytes)
{
  ra8_fs_mount_t* m        = file->mount;
  uint32_t        new_tail = 0U;
  const ra8_err_t we       = priv_trunc_walk(m, file->first_cluster, new_nclus - 1U, &new_tail);
  if (we != k_ra8_ok) {
    return we;
  }
  uint32_t        succ = 0U;
  const ra8_err_t ge   = priv_fat_get(m, new_tail, &succ);
  if (ge != k_ra8_ok) {
    return ge;
  }
  ra8_err_t e = priv_fat_set(m, new_tail, priv_eoc_write(m));
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_exfat_free_run(m, succ, cbytes, 0U);
  if (e != k_ra8_ok) {
    return e;
  }
  file->tail_cluster = new_tail;
  return k_ra8_ok;
}

/**
 * @brief Free an exFAT file's cluster tail past @p new_size.
 *
 * @details A contiguous run keeps `[first, first + new_nclus)` and frees the
 *          rest by count; a FAT-chained run re-terminates the survivor's tail
 *          and walks the freed remainder off the bitmap. Shrinking to zero drops
 *          the whole allocation and returns the handle to the empty shape a
 *          fresh `open(write)` produces.
 *
 * @param[in,out] file     File being shrunk; allocation fields are updated.
 * @param[in]     new_size New length in bytes (`< size_bytes`).
 * @param[in]     cbytes   Bytes per cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    The file owns exactly the clusters @p new_size needs.
 * @retval k_ra8_err_* Bitmap, FAT, or backend error.
 *
 * @pre @p file is non-NULL, in use, and on an exFAT volume.
 * @pre `new_size < file->size_bytes` and the allocation fields are current.
 * @post On success `size_bytes == new_size` and `valid_bytes <= new_size`.
 * @post On success the freed clusters read as free in the allocation bitmap.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_trunc_shrink(ra8_fs_file_t* file, uint32_t new_size, uint32_t cbytes)
{
  ra8_fs_mount_t* m         = file->mount;
  const uint32_t  new_nclus = priv_trunc_clusters(new_size, cbytes);
  if (new_nclus < file->alloc_clusters) {
    if (new_nclus == 0U) {
      const ra8_err_t e =
        priv_exfat_free_run(m, file->first_cluster, file->size_bytes, file->no_fat_chain);
      if (e != k_ra8_ok) {
        return e;
      }
      file->first_cluster = 0U;
      file->no_fat_chain  = 1U;
      file->tail_cluster  = 0U;
    } else if (file->no_fat_chain != 0U) {
      const uint32_t  first = file->first_cluster + new_nclus;
      const ra8_err_t e =
        priv_exfat_free_run(m, first, (file->alloc_clusters - new_nclus) * cbytes, 1U);
      if (e != k_ra8_ok) {
        return e;
      }
      file->tail_cluster = file->first_cluster + new_nclus - 1U;
    } else {
      const ra8_err_t e = priv_exfat_shrink_chain_tail(file, new_nclus, cbytes);
      if (e != k_ra8_ok) {
        return e;
      }
    }
    file->alloc_clusters = new_nclus;
  }
  file->size_bytes = new_size;
  if (file->valid_bytes > new_size) {
    file->valid_bytes = new_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Truncate an exFAT file to @p new_size and flush its entry set.
 *
 * @details Shrink frees the tail; grow takes clusters from the allocation
 *          bitmap (converting a run that outgrows its contiguous space to a FAT
 *          chain, exactly as a streaming write does) and raises `DataLength`
 *          alone -- `ValidDataLength` stays at the written prefix, so the read
 *          path serves the grown gap as zero without a byte being written for
 *          it (exFAT spec sec 7.4.5). Either way the entry set is rewritten so
 *          a later mount sees the new length.
 *
 * @param[in,out] file     Open exFAT file handle.
 * @param[in]     new_size Desired length in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok         The file is @p new_size bytes and its set matches.
 * @retval k_ra8_err_no_mem A grow ran out of free clusters.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend error.
 *
 * @pre @p file is non-NULL, in use, and opened for writing on an exFAT volume.
 * @pre `file->entry_set_*` locate the file's directory entry set.
 * @post On success `size_bytes == new_size` and the Stream entry agrees.
 * @post On a grow `valid_bytes` is unchanged, so the gap reads as zero.
 *
 * @note Not thread-safe; the public entry holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_trunc(ra8_fs_file_t* file, uint32_t new_size)
{
  const uint32_t cbytes = file->mount->sectors_per_cluster * (uint32_t)k_ra8_fs_bytes_per_sector;
  if (new_size < file->size_bytes) {
    const ra8_err_t e = priv_exfat_trunc_shrink(file, new_size, cbytes);
    if (e != k_ra8_ok) {
      return e;
    }
  } else if (new_size > file->size_bytes) {
    const ra8_err_t e = priv_exfat_ensure_clusters(file, priv_trunc_clusters(new_size, cbytes));
    if (e != k_ra8_ok) {
      return e;
    }
    file->size_bytes = new_size;
  } else {
    return k_ra8_ok;
  }
  return priv_exfat_flush_set(file);
}

/* =============================================================================
 * Dispatch and the public entry point
 * =============================================================================
 */

/**
 * @brief Truncate the file behind @p file -- the guarded body of ::ra8_fs_truncate.
 *
 * @details Validates the handle, dispatches on the mount's filesystem, and
 *          finally pulls the offset down if a shrink left it past the new end
 *          (the offset is otherwise left where the caller had it, as
 *          `ftruncate()` promises).
 *
 * @param[in,out] file     Open file handle in a writing mode.
 * @param[in]     new_size Desired length in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok                Length set.
 * @retval k_ra8_err_null_ptr      @p file is NULL.
 * @retval k_ra8_err_invalid_state Not open, or opened read-only.
 * @retval k_ra8_err_no_mem        A grow ran out of free clusters.
 * @retval k_ra8_err_*             Backend, FAT, or bitmap failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p file came from ::ra8_fs_open in write or append mode.
 * @post On success `file->size_bytes == new_size`.
 * @post On success `file->offset == min(old_offset, new_size)`.
 *
 * @note Not thread-safe per file; callers serialise.
 *
 * @note The mode/handle guard is one compound decision of two conditions -- a
 *       closed handle, or a handle opened read-only -- and its vectors live with
 *       the test that drives it, cited as
 *       `libs/ra8_fs/src/ra8_fs_fat_truncate.c@priv_truncate_locked`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_truncate_locked(ra8_fs_file_t* file, uint32_t new_size)
{
  if (file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (file->in_use == 0U || file->mode == k_ra8_fs_mode_read) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t e = k_ra8_ok;
  if (file->mount->type == k_ra8_fs_type_exfat) {
    e = priv_exfat_trunc(file, new_size);
  } else {
    e = priv_fat_trunc(file, new_size);
  }
  if (e != k_ra8_ok) {
    return e;
  }
  if (file->offset > file->size_bytes) {
    file->offset = file->size_bytes;
  }
  return k_ra8_ok;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_truncate(ra8_fs_file_t* file, uint32_t new_size)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_truncate_locked(file, new_size);
  priv_lock_release();
  return err;
}
