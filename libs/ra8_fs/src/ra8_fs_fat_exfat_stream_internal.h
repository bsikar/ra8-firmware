/**
 * @file ra8_fs_fat_exfat_stream_internal.h
 * @brief The exFAT streaming-write mechanism: entry-set positions and its helpers.
 * @ingroup grp_storage
 *
 * @details
 * exFAT streaming write (#602) is one self-contained mechanism with its own
 * vocabulary, so it gets its own themed sub-header rather than being scattered
 * alphabetically across `ra8_fs_fat_protos_a_internal.h` and its `_b` twin --
 * the same reasoning that gave the timestamp and allocator mechanisms theirs.
 * Everything the grow/flush engine needs is declared here, whichever
 * translation unit defines it:
 *
 *   - ::exfat_setpos_t and ::k_exfat_set_max_entries, the coordinates of one
 *     directory ENTRY SET, shared by the mutation helpers and the stream;
 *   - the allocation-bitmap primitives (`priv_exfat_bitmap_*`), which the
 *     grow path drives one cluster at a time;
 *   - ::priv_exfat_find_set and ::priv_exfat_free_clusters, which locate and
 *     release a file's entry set and its clusters;
 *   - ::priv_exfat_link, which lays down a fresh entry set and reports where;
 *   - the stream itself: ::priv_exfat_open_write, ::priv_exfat_write_stream
 *     and ::priv_exfat_flush_set.
 *
 * It is pulled in by the `ra8_fs_fat_internal.h` umbrella and by nothing
 * outside this module.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, March 2021. Section numbers track that document.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_types_internal.h"

/**
 * @enum exfat_set_const_t
 * @brief Bounds on one exFAT directory entry set.
 *
 * @details exFAT spec sec 7.4: a File entry carries a SecondaryCount, and the
 *          set is that many 32-byte entries plus the File entry itself. This
 *          adapter's name cap (::k_exfat_name_cap, 64 UTF-16 units) needs one
 *          Stream entry plus `ceil(64 / 15)` Name entries, so ::k_exfat_set_writable
 *          is the largest set it can rewrite. ::k_exfat_set_max_entries is the
 *          largest the FORMAT allows and bounds the walk over a set some other
 *          implementation wrote.
 *
 * @invariant `k_exfat_set_writable <= k_exfat_set_max_entries`.
 *
 * @par Example:
 * @code
 * exfat_setpos_t pos[k_exfat_set_max_entries] = {};
 * @endcode
 *
 * @see priv_exfat_find_set()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_exfat_set_max_entries = 19U, /**< 1 File + 1 Stream + 17 Name entries.      */
  k_exfat_set_writable    = 7U,  /**< 1 File + 1 Stream + 5 Name (64 chars).    */
  k_exfat_set_min_entries = 3U,  /**< The smallest legal set: File+Stream+Name. */
} exfat_set_const_t;

/**
 * @struct exfat_setpos_t
 * @brief Directory position (cluster + entry index) of one 32-byte entry.
 *
 * @details A directory is a cluster chain, so an entry's address is a cluster
 *          plus an index inside it and NOT a flat offset -- a set can straddle
 *          a cluster boundary and its entries then live in two different
 *          clusters. Everything that rewrites an entry in place carries these
 *          coordinates rather than recomputing them.
 *
 * @invariant `index` is below the directory's entries-per-cluster.
 *
 * @par Example:
 * @code
 * const exfat_setpos_t head = {.cluster = f->entry_set_cluster,
 *                              .index   = f->entry_set_index};
 * @endcode
 *
 * @see priv_exfat_find_set()
 * @since 0.1.0
 */
typedef struct {
  uint32_t cluster; /**< Directory cluster holding the entry.       */
  uint32_t index;   /**< Entry index within that cluster (0-based). */
} exfat_setpos_t;

