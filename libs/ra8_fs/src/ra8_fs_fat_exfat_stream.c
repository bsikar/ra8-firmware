/**
 * @file ra8_fs_fat_exfat_stream.c
 * @brief exFAT streaming write: grow the allocation, keep the entry set true.
 *
 * @details
 * The engine behind `ra8_fs_open(write|append)` on an exFAT volume (#602).
 * Three mechanisms live here and nothing else does:
 *
 * 1. **Growth.** A file takes one cluster at a time out of the allocation
 *    bitmap. Before scanning it PROBES the successor of its current tail,
 *    because a run that stays contiguous keeps `NoFatChain` set and therefore
 *    keeps the read path's O(1) cluster arithmetic.
 *
 * 2. **The `NoFatChain` transition.** exFAT spec sec 7.4.2: while the flag is
 *    set the FAT entries for a file's clusters carry nothing an implementation
 *    may read, so the moment the next cluster is NOT the successor of the last
 *    the flag has to come off -- and coming off means first writing a real FAT
 *    chain over every cluster already allocated, then linking the new one, then
 *    clearing the bit in the entry set. Doing it in any other order leaves a
 *    file the format says is contiguous and the FAT says is not.
 *
 * 3. **`ValidDataLength` vs `DataLength`.** exFAT spec sec 7.4.5 makes the
 *    first a PREFIX length: bytes below it were written, bytes between it and
 *    `DataLength` were never initialised and must read as zero. That is why
 *    ::priv_exfat_write_stream closes any gap with real zeros before writing --
 *    a hole in the MIDDLE of a file has no encoding, so it must not be created
 *    -- and why the read path serves zeros past the prefix instead of whatever
 *    the previous tenant of those clusters left behind.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, March 2021.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the growth walk is bounded by `count_of_clusters`, the entry-set
 *     walk by `k_exfat_set_writable`, and the byte loops by the byte count.
 *   - Rule 3: zero malloc; the largest buffer is one entry set on the stack.
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

/* =============================================================================
 * Growth: one cluster at a time, contiguous while it can be
 * =============================================================================
 */

