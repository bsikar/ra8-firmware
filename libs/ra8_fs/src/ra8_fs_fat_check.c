/**
 * @file ra8_fs_fat_check.c
 * @brief Read-only consistency check (fsck): shared scaffolding + FAT12/16/32.
 *
 * @details
 * The FAT half of ::ra8_fs_check, plus the scaffolding its exFAT half
 * (`ra8_fs_fat_exfat_check.c`) shares: the finding recorder, the visited-cluster
 * bitmap primitives, and the public entry point that brackets the library lock
 * and dispatches on the volume type.
 *
 * The FAT check is three passes over a mounted volume, none of which writes to
 * it:
 *   1. **Classify.** Read every data cluster's FAT entry and tally free / used /
 *      defective, flagging any value that is neither free, a valid in-range
 *      next-cluster, an end-of-chain marker, nor the defective marker.
 *   2. **Walk.** Follow every directory chain from the root, marking each visited
 *      cluster in the caller's bitmap; a second visit is a cross-link, a run into
 *      free space or off the volume is a bad chain, and a directory entry naming a
 *      cluster the volume does not have is a bad entry.
 *   3. **Diff.** A cluster the FAT calls allocated that no walk reached is lost;
 *      and on FAT32 the FSInfo free count is compared against the classify pass.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000. FSInfo is sec 5.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 1: the directory tree is walked with an explicit worklist, never
 *     recursion.
 *   - Rule 2: every cluster walk is bounded by `count_of_clusters` and cut short
 *     by the visited bitmap; the worklist depth is capped by
 *     ::k_ra8_fs_check_max_dirs.
 *   - Rule 3: zero malloc; the visited bitmap is caller-supplied, the worklist a
 *     bounded stack.
 *   - Rule 7: every backend call is checked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_check.h"
#include "ra8_fs_fat_check_internal.h"
#include "ra8_fs_fat_internal.h"

/**
 * @enum ra8_fs_check_marker_t
 * @brief The FAT defective-cluster marker for each FAT width.
 *
 * @details One below the end-of-chain threshold in every FAT variant (MS FAT
 *          spec sec 4): a value the formatter or a surface scan writes to fence
 *          off a bad cluster. It is counted -- informationally -- rather than
 *          treated as corruption.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_check_bad_fat12 = 0x0FF7U,     /**< FAT12 defective-cluster marker. */
  k_check_bad_fat16 = 0xFFF7U,     /**< FAT16 defective-cluster marker. */
  k_check_bad_fat32 = 0x0FFFFFF7U, /**< FAT32 defective-cluster marker. */
} ra8_fs_check_marker_t;

/**
 * @enum ra8_fs_check_bit_t
 * @brief Bit-arithmetic constants for the caller-supplied visited bitmap.
 *
 * @details The visited bitmap packs one cluster per bit, LSB first, so a cluster
 *          index shifts right by ::k_check_byte_shift to its byte and masks with
 *          ::k_check_bit_mask to its bit -- the same layout the exFAT allocation
 *          bitmap uses, so the two diff directly.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_check_bit_mask   = 7U, /**< Cluster index -> bit within its byte.  */
  k_check_byte_shift = 3U, /**< log2(8): cluster index -> bitmap byte. */
} ra8_fs_check_bit_t;

/* =============================================================================
 * Shared scaffolding (used by the exFAT half too)
 * =============================================================================
 */

/* The shared scaffolding -- priv_check_fault(), priv_check_in_range(),
 * priv_check_mark(), priv_check_visit() and priv_check_zero_bitmap() -- is
 * ported to Zig as src/ra8_fs_check_abi.zig; the declarations in
 * ra8_fs_fat_check_internal.h link to it, and both the FAT passes below and
 * the exFAT half call it through them.
 */

/* Pass 1 (the FAT defective-cluster marker and the classify pass over every
 * FAT entry) is ported to Zig as src/ra8_fs_check_abi.zig; the prototypes in
 * ra8_fs_fat_check_internal.h link to it, and the fsinfo pass and priv_check_fat
 * below call it through them.
 */

/* =============================================================================
 * Pass 2: walk the directory tree, marking referenced clusters
 * =============================================================================
 */

/* Pass 2's chain marker (internal_fat_mark_chain) and the directory
 * worklist push (internal_fat_push) are ported to Zig as
 * src/ra8_fs_check_abi.zig; the prototypes in
 * ra8_fs_fat_check_internal.h link to them, and the entry processor and
 * tree scan below call them through those.
 */

/* ra8_fs_check_test_fat_push_overflow(): see header for the test-only contract. */
RA8_TEST_HELPER
void ra8_fs_check_test_fat_push_overflow(ra8_fs_check_ctx_t* ctx,
                                         uint32_t            cluster,
                                         bool                already_truncated)
{
  fat_dir_stack_t stack = {
    .top       = (uint32_t)k_ra8_fs_check_max_dirs,
    .truncated = already_truncated ? 1U : 0U,
  };
  internal_fat_push(ctx, &stack, cluster);
}

/* internal_fat_entry (the 32-byte directory-entry processor) and
 * internal_fat_visit_sector (the per-sector walk) are ported to Zig as
 * src/ra8_fs_check_abi.zig; the prototypes in
 * ra8_fs_fat_check_internal.h link to them, and the scan passes below
 * call them through those.
 */

