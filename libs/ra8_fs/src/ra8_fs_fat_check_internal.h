/**
 * @file ra8_fs_fat_check_internal.h
 * @brief Cross-TU state and helpers for the volume consistency check (fsck).
 * @ingroup grp_storage
 *
 * @details
 * ::ra8_fs_check is split across two translation units to stay inside the
 * 1000-line source cap, along the same FAT / exFAT seam the rest of the adapter
 * uses:
 *
 * - `ra8_fs_fat_check.c`       -- the shared scan context, the finding recorder,
 *                                 the visited-bitmap primitives, the FAT12/16/32
 *                                 check, and the public ::ra8_fs_check entry.
 * - `ra8_fs_fat_exfat_check.c` -- the exFAT check (entry-set checksums, name
 *                                 hashes, and the allocation-bitmap diff).
 *
 * This header carries only what those two files SHARE: the ::ra8_fs_check_ctx_t
 * that threads the mount, the report and the scratch bitmap through the walk,
 * and the recorder / bitmap primitives both use. It is included directly by
 * those two translation units and by nothing else -- it is not part of the
 * `ra8_fs_fat_internal.h` umbrella, because no other unit needs the check's
 * private vocabulary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_check.h"

/**
 * @enum ra8_fs_check_bound_t
 * @brief Static bounds that keep a scan of a corrupt volume terminating.
 *
 * @details A corrupt volume must not loop forever (NASA P10 Rule 2). Every
 *          directory-tree walk pushes subdirectories onto a fixed worklist whose
 *          depth is capped here; overflowing it is reported as
 *          ::k_ra8_fs_check_fault_scan_truncated rather than growing unbounded.
 *          The per-chain cluster walks are bounded separately by the volume's own
 *          `count_of_clusters`, and terminated early by the visited bitmap (a
 *          revisit is a cross-link, and it stops the walk).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_fs_check_max_dirs = 128U, /**< Directory worklist depth cap. */
} ra8_fs_check_bound_t;

/**
 * @struct ra8_fs_check_ctx_t
 * @brief The state one ::ra8_fs_check run threads through its passes.
 *
 * @details Bundles the four things every helper needs -- the mounted volume, the
 *          report being filled, the caller's scratch visited-bitmap and its valid
 *          bit count -- so the walk functions take one pointer instead of four.
 *          When @p bitmap is NULL the reference / lost analysis is skipped and
 *          the check runs its classification pass alone.
 *
 * @invariant `bitmap == nullptr` exactly when the caller supplied no (or a
 *            too-small) bitmap, and then the reference passes do not run.
 * @invariant `bitmap_bits == rep->clusters_total` whenever `bitmap != nullptr`.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_mount_t*        m;           /**< The mounted volume under check.          */
  ra8_fs_check_report_t* rep;         /**< The report being filled.                 */
  uint8_t*               bitmap;      /**< Scratch visited-cluster bitmap, or NULL. */
  uint32_t               bitmap_bits; /**< Valid bit count (== clusters_total).     */
} ra8_fs_check_ctx_t;

