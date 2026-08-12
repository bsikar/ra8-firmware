/**
 * @file ra8_fs_meta.h
 * @brief Volume-level metadata: free space, volume label, and per-entry `utime`.
 * @ingroup grp_storage
 *
 * @details
 * The three metadata operations a general-purpose filesystem is expected to
 * carry that the core `ra8_fs.h` surface (format / mount / open / read / write /
 * stat / listdir / unlink / rename / mkdir / rmdir) does not:
 *
 *   - ::ra8_fs_free_space -- how much room is left, for a data logger that must
 *     know before a run whether to start, roll or stop, and for any UI that
 *     shows a free-space indicator. The counts already live inside the driver
 *     (FAT32's FSInfo free count, and the exFAT allocation bitmap); this exposes
 *     them, falling back to a full FAT / bitmap walk when no trusted count
 *     exists.
 *   - ::ra8_fs_get_label / ::ra8_fs_set_label -- read and change the human name
 *     of the medium after format, the "which card is this" a product shows and a
 *     host lets you rename.
 *   - ::ra8_fs_utime -- put a chosen create / modify / access time on a named
 *     entry, so a backup or sync restore can preserve original timestamps
 *     instead of stamping every restored file with the moment of the restore.
 *
 * They live in their own header, in the shape of ::ra8_fs_stat and the rest of
 * `ra8_fs.h`, because they are an optional metadata extension rather than the
 * read/write core: a consumer that only moves bytes never includes this file
 * and pays nothing for its existence. This header pulls in `ra8_fs.h` for the
 * mount handle and (through it) the ::ra8_fs_datetime_t the `utime` reads.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"

/* =============================================================================
 * Free space (statvfs)
 * =============================================================================
 */

/**
 * @enum ra8_fs_label_limit_t
 * @brief Sizing constant for the volume-label API.
 */
typedef enum : uint8_t {
  k_ra8_fs_label_cap = 12, /**< 11-char FAT/exFAT label + NUL: min `out` size. */
} ra8_fs_label_limit_t;

/**
 * @struct ra8_fs_space_t
 * @brief A mounted volume's capacity, free space, and cluster geometry.
 *
 * @details What ::ra8_fs_free_space reports. Byte counts are 64-bit because a
 *          2 TiB volume's byte total overflows 32 bits even though FAT file
 *          sizes stay 32-bit; the cluster counts and the per-cluster byte size
 *          are 32-bit, matching the on-disk fields they come from.
 *
 * @invariant `used_clusters + free_clusters == total_clusters`.
 * @invariant `total_bytes == (uint64_t)total_clusters * bytes_per_cluster` (and
 *            likewise for `free_bytes` / `used_bytes`).
 * @invariant `bytes_per_cluster == sectors_per_cluster * bytes_per_sector`.
 *
 * @par Example:
 * @code
 * ra8_fs_space_t sp = {};
 * if (ra8_fs_free_space(mnt, &sp) == k_ra8_ok && sp.free_bytes < need) {
 *   // not enough room -- roll the log or refuse the run
 * }
 * @endcode
 *
 * @see ra8_fs_free_space()
 * @since 0.1.0
 */
typedef struct {
  uint64_t total_bytes;       /**< Whole data region, in bytes.       */
  uint64_t free_bytes;        /**< Unallocated data region, in bytes. */
  uint64_t used_bytes;        /**< Allocated data region, in bytes.   */
  uint32_t bytes_per_cluster; /**< Allocation-unit size in bytes.     */
  uint32_t total_clusters;    /**< Data-region cluster count.         */
  uint32_t free_clusters;     /**< Clusters not currently allocated.  */
  uint32_t used_clusters;     /**< Clusters currently allocated.      */
} ra8_fs_space_t;