/**
 * @brief Read one allocation-bitmap bit: is this cluster free?
 *
 * @details The probe the contiguity-preserving allocator runs before anything
 *          else. Keeping a run contiguous is worth one sector read, because a
 *          file that stays contiguous keeps `NoFatChain` and therefore keeps
 *          the read path's O(1) cluster arithmetic.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  bmp_lba  First LBA (volume-relative) of the bitmap.
 * @param[in]  clus     Cluster number to test.
 * @param[out] out_free Receives 1 when the cluster is free, else 0.
 *
 * @return Error code.
 * @retval k_ra8_ok    The bit was read; `*out_free` is 0 or 1.
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p m and @p out_free are non-NULL; `m->type` is exFAT.
 * @pre @p clus is inside `[2, 2 + count_of_clusters)`.
 * @post `*out_free` is 0 or 1 on success.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_bitmap_test(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint8_t* out_free);

/**
 * @brief Find a contiguous free run, starting from the mount's next-free hint.
 *
 * @details Walks the bitmap from the hint and, only if that finds nothing,
 *          repeats from cluster index 0 -- a full rescan rather than a
 *          wrap-around of the remainder, because a free run may STRADDLE the
 *          hint and a window that stopped there would report a full disk the
 *          volume does not have.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  bmp_lba  First LBA (volume-relative) of the bitmap.
 * @param[in]  need     Number of contiguous free clusters required.
 * @param[out] out_clus First cluster of the found run.
 *
 * @return Error code.
 * @retval k_ra8_ok         A run of @p need free clusters was found.
 * @retval k_ra8_err_no_mem No such run anywhere (volume full / too fragmented).
 * @retval k_ra8_err_*      Backend read failure.
 *
 * @pre @p m and @p out_clus are non-NULL; @p need >= 1.
 * @pre The bitmap region is contiguous on disk.
 * @post On success `*out_clus` is the run's first cluster number.
 * @post No volume state is modified.
 *
 * @note Worst case is two passes over the bitmap; typical case is one short one.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_bitmap_scan(const ra8_fs_mount_t* m,
                                 uint32_t              bmp_lba,
                                 uint32_t              need,
                                 uint32_t*             out_clus);

/**
 * @brief Mark a contiguous cluster run as allocated in the bitmap.
 *
 * @details Sets one bit per cluster, batching read-modify-write per bitmap
 *          sector. Per exFAT spec sec 7.1 the bitmap alone is authoritative
 *          for allocation state, so this -- not a FAT edit -- is what makes a
 *          cluster owned.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark used.
 *
 * @return Error code.
 * @retval k_ra8_ok    All bits set + written.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre @p count >= 1.
 * @post The @p count bits for the run read as 1.
 * @post Only the affected bitmap sectors are rewritten.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_bitmap_mark(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count);

/**
 * @brief Locate a file's full directory-entry set with per-entry positions.
 *
 * @details Walks the root directory like ::priv_exfat_find but records the
 *          (cluster, index) of every entry in the matched set -- the File entry
 *          plus all its secondaries -- so callers can rewrite them in place.
 *          Non-matching sets are skipped entry-by-entry, which keeps the cursor
 *          aligned.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      Target name (ASCII, root-level); leading '/' stripped.
 * @param[out] pos       Receives the positions (File entry first).
 * @param[in]  max_pos   Capacity of @p pos.
 * @param[out] out_count Receives the entry count (1 + SecondaryCount).
 * @param[out] file_copy Receives the 32-byte File entry.
 * @param[out] strm_copy Receives the 32-byte Stream-extension entry.
 *
 * @return Error code.
 * @retval k_ra8_ok            Set found; outputs populated.
 * @retval k_ra8_err_not_found No matching set in the root directory.
 * @retval k_ra8_err_no_mem    The set has more entries than @p max_pos.
 * @retval k_ra8_err_*         Backend read failure.
 *
 * @pre All pointers are non-NULL; `m->type` is exFAT.
 * @pre @p path is a flat root-level name.
 * @post On success @p pos holds `*out_count` valid positions.
 * @post On failure the outputs are unspecified.
 *
 * @note Only the root directory is searched (flat namespace).
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_find_set(const ra8_fs_mount_t* m,
                              const char*           path,
                              exfat_setpos_t*       pos,
                              uint32_t              max_pos,
                              uint32_t*             out_count,
                              uint8_t*              file_copy,
                              uint8_t*              strm_copy);

/**
 * @brief Free the cluster run / chain referenced by a Stream entry.
 *
 * @details Contiguous (`NoFatChain`) files free `ceil(DataLength / cluster
 *          bytes)` bitmap bits from the first cluster; FAT-chained files walk
 *          the FAT and free each visited cluster. Per exFAT spec sec 7.1 the
 *          bitmap alone is authoritative, so the FAT entries themselves are
 *          left untouched.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] strm The file's 32-byte Stream-extension entry.
 *
 * @return Error code.
 * @retval k_ra8_ok    Clusters freed (or the file had none).
 * @retval k_ra8_err_* Bitmap or backend failure.
 *
 * @pre @p m and @p strm are non-NULL.
 * @pre The caller owns the file: either its entries are already deleted, or it
 *      is truncating through an open handle.
 * @post The file's clusters read as free in the bitmap.
 * @post FAT contents are unchanged.
 *
 * @note Chain walk is bounded by the volume's cluster count.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_free_clusters(const ra8_fs_mount_t* m, const uint8_t* strm);

/**
 * @brief Lay down a fresh File/Stream/Name entry set and report where it went.
 *
 * @details Finds a run of free directory slots, builds the set (typed entries,
 *          name hash, creation stamps, SetChecksum) and writes it. The caller
 *          gets the head position and the entry count back so a streaming
 *          writer can keep rewriting the same set as the file grows, instead of
 *          searching the directory again on every flush.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      File name (ASCII, no leading '/').
 * @param[in]  nlen      Name length in characters (1..::k_exfat_name_cap).
 * @param[out] out_head  Receives the File entry's (cluster, index).
 * @param[out] out_count Receives the entry count (1 + SecondaryCount).
 *
 * @return Error code.
 * @retval k_ra8_ok         Entry set written; outputs populated.
 * @retval k_ra8_err_no_mem No run of free directory slots big enough.
 * @retval k_ra8_err_*      Backend read/write failure.
 *
 * @pre @p m, @p path, @p out_head and @p out_count are non-NULL.
 * @pre `priv_strlen(path) == nlen` and @p nlen >= 1.
 * @post On success the root directory holds a zero-length entry set for @p path.
 * @post On success `*out_head` addresses that set's File entry.
 *
 * @note The set is kept within one directory cluster.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_link(ra8_fs_mount_t* m,
                          const char*     path,
                          uint32_t        nlen,
                          exfat_setpos_t* out_head,
                          uint32_t*       out_count);

/**
 * @brief Open an exFAT file for writing: create, truncate, or position to append.
 *
 * @details The exFAT half of ::ra8_fs_open for the two writing modes. It
 *          locates the name's entry set, refuses a directory (a truncate would
 *          free the clusters holding its contents), and then either truncates
 *          the file in place -- keeping its entry, and therefore its creation
 *          stamp -- or seeks to `DataLength` for an append. A name that does
 *          not exist is created as a zero-length entry set, so the handle is
 *          usable before a single byte has been written.
 *
 * @param[in,out] handle   Mounted exFAT volume.
 * @param[in]     path     Flat root-level name (ASCII); leading '/' stripped.
 * @param[in]     mode     ::k_ra8_fs_mode_write or ::k_ra8_fs_mode_append.
 * @param[out]    out_file Receives the open handle.
 *
 * @return Error code.
 * @retval k_ra8_ok                File open and positioned for @p mode.
 * @retval k_ra8_err_invalid_arg   Empty or over-long name, or @p path is a
 *                                 directory.
 * @retval k_ra8_err_no_mem        File table full, or no free directory slot.
 * @retval k_ra8_err_not_supported The existing entry set is larger than this
 *                                 adapter can rewrite.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre @p handle, @p path and @p out_file are non-NULL; the mount is in use.
 * @pre @p mode is a writing mode; read opens never reach here.
 * @post On success `*out_file` is in use with its entry-set coordinates set.
 * @post On failure no file slot is left marked in use.
 *
 * @note Not thread-safe; the public entry point holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_open_write(ra8_fs_mount_t* handle,
                                const char*     path,
                                ra8_fs_mode_t   mode,
                                ra8_fs_file_t** out_file);

/**
 * @brief Stream bytes into an exFAT file, growing its allocation as needed.
 *
 * @details The engine behind ::ra8_fs_write on an exFAT mount. It closes any
 *          gap between `ValidDataLength` and the write offset with real zeros
 *          first (the format expresses "not yet written" only as a PREFIX
 *          length, so a hole in the middle cannot be recorded and must not be
 *          left), then writes sector slice by sector slice, taking one more
 *          cluster from the allocation bitmap whenever the offset crosses into
 *          a cluster the file does not own yet.
 *
 * @param[in,out] file Open exFAT handle in a writing mode.
 * @param[in]     buf  Source bytes.
 * @param[in]     len  Number of bytes to write (> 0).
 *
 * @return Error code.
 * @retval k_ra8_ok         All @p len bytes are on the volume.
 * @retval k_ra8_err_no_mem The volume has no free cluster left.
 * @retval k_ra8_err_*      Bitmap, FAT, or backend failure.
 *
 * @pre @p file and @p buf are non-NULL; the handle is in use and writable.
 * @pre `file->mount->type` is exFAT.
 * @post On success `file->offset` advanced by @p len and `valid_bytes` covers it.
 * @post On failure some bytes may already be on the volume; the entry set is
 *       not updated here, so the file's recorded length never overstates it.
 *
 * @note Does NOT touch the directory -- ::priv_exfat_flush_set does that.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_write_stream(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len);

/**
 * @brief Rewrite a file's entry set from the handle, checksum last.
 *
 * @details Reads the set back through a cursor from the recorded head, patches
 *          the File entry's modification stamps and the Stream entry's flags,
 *          `FirstCluster`, `ValidDataLength` and `DataLength`, accumulates the
 *          SetChecksum over the PATCHED bytes, and only then writes the two
 *          entries down. Order matters: the checksum covers every byte the
 *          patch touched, so computing it before the patch produces a set a
 *          host `fsck` rejects.
 *
 * @param[in,out] file Open exFAT handle whose entry set is to be committed.
 *
 * @return Error code.
 * @retval k_ra8_ok    The on-disk set describes the handle exactly.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p file is non-NULL, in use, and its mount is an exFAT volume.
 * @pre `file->entry_set_cluster` / `_index` / `_count` came from a successful
 *      ::priv_exfat_find_set or ::priv_exfat_link.
 * @post On success the Stream entry's `DataLength` equals `file->size_bytes`.
 * @post On success the File entry's SetChecksum covers the patched bytes.
 *
 * @note Called after every write and again at close, so a writer cut off
 *       mid-stream still leaves a set describing what is really on the card.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_flush_set(ra8_fs_file_t* file);