/* internal_fat_scan_fixed_root (the FAT12/16 fixed-root walk) is
 * ported to Zig as src/ra8_fs_check_abi.zig; the prototype in
 * ra8_fs_fat_check_internal.h links to it.
 */

/* internal_fat_scan_cluster_dir (the cluster-chained directory walk)
 * is ported to Zig as src/ra8_fs_check_abi.zig; the test helper below
 * calls it through the prototype.
 */
RA8_TEST_HELPER
ra8_err_t ra8_fs_check_test_fat_scan_cluster_dir(ra8_fs_check_ctx_t* ctx, uint32_t first)
{
  fat_dir_stack_t stack = {};
  return internal_fat_scan_cluster_dir(ctx, &stack, first);
}

/* internal_fat_tree (the worklist driver) is ported to Zig as
 * src/ra8_fs_check_abi.zig; the prototype in
 * ra8_fs_fat_check_internal.h links to it.
 */
/* =============================================================================
 * Pass 3: diff for lost clusters, and the FAT32 FSInfo free count
 * =============================================================================
 */

/* internal_fat_diff (the lost-cluster pass) is ported to Zig as
 * src/ra8_fs_check_abi.zig; the prototype in
 * ra8_fs_fat_check_internal.h links to it.
 */
/* internal_fat_fsinfo (the FSInfo free-count compare) is ported to
 * Zig as src/ra8_fs_check_abi.zig; the prototype in
 * ra8_fs_fat_check_internal.h links to it.
 */
/* priv_check_fat (the FAT orchestrator) is ported to Zig as
 * src/ra8_fs_check_abi.zig; the prototype in
 * ra8_fs_fat_check_internal.h links to it.
 */
/* =============================================================================
 * Public entry point -- the lock bracket
 * =============================================================================
 */

/**
 * @brief Decide whether the caller's bitmap enables the reference passes.
 *
 * @details The bitmap must hold one bit per data cluster -- `(clusters_total + 7)
 *          / 8` bytes -- for the walk / lost analysis to run. A NULL or short
 *          bitmap leaves the classification pass to run alone, with
 *          `clusters_lost` reported as ::k_ra8_fs_check_unknown.
 *
 * @param[in]  total        The volume's data-cluster count.
 * @param[in]  bitmap       The caller's bitmap, or NULL.
 * @param[in]  bitmap_bytes The caller's bitmap size in bytes.
 *
 * @return Whether the reference passes may run.
 * @retval true  @p bitmap is non-NULL and large enough.
 * @retval false Otherwise.
 *
 * @pre None.
 * @pre @p total is the mounted volume's cluster count.
 * @post No state modified.
 * @post Result depends only on the inputs.
 *
 * @note Pure; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_check_bitmap_ok(uint32_t total, const uint8_t* bitmap, uint32_t bitmap_bytes)
{
  if (bitmap == nullptr) {
    return false;
  }
  return bitmap_bytes >= ((total + (uint32_t)k_check_bit_mask) >> (uint32_t)k_check_byte_shift);
}

/**
 * @brief Consistency check -- the guarded body of ::ra8_fs_check().
 *
 * @details Validates the arguments, seeds the report, decides whether the
 *          caller's bitmap enables the reference passes, and dispatches on the
 *          volume type into a local candidate report. The candidate is published
 *          only after the entire scan succeeds, so backend failures cannot expose
 *          a plausible-looking partial result. The public ::ra8_fs_check brackets
 *          this with the library lock; the full contract is documented there.
 *
 * @param[in]  handle       Mount handle.
 * @param[in]  bitmap       Scratch visited-cluster bitmap, or NULL.
 * @param[in]  bitmap_bytes Size of @p bitmap in bytes.
 * @param[out] report       Receives the findings.
 *
 * @return Error code.
 * @retval k_ra8_ok                Scan completed.
 * @retval k_ra8_err_null_ptr      @p handle or @p report is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle and @p report are non-NULL.
 * @post On ::k_ra8_ok the report describes the volume.
 * @post On error @p report retains its entry value.
 * @post No volume state is modified.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_check_locked(ra8_fs_mount_t*        handle,
                                       uint8_t*               bitmap,
                                       uint32_t               bitmap_bytes,
                                       ra8_fs_check_report_t* report)
{
  if ((handle == nullptr) || (report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  ra8_fs_check_report_t candidate = {};
  candidate.type                  = handle->type;
  candidate.clusters_total        = handle->count_of_clusters;
  const bool have_bitmap =
    internal_check_bitmap_ok(handle->count_of_clusters, bitmap, bitmap_bytes);
  candidate.referenced_scan = have_bitmap;
  candidate.clusters_lost   = have_bitmap ? 0U : (uint32_t)k_ra8_fs_check_unknown;
  ra8_fs_check_ctx_t ctx    = {.m           = handle,
                               .rep         = &candidate,
                               .bitmap      = have_bitmap ? bitmap : nullptr,
                               .bitmap_bits = handle->count_of_clusters};
  ra8_err_t          err    = k_ra8_ok;
  if (handle->type == k_ra8_fs_type_exfat) {
    err = priv_check_exfat(&ctx);
  } else {
    err = priv_check_fat(&ctx);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  *report = candidate;
  return k_ra8_ok;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_check(ra8_fs_mount_t*        handle,
                       uint8_t*               bitmap,
                       uint32_t               bitmap_bytes,
                       ra8_fs_check_report_t* report)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_check_locked(handle, bitmap, bitmap_bytes, report);
  priv_lock_release();
  return err;
}