/**
 * @brief Report a mounted volume's total, free, and used space.
 *
 * @details Answers "how much room is left" from the counts the driver already
 *          keeps. On FAT32 whose FSInfo validated at mount the free count is the
 *          cached one (O(1)); otherwise -- FAT12/FAT16, or a FAT32 whose FSInfo
 *          was absent or untrusted -- the FAT is walked once and the result is
 *          cached for subsequent queries. On exFAT the allocation bitmap (which
 *          alone is authoritative for allocation state) is population-counted,
 *          also cached. The byte totals are the cluster counts scaled by the
 *          allocation-unit size, so a caller can compare bytes directly without
 *          knowing the cluster geometry.
 *
 * @param[in]  handle Mount handle from ::ra8_fs_mount().
 * @param[out] out    Receives the capacity / free / used figures on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Figures reported in @p out.
 * @retval k_ra8_err_null_ptr      @p handle or @p out is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure while walking the FAT or
 *                                 the allocation bitmap.
 *
 * @pre `handle` and `out` are non-NULL.
 * @pre Mount is in use.
 * @post On ::k_ra8_ok `out->used_clusters + out->free_clusters ==
 *       out->total_clusters` and the byte totals equal the cluster totals
 *       scaled by `out->bytes_per_cluster`.
 * @post No volume state is modified.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_space_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_free_space(ra8_fs_mount_t* handle, ra8_fs_space_t* out);

/* =============================================================================
 * Volume label
 * =============================================================================
 */

/**
 * @brief Read the volume label of a mounted volume.
 *
 * @details On FAT the label is read from the root directory's `ATTR_VOLUME_ID`
 *          entry when one exists (the copy a desktop shows and edits), falling
 *          back to the boot sector's `BS_VolLab`; the specification's unlabelled
 *          sentinel `"NO NAME    "` reports as the empty string. On exFAT the
 *          root-directory Volume Label entry (type 0x83) is decoded from
 *          UTF-16LE. The result is NUL-terminated and stripped of trailing
 *          padding spaces.
 *
 * @param[in]  handle  Mount handle.
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes; ::k_ra8_fs_label_cap holds
 *                     any label this filesystem can store.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Label written to @p out (possibly empty).
 * @retval k_ra8_err_null_ptr      @p handle or @p out is NULL.
 * @retval k_ra8_err_invalid_arg   @p out_len is 0.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure.
 *
 * @pre `handle` and `out` are non-NULL; `out_len >= 1`.
 * @pre Mount is in use.
 * @post On ::k_ra8_ok @p out is NUL-terminated (truncated to fit @p out_len).
 * @post No volume state is modified.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_set_label()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_get_label(ra8_fs_mount_t* handle, char* out, uint32_t out_len);

/**
 * @brief Set (or clear) the volume label of a mounted volume.
 *
 * @details On FAT the boot sector's `BS_VolLab` and the root directory's
 *          `ATTR_VOLUME_ID` entry are kept in step: a non-empty label writes
 *          both (creating the root entry if absent), an empty label restores the
 *          `"NO NAME    "` sentinel and removes the root entry, so `fsck.fat`
 *          reports neither a blank nor a mismatched label. On exFAT the root
 *          Volume Label entry (type 0x83) is rewritten in place with the new
 *          UTF-16LE label and character count. Data and every other entry are
 *          untouched.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     label  New label (<= 11 characters), or NULL / "" to clear it.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Label written.
 * @retval k_ra8_err_null_ptr      @p handle is NULL.
 * @retval k_ra8_err_invalid_arg   @p label is longer than 11 characters.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_no_mem        The root directory has no free slot for a new
 *                                 label entry.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre `handle` is non-NULL and the mount is in use.
 * @pre No host label longer than 11 characters is requested.
 * @post On ::k_ra8_ok a later ::ra8_fs_get_label reports @p label (empty when
 *       @p label was NULL/"").
 * @post On an argument error the volume is unchanged.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_get_label()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_set_label(ra8_fs_mount_t* handle, const char* label);

/* =============================================================================
 * Per-entry timestamps (utime)
 * =============================================================================
 */