/**
 * @brief Choose the next cluster to allocate, preferring the tail's successor.
 *
 * @details Probes @p prefer first and falls back to the hinted bitmap scan.
 *          The probe is one sector read and it is what keeps a sequentially
 *          written file contiguous on a volume that has room for it.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[in]  bmp_lba First LBA (volume-relative) of the allocation bitmap.
 * @param[in]  prefer  Cluster to take if it is free; 0 asks for any.
 * @param[out] out     Receives the chosen cluster number.
 *
 * @return Error code.
 * @retval k_ra8_ok         A free cluster was chosen.
 * @retval k_ra8_err_no_mem The volume has no free cluster at all.
 * @retval k_ra8_err_*      Backend read failure.
 *
 * @pre @p m and @p out are non-NULL; `m->type` is exFAT.
 * @pre @p bmp_lba came from ::priv_exfat_bitmap_lba.
 * @post On success `*out` is a cluster whose bitmap bit currently reads 0.
 * @post No volume state is modified -- the caller marks the bit.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @note The range guard on @p prefer is one compound decision of two
 *       conditions; its vectors live with the tests that drive it, cited as
 *       `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_pick_cluster`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_pick_cluster(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t prefer, uint32_t* out)
{
  const uint32_t past_end = (uint32_t)k_cluster_first_data + m->count_of_clusters;
  if ((prefer >= (uint32_t)k_cluster_first_data) && (prefer < past_end)) {
    uint8_t         is_free = 0U;
    const ra8_err_t pe      = priv_exfat_bitmap_test(m, bmp_lba, prefer, &is_free);
    if (pe != k_ra8_ok) {
      return pe;
    }
    if (is_free != 0U) {
      *out = prefer;
      return k_ra8_ok;
    }
  }
  return priv_exfat_bitmap_scan(m, bmp_lba, 1U, out);
}

/**
 * @brief Write a real FAT chain over a run that was contiguous until now.
 *
 * @details The one-way half of the `NoFatChain` transition. Links every
 *          already-allocated cluster to its successor and the tail to @p next,
 *          which is what makes the run readable once the flag comes off. The
 *          caller clears the flag only after this returns, so an interrupted
 *          transition leaves a set that still says "contiguous" over clusters
 *          that still are.
 *
 * @param[in] file File whose contiguous run is being converted.
 * @param[in] next The newly allocated cluster the tail must point at.
 *
 * @return Error code.
 * @retval k_ra8_ok    The chain now spans the run and reaches @p next.
 * @retval k_ra8_err_* FAT write failure; the transition is incomplete.
 *
 * @pre @p file is non-NULL with `no_fat_chain != 0` and `alloc_clusters >= 1`.
 * @pre The run is `[first_cluster, first_cluster + alloc_clusters)`.
 * @post On success `FAT[tail_cluster]` is @p next and every earlier cluster
 *       points at its successor.
 * @post The allocation bitmap is untouched.
 *
 * @note O(alloc_clusters) FAT writes, paid once per file that fragments.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_materialize_chain(const ra8_fs_file_t* file, uint32_t next)
{
  const ra8_fs_mount_t* m = file->mount;
  for (uint32_t i = 0U; (i + 1U) < file->alloc_clusters; i++) {
    const uint32_t  here = file->first_cluster + i;
    const ra8_err_t e    = priv_fat_set(m, here, here + 1U);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return priv_fat_set(m, file->tail_cluster, next);
}

/**
 * @brief Attach a freshly allocated cluster to the file's allocation.
 *
 * @details Three cases, and the middle one is the whole point of the format's
 *          `NoFatChain` bit: the file has no clusters yet (nothing to link),
 *          the new cluster continues the contiguous run (still nothing to
 *          link -- writing FAT entries here would be writing bytes the spec
 *          says nobody may read), or contiguity is broken and the run has to
 *          become a real chain.
 *
 * @param[in,out] file File being grown; `no_fat_chain` may be cleared here.
 * @param[in]     next The newly allocated cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    The cluster is reachable from the file's head.
 * @retval k_ra8_err_* FAT write failure.
 *
 * @pre @p file is non-NULL and its mount is an exFAT volume.
 * @pre @p next is marked allocated in the bitmap already.
 * @post On success a reader can reach @p next from `first_cluster`.
 * @post `no_fat_chain` is 0 whenever the allocation is not one contiguous run.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @note The stay-contiguous test is one compound decision of two conditions;
 *       its vectors live with the tests that drive it, cited as
 *       `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_link_cluster`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_link_cluster(ra8_fs_file_t* file, uint32_t next)
{
  if (file->alloc_clusters == 0U) {
    return k_ra8_ok;
  }
  if ((file->no_fat_chain != 0U) && (next == (file->tail_cluster + 1U))) {
    return k_ra8_ok;
  }
  if (file->no_fat_chain != 0U) {
    const ra8_err_t e = priv_exfat_materialize_chain(file, next);
    if (e != k_ra8_ok) {
      return e;
    }
    file->no_fat_chain = 0U;
  } else {
    const ra8_err_t e = priv_fat_set(file->mount, file->tail_cluster, next);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return priv_fat_set(file->mount, next, priv_eoc_write(file->mount));
}

/**
 * @brief Add one cluster to a file's allocation.
 *
 * @details Picks a cluster, marks it in the bitmap FIRST and only then links
 *          it. That order is deliberate: a failure between the two leaks a
 *          cluster, which a later `fsck` reclaims, whereas the reverse order
 *          would leave a file pointing at a cluster the volume still believes
 *          is free -- and the next allocation would hand it to someone else.
 *
 * @param[in,out] file File to grow by one cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok         The file owns one more cluster.
 * @retval k_ra8_err_no_mem The volume is full.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend failure.
 *
 * @pre @p file is non-NULL, in use, and its mount is an exFAT volume.
 * @pre The caller holds the library lock.
 * @post On success `alloc_clusters` is one higher and `tail_cluster` names the
 *       new cluster.
 * @post On success the mount's next-free hint points past the new cluster.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_grow_one(ra8_fs_file_t* file)
{
  ra8_fs_mount_t* m       = file->mount;
  uint32_t        bmp_lba = 0U;
  ra8_err_t       e       = priv_exfat_bitmap_lba(m, &bmp_lba);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t prefer = (file->alloc_clusters > 0U) ? (file->tail_cluster + 1U) : 0U;
  uint32_t       next   = 0U;
  e                     = priv_exfat_pick_cluster(m, bmp_lba, prefer, &next);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_exfat_bitmap_mark(m, bmp_lba, next, 1U);
  if (e != k_ra8_ok) {
    return e;
  }
  priv_alloc_hint_set(m, next + 1U);
  e = priv_exfat_link_cluster(file, next);
  if (e != k_ra8_ok) {
    return e;
  }
  if (file->alloc_clusters == 0U) {
    file->first_cluster      = next;
    file->cur_cluster        = next;
    file->walk_cache_idx     = 0U;
    file->walk_cache_cluster = next;
  }
  file->tail_cluster = next;
  file->alloc_clusters++;
  return k_ra8_ok;
}

/**
 * @brief Grow the file until it owns at least @p need clusters.
 *
 * @details The only caller is the slice resolver, which asks for the cluster
 *          the current offset lands in, so `need` exceeds the allocation by at
 *          most one on a sequential write.
 *
 * @param[in,out] file File to grow.
 * @param[in]     need Clusters the file must own on return.
 *
 * @return Error code.
 * @retval k_ra8_ok         The file owns @p need clusters or more.
 * @retval k_ra8_err_no_mem The volume ran out before reaching @p need.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend failure.
 *
 * @pre @p file is non-NULL and its mount is an exFAT volume.
 * @pre @p need is at least 1.
 * @post On success `alloc_clusters >= need`.
 * @post On failure the clusters already taken stay allocated to @p file.
 *
 * @note Bounded: each iteration raises `alloc_clusters`, which cannot pass
 *       the volume's cluster count without the bitmap scan failing first.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_ensure_clusters(ra8_fs_file_t* file, uint32_t need)
{
  while (file->alloc_clusters < need) {
    const ra8_err_t e = priv_exfat_grow_one(file);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve the cluster at chain index @p idx of an already-grown file.
 *
 * @details A contiguous file is arithmetic. A chained one walks the FAT from
 *          the forward waypoint the read path already maintains, so a
 *          sequential write costs one FAT lookup per cluster rather than one
 *          per cluster examined from the head.
 *
 * @param[in,out] file File to position within.
 * @param[in]     idx  Chain index (0 = the file's first cluster).
 * @param[out]    out  Receives the cluster number at @p idx.
 *
 * @return Error code.
 * @retval k_ra8_ok                The cluster was resolved.
 * @retval k_ra8_err_invalid_state The chain ended before @p idx -- the entry
 *                                 set and the FAT disagree.
 * @retval k_ra8_err_*             FAT read failure.
 *
 * @pre @p file and @p out are non-NULL; `alloc_clusters > idx`.
 * @pre `first_cluster` is a real data cluster.
 * @post On success the forward waypoint sits at @p idx.
 * @post No volume state is modified.
 *
 * @note The waypoint guards are nested rather than joined -- see the comment
 *       on them -- so each is a single condition and MC/DC collapses to
 *       branch coverage.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_cluster_at(ra8_fs_file_t* file, uint32_t idx, uint32_t* out)
{
  if (file->no_fat_chain != 0U) {
    *out = file->first_cluster + idx;
    return k_ra8_ok;
  }
  /* Nested rather than joined, because only the inner guard has both answers
   * available: by the time a chained file is walked its waypoint is always a
   * real cluster, so a joined decision would carry a condition no input can
   * make false -- a permanent MC/DC hole. The inner guard varies with every
   * backward seek. */
  uint32_t from = file->first_cluster;
  uint32_t hops = idx;
  if (file->walk_cache_cluster >= (uint32_t)k_cluster_first_data) {
    if (idx >= file->walk_cache_idx) {
      from = file->walk_cache_cluster;
      hops = idx - file->walk_cache_idx;
    }
  }
  uint32_t cur = from;
  for (uint32_t i = 0U; i < hops; i++) {
    uint32_t        next = 0U;
    const ra8_err_t e    = priv_fat_get(file->mount, cur, &next);
    if (e != k_ra8_ok) {
      return e;
    }
    if (priv_is_eoc(file->mount, next) != 0U) {
      return k_ra8_err_invalid_state;
    }
    cur = next;
  }
  file->walk_cache_idx     = idx;
  file->walk_cache_cluster = cur;
  *out                     = cur;
  return k_ra8_ok;
}

