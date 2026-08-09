/**
 * @file ra8_fs_check.h
 * @brief Read-only on-device volume consistency check (fsck) for `ra8_fs`.
 * @ingroup grp_storage
 *
 * @details
 * The `fsck.fat -n` / `fsck.exfat -n` a headless product cannot outsource to a
 * PC: ::ra8_fs_check walks a mounted volume, classifies every cluster, follows
 * every directory chain and diffs what is referenced against what is allocated,
 * and reports the findings in a ::ra8_fs_check_report_t. It is the answer to
 * "can I trust this volume before I write to it" -- because writing to an
 * already-inconsistent volume is how partial damage becomes total damage.
 *
 * It is **read-only**. There is no repair path here by design: a repair that
 * runs on a volume already known to be inconsistent, in a firmware with no
 * journal and no undo, is how a recoverable card becomes an empty one. A return
 * of ::k_ra8_ok means the check RAN, not that the volume is clean -- inspect
 * ::ra8_fs_check_report_t::faults_total for that.
 *
 * ## What it checks
 *
 * FAT12 / FAT16 / FAT32:
 *   - every FAT entry value is free, a valid in-range next-cluster, an
 *     end-of-chain marker, or the defective-cluster marker -- anything else is
 *     a bad FAT value;
 *   - directory entries reference a valid first cluster;
 *   - no cluster is reached by two chains (cross-link) and no chain runs into
 *     free space or off the volume (bad chain);
 *   - allocated clusters that no chain reaches are lost;
 *   - on FAT32, the FSInfo free count agrees with the scan when its signatures
 *     validate.
 *
 * exFAT:
 *   - each File entry set's `SetChecksum` recomputes to the stored value;
 *   - each Stream entry's `NameHash` recomputes to the stored value;
 *   - the allocation bitmap agrees, in BOTH directions, with the clusters the
 *     entry sets (plus the bitmap, up-case and directory system runs) actually
 *     reference: a bit set with nothing referencing it is a lost cluster, a
 *     cluster referenced with its bit clear is a bitmap mismatch.
 *
 * ## The caller-supplied bitmap
 *
 * The reference / lost analysis needs one bit per data cluster to record which
 * clusters a walk has visited -- 128 KB for a 32 GB FAT32 card -- which the
 * platform's zero-heap rule (NASA P10 Rule 3) says must be caller-supplied. Pass
 * a @p bitmap of at least `(clusters_total + 7) / 8` bytes and the full check
 * runs. Pass `bitmap == NULL` (or one too small) and the check falls back to the
 * FAT / bitmap CLASSIFICATION pass only: the total / free / used / bad counts are
 * still reported, ::ra8_fs_check_report_t::clusters_lost reads
 * ::k_ra8_fs_check_unknown, and the reference-dependent fields stay zero.
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
 * Sentinels
 * =============================================================================
 */

/**
 * @enum ra8_fs_check_const_t
 * @brief Sentinels the check reports for values it could not determine.
 */
typedef enum : uint32_t {
  k_ra8_fs_check_unknown =
    0xFFFFFFFFU, /**< A count the check could not determine (no bitmap supplied). */
} ra8_fs_check_const_t;

/* =============================================================================
 * Fault categories
 * =============================================================================
 */

/**
 * @enum ra8_fs_check_fault_kind_t
 * @brief The category of a single consistency finding.
 *
 * @details Recorded in ::ra8_fs_check_report_t::first_fault to name what the
 *          first finding was; the per-category counts in the report say how many
 *          of each kind the whole scan found.
 *
 * @see ra8_fs_check_fault_t
 * @see ra8_fs_check_report_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fs_check_fault_none             = 0,  /**< No fault (a clean scan).                       */
  k_ra8_fs_check_fault_bad_fat_value    = 1,  /**< FAT entry is out of range / reserved.          */
  k_ra8_fs_check_fault_crosslink        = 2,  /**< A cluster is reached by more than one chain.   */
  k_ra8_fs_check_fault_lost_cluster     = 3,  /**< A cluster is allocated but referenced by none. */
  k_ra8_fs_check_fault_bad_chain        = 4,  /**< A chain runs into free space or off-volume.    */
  k_ra8_fs_check_fault_bad_dir_entry    = 5,  /**< A directory entry's first cluster is invalid.  */
  k_ra8_fs_check_fault_bad_set_checksum = 6,  /**< exFAT entry-set SetChecksum mismatch.          */
  k_ra8_fs_check_fault_bad_name_hash    = 7,  /**< exFAT Stream NameHash mismatch.                */
  k_ra8_fs_check_fault_bitmap_ref_unset = 8,  /**< exFAT: referenced, but its bitmap bit is 0.    */
  k_ra8_fs_check_fault_free_count_bad   = 9,  /**< FAT32 FSInfo free count disagrees.             */
  k_ra8_fs_check_fault_scan_truncated   = 10, /**< The directory worklist hit its static bound.   */
} ra8_fs_check_fault_kind_t;

