/**
 * @file ra8_fs_fat_exfat_stream.c
 * @brief exFAT streaming write: grow the allocation, keep the entry set true.
 *
 * @details
 * The engine behind `ra8_fs_open(write|append)` on an exFAT volume.
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

/* Ported to Zig: the allocation half (RA8FW-1118) and the directory-growth
 * half (RA8FW-1119) live in ra8_fs_exfat_stream_abi.zig; the stream-write
 * half below is the last of this file. */

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
static ra8_err_t internal_exfat_cluster_at(ra8_fs_file_t* file, uint32_t idx, uint32_t* out)
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
 * @param[in]     pos      Byte position within the file (64-bit).
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
 * @post On success `*out_room >= 1` and `*out_off + *out_room` is the sector size.
 * @post On success `file->cur_cluster` is the cluster holding @p pos.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_slice_at(ra8_fs_file_t* file,
                                         uint64_t       pos,
                                         uint64_t*      out_lba,
                                         uint32_t*      out_off,
                                         uint32_t*      out_room)
{
  const ra8_fs_mount_t* m      = file->mount;
  const uint32_t        cbytes = priv_cluster_bytes(m);
  const uint32_t        idx    = (uint32_t)(pos / cbytes);
  ra8_err_t             e      = priv_exfat_ensure_clusters(file, idx + 1U);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t cur = 0U;
  e            = internal_exfat_cluster_at(file, idx, &cur);
  if (e != k_ra8_ok) {
    return e;
  }
  file->cur_cluster         = cur;
  const uint32_t in_cluster = (uint32_t)(pos % cbytes);
  *out_lba                  = priv_cluster_to_lba(m, cur) + (in_cluster / priv_bps(m));
  *out_off                  = in_cluster % priv_bps(m);
  *out_room                 = priv_bps(m) - *out_off;
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
static ra8_err_t internal_exfat_close_gap(ra8_fs_file_t* file)
{
  while (file->valid_bytes < file->offset) {
    uint64_t  lba  = 0U;
    uint32_t  off  = 0U;
    uint32_t  room = 0U;
    ra8_err_t e    = internal_exfat_slice_at(file, file->valid_bytes, &lba, &off, &room);
    if (e != k_ra8_ok) {
      return e;
    }
    const uint64_t gap = file->offset - file->valid_bytes;
    uint32_t       n   = room;
    if (gap < (uint64_t)room) {
      n = (uint32_t)gap;
    }
    e = priv_write_into_sector(file->mount, lba, off, k_zero_sector, n);
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
  ra8_err_t e = internal_exfat_close_gap(file);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t consumed = 0U;
  while (consumed < len) {
    uint64_t lba  = 0U;
    uint32_t off  = 0U;
    uint32_t room = 0U;
    e             = internal_exfat_slice_at(file, file->offset, &lba, &off, &room);
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
 *          `FirstCluster`, `ValidDataLength` and `DataLength` -- as the full
 *          64-bit lengths the format defines, so a file past 4 GiB records its
 *          real size. `NoFatChain`
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
static void internal_exfat_patch_stream(const ra8_fs_file_t* file, uint8_t* strm)
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
  priv_wr64(&strm[k_exfat_off_strm_valid], file->valid_bytes);
  priv_wr64(&strm[k_exfat_strm_off_dlen], file->size_bytes);
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
static ra8_err_t internal_exfat_gather_set(const ra8_fs_file_t* file,
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
  ra8_err_t             e = internal_exfat_gather_set(file, set, &at_file, &at_stream);
  if (e != k_ra8_ok) {
    return e;
  }
  /* Stamp and patch BEFORE the checksum: it covers every byte both touch, so
   * the other order writes a set a host `fsck` rejects. */
  priv_exfat_file_stamp_write(set);
  /* A write also sets the archive attribute: the convention a backup tool
   * clears and the OS re-sets on modification. The low byte of the 16-bit
   * FileAttributes field carries it; the other bits (read-only can never be set
   * on a handle that reached a write) are preserved. */
  set[k_exfat_off_file_attr] =
    (uint8_t)(set[k_exfat_off_file_attr] | (uint8_t)k_exfat_attr_archive);
  internal_exfat_patch_stream(file, &set[k_exfat_entry_bytes]);
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