/**
 * @brief Locate byte position @p pos on the volume, allocating to reach it.
 *
 * @details The single place both the data loop and the gap-filling loop turn a
 *          file offset into a sector plus an offset inside it. It grows the
 *          allocation first, so a caller never has to ask whether the cluster
 *          it is about to write exists.
 *
 * @param[in,out] file     Open exFAT handle.
 * @param[in]     pos      Byte position within the file.
 * @param[out]    out_lba  Receives the volume-relative sector holding @p pos.
 * @param[out]    out_off  Receives the byte offset of @p pos inside it.
 * @param[out]    out_room Receives the bytes from @p pos to the sector's end.
 *
 * @return Error code.
 * @retval k_ra8_ok         The position is backed by an allocated cluster.
 * @retval k_ra8_err_no_mem The volume had no cluster to grow into.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend failure.
 *
 * @pre Every pointer is non-NULL and the handle's mount is an exFAT volume.
 * @pre The caller holds the library lock.
 * @post On success `*out_room` is in 1..512 and `*out_off + *out_room == 512`.
 * @post On success `file->cur_cluster` is the cluster holding @p pos.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_slice_at(ra8_fs_file_t* file,
                                     uint32_t       pos,
                                     uint32_t*      out_lba,
                                     uint32_t*      out_off,
                                     uint32_t*      out_room)
{
  const ra8_fs_mount_t* m      = file->mount;
  const uint32_t        cbytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  const uint32_t        idx    = pos / cbytes;
  ra8_err_t             e      = priv_exfat_ensure_clusters(file, idx + 1U);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t cur = 0U;
  e            = priv_exfat_cluster_at(file, idx, &cur);
  if (e != k_ra8_ok) {
    return e;
  }
  file->cur_cluster         = cur;
  const uint32_t in_cluster = pos % cbytes;
  *out_lba  = priv_cluster_to_lba(m, cur) + (in_cluster / k_ra8_fs_bytes_per_sector);
  *out_off  = in_cluster % k_ra8_fs_bytes_per_sector;
  *out_room = (uint32_t)k_ra8_fs_bytes_per_sector - *out_off;
  return k_ra8_ok;
}

/**
 * @brief Fill `[valid_bytes, offset)` with real zeros.
 *
 * @details Only ever has work to do when a handle is positioned past the
 *          written prefix -- an append to a file some other implementation
 *          left with `ValidDataLength < DataLength`. `ValidDataLength` is a
 *          PREFIX length, so the gap cannot be recorded as unwritten once
 *          bytes land beyond it; the only honest answer is to write the zeros
 *          a reader is entitled to see.
 *
 * @param[in,out] file Open exFAT handle in a writing mode.
 *
 * @return Error code.
 * @retval k_ra8_ok         There is no gap left below `file->offset`.
 * @retval k_ra8_err_no_mem The volume had no cluster to grow into.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend failure.
 *
 * @pre @p file is non-NULL and its mount is an exFAT volume.
 * @pre The caller holds the library lock.
 * @post On success `file->valid_bytes >= file->offset`.
 * @post On failure `file->valid_bytes` still describes real zeros on disk.
 *
 * @note Each iteration advances `valid_bytes` by at least one byte, so the
 *       loop is bounded by the gap it was given.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_close_gap(ra8_fs_file_t* file)
{
  /* One sector of zero bytes, the source every hole is filled from. At BLOCK
   * scope because it is used in this function and nowhere else (MISRA C:2012
   * Rule 8.9), and a PLAIN comment because Doxygen cannot attach a symbol to a
   * function-local and mis-files a doc block here as the function's own.
   * `static const` keeps it in .rodata rather than costing a 512-byte stack
   * frame on a call path that already carries a sector buffer of its own. */
  static const uint8_t s_zeros[k_ra8_fs_bytes_per_sector] = {};
  while (file->valid_bytes < file->offset) {
    uint32_t  lba  = 0U;
    uint32_t  off  = 0U;
    uint32_t  room = 0U;
    ra8_err_t e    = priv_exfat_slice_at(file, file->valid_bytes, &lba, &off, &room);
    if (e != k_ra8_ok) {
      return e;
    }
    uint32_t n = file->offset - file->valid_bytes;
    if (n > room) {
      n = room;
    }
    e = priv_write_into_sector(file->mount, lba, off, s_zeros, n);
    if (e != k_ra8_ok) {
      return e;
    }
    file->valid_bytes += n;
  }
  return k_ra8_ok;
}