/**
 * @brief Set a named entry's create / modify / access timestamps.
 *
 * @details The `touch` / `utime` primitive: each non-NULL reading overwrites
 *          that entry's corresponding timestamp fields, each NULL one leaves
 *          them unchanged. Unlike the close-time and write-time stamps -- which
 *          come from the injected clock (see ::ra8_fs_set_clock()) -- these are
 *          caller-chosen, so a backup or sync restore can put a file's ORIGINAL
 *          create / modify time back instead of the moment of the restore, which
 *          is what every "newest wins" incremental heuristic keys on.
 *
 *          FAT stores a create date+time, a modify date+time and an access DATE
 *          only (there is no access time); exFAT stores all three as full
 *          timestamps plus the 10 ms increments and UtcOffset bytes, and its
 *          entry-set SetChecksum is recomputed after the patch. Each reading is
 *          clamped into the range the on-disk format can express, so an
 *          out-of-range field is recorded as the nearest legal value rather than
 *          failing the call. The entry may be a file or a directory; the volume
 *          root has no entry to stamp and is rejected.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   Path to the entry (resolved as ::ra8_fs_stat() resolves).
 * @param[in]     create Create stamp to write, or NULL to leave it unchanged.
 * @param[in]     modify Modify stamp to write, or NULL to leave it unchanged.
 * @param[in]     access Access stamp to write, or NULL to leave it unchanged.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Requested stamps written.
 * @retval k_ra8_err_null_ptr      @p handle or @p path is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   @p path names the volume root, or is not a
 *                                 valid name for this filesystem.
 * @retval k_ra8_err_not_found     Nothing at @p path.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre `handle` and `path` are non-NULL; the mount is in use.
 * @pre No open handle is mid-write on @p path (its close would re-stamp it).
 * @post On ::k_ra8_ok each non-NULL reading's fields hold that (clamped) instant.
 * @post A NULL reading's fields, and any non-time byte, are unchanged.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_set_clock()  Installs the clock the automatic stamps use.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_utime(ra8_fs_mount_t*          handle,
                                     const char*              path,
                                     const ra8_fs_datetime_t* create,
                                     const ra8_fs_datetime_t* modify,
                                     const ra8_fs_datetime_t* access);

/**
 * @brief Set an open file's length to @p new_size, shrinking or growing it.
 *
 * @details The `ftruncate()` verb in both directions (#680), for a handle open
 * in a writing mode. It is the only way to give a file an arbitrary length:
 * writing extends only where bytes land, and ::ra8_fs_seek clamps to the current
 * size, so neither can pre-size a file or trim it to N > 0 bytes. A shrink frees
 * the tail clusters and lowers the length; a grow extends the file and the gap
 * `[old_size, new_size)` reads back as zero -- FAT zero-fills the fresh clusters
 * on disk, exFAT raises `DataLength` while `ValidDataLength` stays at the written
 * prefix so the format serves the gap as zero (and converts a run that outgrows
 * its contiguous space to a real FAT chain). The offset is left where it was,
 * pulled down only by a shrink that lands below it. Lengths are 64-bit: an
 * exFAT file truncates to any size the volume can hold, past 4 GiB included
 * (#676). On FAT12/16/32 a @p new_size above ::k_ra8_fs_fat_max_file_bytes is
 * refused with ::k_ra8_err_invalid_size -- `DIR_FileSize` is 32-bit, so the
 * format itself cannot express it.
 *
 * @param[in,out] file     Open handle in ::k_ra8_fs_mode_write or _append.
 * @param[in]     new_size Desired length in bytes.
 *
 * @retval k_ra8_ok                Length set; the entry / directory reflects it.
 * @retval k_ra8_err_null_ptr      @p file is NULL.
 * @retval k_ra8_err_invalid_state Not open, or opened read-only.
 * @retval k_ra8_err_invalid_size  FAT volume and @p new_size exceeds 4 GiB - 1.
 * @retval k_ra8_err_no_mem        A grow ran out of free clusters.
 * @retval k_ra8_err_*             Backend, FAT, or bitmap failure.
 *
 * @pre @p file is a handle from ::ra8_fs_open() in write or append mode.
 * @post On ::k_ra8_ok ::ra8_fs_size() reports @p new_size and a re-read of the
 *       gap returns zeros.
 * @post On ::k_ra8_ok the offset is `min(old_offset, new_size)`.
 *
 * @note Not thread-safe per file; the public entry holds the library lock.
 *
 * @see ra8_fs_write()  Extends a file only where bytes are actually written.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_truncate(ra8_fs_file_t* file, uint64_t new_size);

/* =============================================================================
 * Per-entry attributes (chmod-style)
 * =============================================================================
 */