/**
 * @brief Record one consistency finding into the report.
 *
 * @details Increments `rep->faults_total` and, on the FIRST finding, stamps
 *          `rep->first_fault` with @p kind and the locator. Later findings raise
 *          the total but leave the first fault untouched, so the report always
 *          names the earliest thing the scan tripped on -- what `fsck -n` prints
 *          first. The per-category counters are the callers' responsibility; this
 *          owns only the total and the first-fault stamp.
 *
 * @param[in,out] ctx       The scan context.
 * @param[in]     kind      The finding's category.
 * @param[in]     cluster   Cluster the finding concerns (0 if n/a).
 * @param[in]     lba       Volume-relative sector of the entry (0 if n/a).
 * @param[in]     entry_off Byte offset of the entry in that sector (0 if n/a).
 *
 * @return Nothing.
 *
 * @pre @p ctx and `ctx->rep` are non-NULL.
 * @pre @p kind is not ::k_ra8_fs_check_fault_none.
 * @post `rep->faults_total` is one higher.
 * @post `rep->first_fault` names the earliest finding.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_check_fault(ra8_fs_check_ctx_t*       ctx,
                      ra8_fs_check_fault_kind_t kind,
                      uint32_t                  cluster,
                      uint32_t                  lba,
                      uint32_t                  entry_off);

/**
 * @brief True when @p cluster is a real data cluster of the volume under check.
 *
 * @details The addressable range is `[2, 2 + clusters_total)`; anything else --
 *          the reserved 0 and 1, or a value past the last cluster -- is not a
 *          cluster this volume has.
 *
 * @param[in] ctx     The scan context.
 * @param[in] cluster The cluster number to test.
 *
 * @return Whether @p cluster addresses a data cluster.
 * @retval true  `2 <= cluster < 2 + clusters_total`.
 * @retval false Out of range.
 *
 * @pre @p ctx is non-NULL.
 * @pre The report's `clusters_total` is populated.
 * @post No state modified.
 * @post Result depends only on the inputs.
 *
 * @note Pure; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_check_in_range(const ra8_fs_check_ctx_t* ctx, uint32_t cluster);

/**
 * @brief Mark @p cluster visited in the scratch bitmap; report a prior visit.
 *
 * @details Test-and-set of the bit for @p cluster. A bit that was ALREADY set
 *          means a second chain reached this cluster -- a cross-link, or a loop
 *          -- so the caller stops the walk it is on. The bit index is
 *          `cluster - 2`, matching the exFAT allocation bitmap's own layout so
 *          the two can be diffed directly.
 *
 * @param[in,out] ctx     The scan context (its bitmap is written).
 * @param[in]     cluster The cluster to mark; must be in range.
 *
 * @return Whether the cluster had already been visited.
 * @retval true  The bit was already set (cross-link / loop).
 * @retval false The bit was clear and is now set (first visit).
 *
 * @pre @p ctx and `ctx->bitmap` are non-NULL.
 * @pre ::priv_check_in_range holds for @p cluster.
 * @post The bit for @p cluster reads as 1.
 * @post No other bit is changed.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_check_mark(ra8_fs_check_ctx_t* ctx, uint32_t cluster);

/**
 * @brief Visit a cluster during a walk: mark it, and report a range or link fault.
 *
 * @details The one place the walkers share -- FAT chains, exFAT runs, and the
 *          directory allocations of both -- so the range and cross-link handling
 *          lives once. It marks @p cluster and returns whether the walk must stop:
 *          a cluster out of range records @p oor_kind (bumping `entries_bad` when
 *          that kind is ::k_ra8_fs_check_fault_bad_dir_entry), and a cluster
 *          already visited records a cross-link.
 *
 * @param[in,out] ctx      The scan context (its bitmap and report are written).
 * @param[in]     cluster  The cluster to visit.
 * @param[in]     oor_kind The fault kind to record when @p cluster is out of range.
 *
 * @return Whether the caller must stop walking.
 * @retval true  A fault was recorded (out of range, or a cross-link).
 * @retval false @p cluster was in range and is now freshly marked.
 *
 * @pre @p ctx and its bitmap are non-NULL.
 * @pre @p oor_kind is a range-style fault kind.
 * @post On false the bit for @p cluster reads as 1.
 * @post On true the report carries the finding.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_check_visit(ra8_fs_check_ctx_t*       ctx,
                      uint32_t                  cluster,
                      ra8_fs_check_fault_kind_t oor_kind);

/**
 * @brief Zero the scratch visited-bitmap over its valid bit range.
 *
 * @details Clears `(bitmap_bits + 7) / 8` bytes so a stale caller buffer cannot
 *          make a fresh scan see clusters as already visited. Called once, before
 *          the reference passes.
 *
 * @param[in,out] ctx The scan context (its bitmap is cleared).
 *
 * @return Nothing.
 *
 * @pre @p ctx and `ctx->bitmap` are non-NULL.
 * @pre `ctx->bitmap` is at least `(bitmap_bits + 7) / 8` bytes.
 * @post Every valid bit reads as 0.
 * @post No state outside the bitmap is touched.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_check_zero_bitmap(ra8_fs_check_ctx_t* ctx);

/**
 * @brief Run the FAT12/16/32 consistency check into the context's report.
 *
 * @details The FAT-side dispatch of ::ra8_fs_check: classifies the FAT, walks the
 *          directory tree marking referenced clusters, diffs for lost clusters,
 *          and compares the FAT32 FSInfo free count. Defined in
 *          `ra8_fs_fat_check.c`.
 *
 * @param[in,out] ctx The scan context (`ctx->m->type` is a FAT variant).
 *
 * @return Error code.
 * @retval k_ra8_ok    The scan completed; findings are in the report.
 * @retval k_ra8_err_* Backend read failure mid-scan.
 *
 * @pre @p ctx is non-NULL; `ctx->m->type` is FAT12/16/32.
 * @pre The mount is in use and its geometry is populated.
 * @post On ::k_ra8_ok the report's counts and faults describe the volume.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_check_fat(ra8_fs_check_ctx_t* ctx);

/**
 * @brief Run the exFAT consistency check into the context's report.
 *
 * @details The exFAT-side dispatch of ::ra8_fs_check: verifies each entry set's
 *          SetChecksum and NameHash, marks every referenced cluster (files,
 *          directories and the bitmap / up-case system runs), and diffs that
 *          against the allocation bitmap in both directions. Defined in
 *          `ra8_fs_fat_exfat_check.c`.
 *
 * @param[in,out] ctx The scan context (`ctx->m->type` is exFAT).
 *
 * @return Error code.
 * @retval k_ra8_ok    The scan completed; findings are in the report.
 * @retval k_ra8_err_* Backend read failure mid-scan.
 *
 * @pre @p ctx is non-NULL; `ctx->m->type` is exFAT.
 * @pre The mount is in use and its geometry is populated.
 * @post On ::k_ra8_ok the report's counts and faults describe the volume.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_check_exfat(ra8_fs_check_ctx_t* ctx);