/**
 * @struct ra8_fs_check_fault_t
 * @brief Where and what the first consistency finding was.
 *
 * @details A minimal locator, in the shape of what `fsck -n` prints for its
 *          first complaint: the category, the cluster the finding is about, and
 *          the directory sector (volume/partition-relative LBA) plus the byte
 *          offset of the entry within it, when the finding is entry-related.
 *          Fields that do not apply to @p kind are zero.
 *
 * @invariant `kind == k_ra8_fs_check_fault_none` exactly when the scan found
 *            nothing, in which case every other field is zero.
 *
 * @see ra8_fs_check_report_t::first_fault
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_check_fault_kind_t kind;      /**< Category of the first finding.                   */
  uint32_t                  cluster;   /**< Cluster the finding concerns (0 if n/a).         */
  uint32_t                  lba;       /**< Volume-relative sector of the entry (0 if n/a).  */
  uint32_t                  entry_off; /**< Byte offset of the entry in that sector (0 n/a). */
} ra8_fs_check_fault_t;

/* =============================================================================
 * Report
 * =============================================================================
 */

/**
 * @struct ra8_fs_check_report_t
 * @brief The structured result of one ::ra8_fs_check run.
 *
 * @details Counts plus the first fault. ::faults_total is the primary verdict --
 *          zero means the scan found nothing -- and the per-category counts (and
 *          ::first_fault) explain a non-zero total. The reference-dependent
 *          fields (::clusters_lost, ::chains_crosslinked, ::bitmap_mismatches,
 *          ::entries_bad, ::dirs_visited, ::files_visited) are only populated
 *          when ::referenced_scan is true; without a bitmap ::clusters_lost reads
 *          ::k_ra8_fs_check_unknown and the rest stay zero.
 *
 * @invariant `referenced_scan == false` implies `clusters_lost ==
 *            k_ra8_fs_check_unknown`.
 * @invariant `faults_total == 0` implies `first_fault.kind ==
 *            k_ra8_fs_check_fault_none`.
 *
 * @par Example:
 * @code
 * static uint8_t s_bmp[(k_clusters + 7) / 8];
 * ra8_fs_check_report_t rep = {};
 * if (ra8_fs_check(mnt, s_bmp, sizeof(s_bmp), &rep) == k_ra8_ok &&
 *     rep.faults_total == 0U) {
 *   // volume is self-consistent; safe to write
 * }
 * @endcode
 *
 * @see ra8_fs_check()
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_type_t type;            /**< The volume variant that was checked.               */
  bool          referenced_scan; /**< true when a bitmap enabled the reference analysis. */
  uint32_t      clusters_total;  /**< Data-region cluster count.                         */
  uint32_t      clusters_free;   /**< Clusters that read as free.                        */
  uint32_t      clusters_used;   /**< Clusters that read as allocated.                   */
  uint32_t      clusters_bad;    /**< FAT defective-marker clusters (informational).     */
  uint32_t      clusters_lost;   /**< Allocated but referenced by nothing; ::k_ra8_fs_check_unknown
                             when ::referenced_scan is false.                             */
  uint32_t      chains_crosslinked; /**< Clusters reached by more than one chain.             */
  uint32_t      bitmap_mismatches;  /**< exFAT clusters referenced with the bitmap bit clear. */
  uint32_t      entries_bad;        /**< Bad dir entry / SetChecksum / NameHash findings.     */
  uint32_t      dirs_visited;       /**< Directories walked.                                  */
  uint32_t      files_visited;      /**< Files walked.                                        */
  uint32_t      faults_total;       /**< Every finding, across all categories (0 == clean).   */
  ra8_fs_check_fault_t first_fault; /**< Kind + location of the first finding.                */
} ra8_fs_check_report_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Scan a mounted volume for consistency, read-only, and report findings.
 *
 * @details
 * Runs the passes described in this header's file comment for @p handle's
 * filesystem and fills @p report. Nothing on the volume is written, so it is
 * safe on a volume already suspected to be damaged -- that is the whole point.
 *
 * A return of ::k_ra8_ok means the scan COMPLETED, not that the volume is clean:
 * a clean volume and a volume with a thousand lost clusters both return
 * ::k_ra8_ok, and only `report->faults_total` tells them apart. A non-::k_ra8_ok
 * return means the scan could not run to completion -- a null argument, an
 * unmounted handle, or a backend read failure -- and @p report is then only
 * partially filled.
 *
 * Pass @p bitmap of at least `(report->clusters_total + 7) / 8` bytes to enable
 * the reference / lost-cluster / cross-link analysis; pass `bitmap == NULL` (or
 * fewer bytes) to run the classification pass alone (see the file comment). The
 * bitmap is scratch: its contents on entry are ignored and on return undefined.
 *
 * @param[in]  handle       Mount handle from ::ra8_fs_mount().
 * @param[in]  bitmap       Scratch visited-cluster bitmap, or NULL for the
 *                          classification-only pass. Not read on entry.
 * @param[in]  bitmap_bytes Size of @p bitmap in bytes (0 when @p bitmap is NULL).
 * @param[out] report       Receives the structured findings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Scan completed; inspect `report->faults_total`.
 * @retval k_ra8_err_null_ptr      @p handle or @p report is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure while scanning.
 *
 * @pre `handle` and `report` are non-NULL.
 * @pre Mount is in use; no file open on it is mid-write.
 * @post On ::k_ra8_ok `report->faults_total == 0` iff the volume is consistent
 *       within the scope this checker covers.
 * @post No volume state is modified, on any return path.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 * @warning Read-only: this never repairs. Do not treat a non-zero
 *          `faults_total` as "then fix it" -- image the card off-device first.
 *
 * @see ra8_fs_check_report_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_check(ra8_fs_mount_t*        handle,
                                     uint8_t*               bitmap,
                                     uint32_t               bitmap_bytes,
                                     ra8_fs_check_report_t* report);

#ifdef __cplusplus
}
#endif