/* `priv_exfat_write_stream()`: see header for the documented contract. */
ra8_err_t priv_exfat_write_stream(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len)
{
  ra8_err_t e = priv_exfat_close_gap(file);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t consumed = 0U;
  while (consumed < len) {
    uint32_t lba  = 0U;
    uint32_t off  = 0U;
    uint32_t room = 0U;
    e             = priv_exfat_slice_at(file, file->offset, &lba, &off, &room);
    if (e != k_ra8_ok) {
      return e;
    }
    uint32_t put = len - consumed;
    if (put > room) {
      put = room;
    }
    e = priv_write_into_sector(file->mount, lba, off, &buf[consumed], put);
    if (e != k_ra8_ok) {
      return e;
    }
    consumed += put;
    file->offset += put;
    if (file->offset > file->valid_bytes) {
      file->valid_bytes = file->offset;
    }
    if (file->offset > file->size_bytes) {
      file->size_bytes = file->offset;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Flush: the entry set says exactly what the volume holds
 * =============================================================================
 */

/**
 * @brief Patch a Stream-extension entry to describe the handle's current state.
 *
 * @details Writes the four fields a stream changes -- `GeneralSecondaryFlags`,
 *          `FirstCluster`, `ValidDataLength` and `DataLength` -- and zeroes the
 *          high words of the two 64-bit lengths, which matters when the set was
 *          written by an implementation that had a use for them. `NoFatChain`
 *          is asserted only for a file that actually owns clusters: exFAT spec
 *          sec 7.4.4 requires `FirstCluster` 0 on an empty file, and a flag
 *          claiming a contiguous run of nothing is a claim `fsck` checks.
 *
 * @param[in]     file File whose state the entry must describe.
 * @param[in,out] strm The 32-byte Stream-extension entry to patch in place.
 *
 * @return Nothing.
 *
 * @pre @p file and @p strm are non-NULL.
 * @pre @p strm was read back from the volume as part of this file's set.
 * @post `DataLength` equals `file->size_bytes` and its high word is 0.
 * @post `NoFatChain` is set only when the allocation is one contiguous run.
 *
 * @note The caller recomputes the set's SetChecksum after this.
 *
 * @note The two guards are nested rather than joined, because the inner one
 *       is meaningless without the outer: there is no contiguity to record
 *       about a file that owns nothing.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exfat_patch_stream(const ra8_fs_file_t* file, uint8_t* strm)
{
  uint8_t  flags = (uint8_t)k_exfat_secflag_poss;
  uint32_t first = 0U;
  if (file->alloc_clusters > 0U) {
    first = file->first_cluster;
    if (file->no_fat_chain != 0U) {
      flags = (uint8_t)k_exfat_secflag_alloc;
    }
  }
  strm[k_exfat_strm_off_flags] = flags;
  priv_wr32(&strm[k_exfat_strm_off_clus], first);
  priv_wr32(&strm[k_exfat_off_strm_valid], file->valid_bytes);
  priv_wr32(&strm[k_exfat_strm_off_vlen_hi], 0U);
  priv_wr32(&strm[k_exfat_strm_off_dlen], file->size_bytes);
  priv_wr32(&strm[k_exfat_strm_off_dlen_hi], 0U);
}

/**
 * @brief Read a file's entry set back into @p set, recording where it lives.
 *
 * @details Walks `file->entry_set_count` entries from the recorded head with
 *          the ordinary directory cursor, so a set that straddles a cluster
 *          boundary is gathered correctly rather than by flat arithmetic that
 *          would run off the end of the first cluster.
 *
 * @param[in]  file      Open exFAT handle carrying the set's coordinates.
 * @param[out] set       Receives `entry_set_count * 32` bytes.
 * @param[out] at_file   Receives the File entry's position.
 * @param[out] at_stream Receives the Stream entry's position.
 *
 * @return Error code.
 * @retval k_ra8_ok    The set is in @p set and both positions are known.
 * @retval k_ra8_err_* Backend read failure, or the directory ended mid-set.
 *
 * @pre Every pointer is non-NULL and @p set holds `k_exfat_max_set_bytes`.
 * @pre `2 <= file->entry_set_count <= k_exfat_set_writable`.
 * @post On success @p set mirrors the on-disk entries byte for byte.
 * @post No volume state is modified.
 *
 * @note Helper of ::priv_exfat_flush_set (complexity split).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_gather_set(const ra8_fs_file_t* file,
                                       uint8_t*             set,
                                       exfat_setpos_t*      at_file,
                                       exfat_setpos_t*      at_stream)
{
  exfat_cursor_t cur = {.cluster          = file->entry_set_cluster,
                        .entry_in_cluster = file->entry_set_index,
                        .scanned          = 0U};
  for (uint32_t k = 0U; k < file->entry_set_count; k++) {
    const exfat_setpos_t here = {.cluster = cur.cluster, .index = cur.entry_in_cluster};
    const ra8_err_t      e =
      priv_exfat_next_entry(file->mount, &cur, &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (e != k_ra8_ok) {
      return e;
    }
    if (k == 0U) {
      *at_file = here;
    }
    if (k == 1U) {
      *at_stream = here;
    }
  }
  return k_ra8_ok;
}

/* `priv_exfat_flush_set()`: see header for the documented contract. */
ra8_err_t priv_exfat_flush_set(ra8_fs_file_t* file)
{
  const ra8_fs_mount_t* m                          = file->mount;
  uint8_t               set[k_exfat_max_set_bytes] = {};
  exfat_setpos_t        at_file                    = {};
  exfat_setpos_t        at_stream                  = {};
  ra8_err_t             e = priv_exfat_gather_set(file, set, &at_file, &at_stream);
  if (e != k_ra8_ok) {
    return e;
  }
  /* Stamp and patch BEFORE the checksum: it covers every byte both touch, so
   * the other order writes a set a host `fsck` rejects (#601). */
  priv_exfat_file_stamp_write(set);
  priv_exfat_patch_stream(file, &set[k_exfat_entry_bytes]);
  priv_wr16(&set[k_exfat_off_file_csum],
            priv_exfat_set_checksum(set, file->entry_set_count * (uint32_t)k_exfat_entry_bytes));
  e =
    priv_exfat_write_dir_set(m, at_file.cluster, at_file.index, set, (uint32_t)k_exfat_entry_bytes);
  if (e != k_ra8_ok) {
    return e;
  }
  return priv_exfat_write_dir_set(m,
                                  at_stream.cluster,
                                  at_stream.index,
                                  &set[k_exfat_entry_bytes],
                                  (uint32_t)k_exfat_entry_bytes);
}