/**
 * @enum ra8_fs_attr_settable_t
 * @brief The attribute bits ::ra8_fs_set_attr may change.
 *
 * @details The FAT/exFAT attribute byte also carries the DIRECTORY, VOLUME_ID
 *          and long-name bits, which describe what an entry IS rather than how a
 *          host wants it treated; changing them would reclassify the entry and
 *          corrupt the volume. So the set/clear masks are confined to the four
 *          host-controlled bits -- read-only, hidden, system and archive -- and
 *          a request naming any other bit is rejected.
 *
 * @see ra8_fs_set_attr()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fs_attr_settable =
    (uint8_t)((uint8_t)k_ra8_fs_attr_read_only | (uint8_t)k_ra8_fs_attr_hidden |
              (uint8_t)k_ra8_fs_attr_system | (uint8_t)k_ra8_fs_attr_archive),
  /**< Union of the four settable bits (0x27). */
} ra8_fs_attr_settable_t;

/**
 * @brief Set and/or clear a named entry's file attributes (chmod-style).
 *
 * @details Patches the entry's on-disk attribute byte to
 *          `(attr & ~clear_mask) | set_mask`: bits in @p clear_mask are cleared,
 *          bits in @p set_mask are set, every other bit is left as it was. Only
 *          the four host-controlled bits are settable
 *          (::k_ra8_fs_attr_settable) -- read-only, hidden, system, archive; a
 *          mask naming DIRECTORY, VOLUME_ID or a long-name bit, or a bit that
 *          appears in BOTH masks, is rejected without touching the volume, so a
 *          caller cannot reclassify a directory as a file or give one
 *          contradictory instructions.
 *
 *          This is how a read-only marker is PUT ON a file (so a later write /
 *          unlink / rename is refused with ::k_ra8_err_access_denied) or taken
 *          OFF one, and how hidden / system / archive are managed to match host
 *          behaviour. On FAT the byte lives in the directory entry; on exFAT it
 *          is the low byte of the File entry's FileAttributes, and the entry
 *          set's SetChecksum is recomputed after the patch. The volume root has
 *          no entry of its own and is rejected.
 *
 * @param[in,out] handle     Mount handle.
 * @param[in]     path       Path to the entry (resolved as ::ra8_fs_stat() does).
 * @param[in]     set_mask   Attribute bits to set (subset of
 *                           ::k_ra8_fs_attr_settable).
 * @param[in]     clear_mask Attribute bits to clear (subset of
 *                           ::k_ra8_fs_attr_settable).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Attribute byte patched (or both masks 0: a
 *                                 no-op success).
 * @retval k_ra8_err_null_ptr      @p handle or @p path is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   @p path names the volume root or is not a
 *                                 valid name; or a mask names a non-settable bit
 *                                 or a bit in both masks.
 * @retval k_ra8_err_not_found     Nothing at @p path.
 * @retval k_ra8_err_*             Backend read/write failure.
 *
 * @pre `handle` and `path` are non-NULL; the mount is in use.
 * @pre `set_mask & ~k_ra8_fs_attr_settable == 0` and likewise for @p clear_mask.
 * @post On ::k_ra8_ok a later ::ra8_fs_stat reports `(old & ~clear_mask) |
 *       set_mask` for the settable bits, and no other bit changed.
 * @post On any error the entry's attribute byte is unchanged.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_stat()  Reports the attribute byte this patches.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_set_attr(ra8_fs_mount_t* handle, const char* path, uint8_t set_mask, uint8_t clear_mask);

#ifdef __cplusplus
}
#endif
