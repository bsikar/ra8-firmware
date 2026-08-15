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

/**
 * @struct fat_dir_stack_t
 * @brief Bounded worklist of directories still to walk (NASA P10 Rule 1).
 *
 * @details A stack of directory locations rather than recursion. Pushing past
 *          ::k_ra8_fs_check_max_dirs sets @p truncated -- recorded once as
 *          ::k_ra8_fs_check_fault_scan_truncated -- and drops the entry, so a
 *          pathologically deep or wide tree bounds the scan instead of
 *          overflowing the C stack.
 *
 * @invariant `top <= k_ra8_fs_check_max_dirs`.
 * @since 0.1.0
 */
typedef struct {
  dir_loc_t items[k_ra8_fs_check_max_dirs]; /**< The pending directory locations. */
  uint32_t  top;                            /**< Count of pending entries.        */
  uint8_t   truncated;                      /**< 1 once an overflow was dropped.  */
} fat_dir_stack_t;

/* =============================================================================
 * Shared scaffolding (used by the exFAT half too)
 * =============================================================================
 */

/* `priv_check_fault()`: see header for the documented contract. */
void priv_check_fault(ra8_fs_check_ctx_t*       ctx,
                      ra8_fs_check_fault_kind_t kind,
                      uint32_t                  cluster,
                      uint64_t                  lba,
                      uint32_t                  entry_off)
{
  if (ctx->rep->faults_total == 0U) {
    ctx->rep->first_fault.kind      = kind;
    ctx->rep->first_fault.cluster   = cluster;
    ctx->rep->first_fault.lba       = lba;
    ctx->rep->first_fault.entry_off = entry_off;
  }
  ctx->rep->faults_total++;
}

/* `priv_check_in_range()`: see header for the documented contract. */
bool priv_check_in_range(const ra8_fs_check_ctx_t* ctx, uint32_t cluster)
{
  if (cluster < (uint32_t)k_cluster_first_data) {
    return false;
  }
  return (cluster - (uint32_t)k_cluster_first_data) < ctx->rep->clusters_total;
}

/* `priv_check_mark()`: see header for the documented contract. */
bool priv_check_mark(ra8_fs_check_ctx_t* ctx, uint32_t cluster)
{
  const uint32_t idx  = cluster - (uint32_t)k_cluster_first_data;
  const uint32_t byte = idx >> (uint32_t)k_check_byte_shift;
  const uint8_t  mask = (uint8_t)(1U << (idx & (uint32_t)k_check_bit_mask));
  if ((ctx->bitmap[byte] & mask) != 0U) {
    return true;
  }
  ctx->bitmap[byte] = (uint8_t)(ctx->bitmap[byte] | mask);
  return false;
}

/* `priv_check_visit()`: see header for the documented contract. */
bool priv_check_visit(ra8_fs_check_ctx_t* ctx, uint32_t cluster, ra8_fs_check_fault_kind_t oor_kind)
{
  if (!priv_check_in_range(ctx, cluster)) {
    if (oor_kind == k_ra8_fs_check_fault_bad_dir_entry) {
      ctx->rep->entries_bad++;
    }
    priv_check_fault(ctx, oor_kind, cluster, 0U, 0U);
    return true;
  }
  if (priv_check_mark(ctx, cluster)) {
    ctx->rep->chains_crosslinked++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_crosslink, cluster, 0U, 0U);
    return true;
  }
  return false;
}

/* `priv_check_zero_bitmap()`: see header for the documented contract. */
void priv_check_zero_bitmap(ra8_fs_check_ctx_t* ctx)
{
  const uint32_t nbytes =
    (ctx->bitmap_bits + (uint32_t)k_check_bit_mask) >> (uint32_t)k_check_byte_shift;
  for (uint32_t i = 0U; i < nbytes; i++) {
    ctx->bitmap[i] = 0U;
  }
}

/* =============================================================================
 * Pass 1: classify every FAT entry
 * =============================================================================
 */

/**
 * @brief The FAT defective-cluster marker for the mounted variant.
 *
 * @details One below the end-of-chain threshold for the mounted FAT variant (MS FAT spec sec 4).
 *
 * @param[in] m Mounted FAT volume.
 * @return The marker value for `m->type`.
 * @retval k_check_bad_fat12 FAT12.
 * @retval k_check_bad_fat16 FAT16.
 * @retval k_check_bad_fat32 FAT32.
 * @pre @p m is non-NULL and `m->type` is a FAT variant.
 * @pre The mount's geometry is populated.
 * @post No state modified.
 * @post Result depends only on `m->type`.
 * @note Pure; trivially thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_fat_bad_marker(const ra8_fs_mount_t* m)
{
  if (m->type == k_ra8_fs_type_fat12) {
    return (uint32_t)k_check_bad_fat12;
  }
  if (m->type == k_ra8_fs_type_fat16) {
    return (uint32_t)k_check_bad_fat16;
  }
  return (uint32_t)k_check_bad_fat32;
}

/**
 * @brief Classify one FAT entry value into the report's cluster tallies.
 *
 * @details Free, defective and end-of-chain are each their own tally; a value in
 *          `[2, 2 + clusters_total)` is a valid next-cluster pointer and counts
 *          used; anything else -- the reserved 1, or a pointer past the last
 *          cluster -- is a bad FAT value, still counted used but recorded as a
 *          fault.
 *
 * @param[in,out] ctx     The scan context.
 * @param[in]     cluster The cluster whose entry @p value belongs to.
 * @param[in]     value   The FAT entry value read for @p cluster.
 *
 * @return Nothing.
 *
 * @pre @p ctx is non-NULL; the report's `clusters_total` is populated.
 * @pre @p value came from ::priv_fat_get for @p cluster.
 * @post Exactly one tally (`clusters_free` / `clusters_bad` / `clusters_used`) is
 *       incremented.
 * @post A reserved / out-of-range pointer raises a bad-FAT-value fault.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fat_classify_one(ra8_fs_check_ctx_t* ctx, uint32_t cluster, uint32_t value)
{
  if (value == (uint32_t)k_cluster_free) {
    ctx->rep->clusters_free++;
    return;
  }
  if (value == internal_fat_bad_marker(ctx->m)) {
    ctx->rep->clusters_bad++;
    return;
  }
  ctx->rep->clusters_used++;
  if (priv_is_eoc(ctx->m, value) != 0U) {
    return;
  }
  if (!priv_check_in_range(ctx, value)) {
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_fat_value, cluster, 0U, 0U);
  }
}

/**
 * @brief Pass 1: read and classify every data cluster's FAT entry.
 *
 * @details A single pass over the FAT, tallying free, used and defective clusters.
 *
 * @param[in,out] ctx The scan context.
 * @return Error code.
 * @retval k_ra8_ok    Every entry classified.
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx is non-NULL; `ctx->m->type` is a FAT variant.
 * @pre The report's `clusters_total` is populated.
 * @post The free / used / bad tallies describe the whole FAT.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): `clusters_total` iterations.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_classify(ra8_fs_check_ctx_t* ctx)
{
  const uint32_t last = (uint32_t)k_cluster_first_data + ctx->rep->clusters_total;
  for (uint32_t c = (uint32_t)k_cluster_first_data; c < last; c++) {
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(ctx->m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    internal_fat_classify_one(ctx, c, v);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Pass 2: walk the directory tree, marking referenced clusters
 * =============================================================================
 */

/**
 * @brief Mark a file's whole cluster chain, detecting cross-links and breaks.
 *
 * @details Follows the FAT from @p first, marking each cluster. A cluster already
 *          marked is a cross-link (or a loop) and stops the walk; a next-cluster
 *          that is free or out of range is a broken chain and stops it too. The
 *          hop count is bounded by `clusters_total`. After the last permitted
 *          hop, a non-terminal successor is checked once more without reading
 *          another FAT entry: it must revisit one of the volume's clusters or
 *          name a cluster outside the volume, so the bound cannot look clean.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     first The file's first cluster (>= 2).
 *
 * @return Error code.
 * @retval k_ra8_ok    The chain was walked (a fault may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p ctx and its bitmap are non-NULL; @p first is in range.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every cluster of the chain up to the first fault reads as visited.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `clusters_total` hops, bitmap-terminated.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_mark_chain(ra8_fs_check_ctx_t* ctx, uint32_t first)
{
  uint32_t c = first;
  for (uint32_t hop = 0U; hop < ctx->rep->clusters_total; hop++) {
    if (priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_chain)) {
      return k_ra8_ok;
    }
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(ctx->m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(ctx->m, v) != 0U) {
      return k_ra8_ok;
    }
    if (!priv_check_in_range(ctx, v)) {
      priv_check_fault(ctx, k_ra8_fs_check_fault_bad_chain, c, 0U, 0U);
      return k_ra8_ok;
    }
    c = v;
  }
  (void)priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_chain);
  return k_ra8_ok;
}

/**
 * @brief Push a subdirectory onto the walk stack, capping the depth.
 *
 * @details Records ::k_ra8_fs_check_fault_scan_truncated the first time the stack
 *          is full and drops the entry, so the tree walk stays bounded.
 *
 * @param[in,out] ctx   The scan context (for the truncation fault).
 * @param[in,out] stack The directory worklist.
 * @param[in]     clus  The subdirectory's first cluster.
 *
 * @return Nothing.
 *
 * @pre @p ctx and @p stack are non-NULL; @p clus is in range.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post The subdirectory is queued, or the stack was full and it was dropped.
 * @post On the first overflow a truncation fault is recorded.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fat_push(ra8_fs_check_ctx_t* ctx, fat_dir_stack_t* stack, uint32_t clus)
{
  if (stack->top >= (uint32_t)k_ra8_fs_check_max_dirs) {
    /* GCOVR_EXCL_START -- a directory tree more than k_ra8_fs_check_max_dirs deep */
    if (stack->truncated == 0U) {
      stack->truncated = 1U;
      priv_check_fault(ctx, k_ra8_fs_check_fault_scan_truncated, clus, 0U, 0U);
    }
    return;
    /* GCOVR_EXCL_STOP */
  }
  stack->items[stack->top].is_root = 0U;
  stack->items[stack->top].cluster = clus;
  stack->top++;
}

/**
 * @brief Process one 32-byte FAT directory entry.
 *
 * @details Skips the entries that name nothing walkable -- end-of-directory,
 *          deleted, long-name, "." / "..", and the volume-label -- then validates
 *          the first cluster of a real file or directory. A file's chain is marked
 *          now; a directory is pushed to be walked (and marked) when it is popped.
 *
 * @param[in,out] ctx       The scan context.
 * @param[in,out] stack     The directory worklist.
 * @param[in]     ent       The 32-byte directory entry.
 * @param[in]     lba       Volume-relative sector the entry was read from.
 * @param[in]     entry_off Byte offset of the entry in that sector.
 * @param[out]    out_eod   Set to 1 when @p ent is the end-of-directory marker.
 *
 * @return Error code.
 * @retval k_ra8_ok    The entry was processed (a fault may have been recorded).
 * @retval k_ra8_err_* Backend read failure while marking a file chain.
 *
 * @pre Every pointer is non-NULL; @p ent points at 32 valid bytes.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post On a valid file entry its chain is marked; on a directory it is queued.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_entry(ra8_fs_check_ctx_t* ctx,
                                    fat_dir_stack_t*    stack,
                                    const uint8_t*      ent,
                                    uint64_t            lba,
                                    uint32_t            entry_off,
                                    uint8_t*            out_eod)
{
  *out_eod            = 0U;
  const uint8_t name0 = ent[k_dir_off_name];
  const uint8_t attr  = ent[k_dir_off_attr];
  if (name0 == (uint8_t)k_dir_marker_free_perm) {
    *out_eod = 1U;
    return k_ra8_ok;
  }
  if (name0 == (uint8_t)k_dir_marker_free_used) {
    return k_ra8_ok;
  }
  if (attr == (uint8_t)k_ra8_fs_attr_lfn) {
    return k_ra8_ok;
  }
  if (name0 == (uint8_t)k_dir_marker_dot) {
    return k_ra8_ok;
  }
  const uint8_t is_dir = ((attr & (uint8_t)k_ra8_fs_attr_directory) != 0U) ? 1U : 0U;
  if ((is_dir == 0U) && ((attr & (uint8_t)k_ra8_fs_attr_volume_id) != 0U)) {
    return k_ra8_ok; /* a volume-label entry owns no chain */
  }
  const uint32_t first = priv_entry_first_cluster(ent);
  if ((first != 0U) && !priv_check_in_range(ctx, first)) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_dir_entry, first, lba, entry_off);
    return k_ra8_ok;
  }
  if (is_dir != 0U) {
    if (first != 0U) {
      internal_fat_push(ctx, stack, first);
    }
    return k_ra8_ok;
  }
  ctx->rep->files_visited++;
  if (first != 0U) {
    return internal_fat_mark_chain(ctx, first);
  }
  return k_ra8_ok;
}

/**
 * @brief Process the 16 directory entries of one loaded sector.
 *
 * @details Processes the sixteen directory entries packed into one 512-byte sector.
 *
 * @param[in,out] ctx     The scan context.
 * @param[in,out] stack   The directory worklist.
 * @param[in]     buf     The 512-byte directory sector.
 * @param[in]     lba     Volume-relative sector number of @p buf.
 * @param[out]    out_eod Set to 1 when the end-of-directory marker was hit.
 * @return Error code.
 * @retval k_ra8_ok    The sector was processed.
 * @retval k_ra8_err_* Backend read failure while marking a chain.
 * @pre Every pointer is non-NULL; @p buf holds a directory sector.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post `*out_eod` is 1 iff the walk should stop.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): one sector's worth of entries.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_visit_sector(ra8_fs_check_ctx_t* ctx,
                                           fat_dir_stack_t*    stack,
                                           const uint8_t*      buf,
                                           uint64_t            lba,
                                           uint8_t*            out_eod)
{
  for (uint32_t e = 0U; e < priv_dir_eps(ctx->m); e++) {
    const uint32_t  off = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
    uint8_t         eod = 0U;
    const ra8_err_t err = internal_fat_entry(ctx, stack, &buf[off], lba, off, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
    if (eod != 0U) {
      *out_eod = 1U;
      return k_ra8_ok;
    }
  }
  *out_eod = 0U;
  return k_ra8_ok;
}

/**
 * @brief Walk the FAT12/16 fixed root directory region.
 *
 * @details Reads the FAT12/16 fixed root-directory region sector by sector.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in,out] stack The directory worklist.
 * @return Error code.
 * @retval k_ra8_ok    The fixed root was walked.
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx and @p stack are non-NULL; the mount is FAT12/16.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every visible root entry was processed.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): the fixed root's sector span.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_scan_fixed_root(ra8_fs_check_ctx_t* ctx, fat_dir_stack_t* stack)
{
  const uint32_t secs = (ctx->m->root_entries + (priv_dir_eps(ctx->m) - 1U)) / priv_dir_eps(ctx->m);
  for (uint32_t s = 0U; s < secs; s++) {
    const uint64_t lba = ctx->m->first_root_lba + s;
    uint8_t* const buf = priv_sec_walk();
    ra8_err_t      err = priv_read_sector(ctx->m, lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    uint8_t eod = 0U;
    err         = internal_fat_visit_sector(ctx, stack, buf, lba, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
    if (eod != 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Walk a cluster-chained directory (a FAT32 root or any subdirectory).
 *
 * @details Follows the directory's own cluster chain, marking each cluster (a
 *          revisit is a cross-link), and processes every sector of each cluster.
 *          The end-of-directory marker or a terminal FAT entry ends the walk.
 *          Reaching the hop bound validates the pending successor once more so a
 *          full-volume cycle or off-volume tail is reported, never accepted.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in,out] stack The directory worklist.
 * @param[in]     first The directory's first cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    The directory was walked.
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p ctx and @p stack are non-NULL; @p first is a directory's first cluster.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every reachable directory cluster is marked visited.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `clusters_total` hops, bitmap-terminated.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_fat_scan_cluster_dir(ra8_fs_check_ctx_t* ctx, fat_dir_stack_t* stack, uint32_t first)
{
  uint32_t c = first;
  for (uint32_t hop = 0U; hop < ctx->rep->clusters_total; hop++) {
    if (priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_dir_entry)) {
      return k_ra8_ok;
    }
    const uint64_t base = priv_cluster_to_lba(ctx->m, c);
    for (uint32_t s = 0U; s < ctx->m->sectors_per_cluster; s++) {
      uint8_t* const buf = priv_sec_walk();
      ra8_err_t      err = priv_read_sector(ctx->m, base + s, buf);
      if (err != k_ra8_ok) {
        return err;
      }
      uint8_t eod = 0U;
      err         = internal_fat_visit_sector(ctx, stack, buf, base + s, &eod);
      if (err != k_ra8_ok) {
        return err;
      }
      if (eod != 0U) {
        return k_ra8_ok;
      }
    }
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(ctx->m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(ctx->m, v) != 0U) {
      return k_ra8_ok;
    }
    c = v;
  }
  (void)priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_dir_entry);
  return k_ra8_ok;
}

RA8_TEST_HELPER
ra8_err_t ra8_fs_check_test_fat_scan_cluster_dir(ra8_fs_check_ctx_t* ctx, uint32_t first)
{
  fat_dir_stack_t stack = {};
  return internal_fat_scan_cluster_dir(ctx, &stack, first);
}

/**
 * @brief Walk the whole directory tree from the root with an explicit worklist.
 *
 * @details Pops directories off the worklist and walks each, the volume root first.
 *
 * @param[in,out] ctx The scan context.
 * @return Error code.
 * @retval k_ra8_ok    The tree was walked (faults may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx and its bitmap are non-NULL; `ctx->m->type` is a FAT variant.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every reachable file and directory cluster is marked.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): at most ::k_ra8_fs_check_max_dirs pops.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_tree(ra8_fs_check_ctx_t* ctx)
{
  fat_dir_stack_t stack  = {};
  stack.items[0].is_root = 1U;
  stack.items[0].cluster = 0U;
  stack.top              = 1U;
  const bool fat32       = (ctx->m->type == k_ra8_fs_type_fat32);
  while (stack.top > 0U) {
    stack.top--;
    const dir_loc_t loc = stack.items[stack.top];
    ctx->rep->dirs_visited++;
    ra8_err_t err = k_ra8_ok;
    if ((loc.is_root != 0U) && !fat32) {
      err = internal_fat_scan_fixed_root(ctx, &stack);
    } else {
      const uint32_t first = (loc.is_root != 0U) ? ctx->m->root_cluster : loc.cluster;
      err                  = internal_fat_scan_cluster_dir(ctx, &stack, first);
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Pass 3: diff for lost clusters, and the FAT32 FSInfo free count
 * =============================================================================
 */

/**
 * @brief Pass 3: flag allocated clusters no directory chain reached.
 *
 * @details A single pass over the FAT flagging allocated clusters that no chain reached.
 *
 * @param[in,out] ctx The scan context.
 * @return Error code.
 * @retval k_ra8_ok    The diff completed.
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx and its bitmap are non-NULL; the tree walk has run.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post `clusters_lost` counts allocated-but-unvisited clusters.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): `clusters_total` iterations.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_diff(ra8_fs_check_ctx_t* ctx)
{
  const uint32_t marker = internal_fat_bad_marker(ctx->m);
  const uint32_t last   = (uint32_t)k_cluster_first_data + ctx->rep->clusters_total;
  for (uint32_t c = (uint32_t)k_cluster_first_data; c < last; c++) {
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(ctx->m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((v == (uint32_t)k_cluster_free) || (v == marker)) {
      continue;
    }
    const uint32_t idx  = c - (uint32_t)k_cluster_first_data;
    const uint8_t  mask = (uint8_t)(1U << (idx & (uint32_t)k_check_bit_mask));
    if ((ctx->bitmap[idx >> (uint32_t)k_check_byte_shift] & mask) == 0U) {
      ctx->rep->clusters_lost++;
      priv_check_fault(ctx, k_ra8_fs_check_fault_lost_cluster, c, 0U, 0U);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Compare a FAT32 volume's FSInfo free count against the classify pass.
 *
 * @details Reads `BPB_FSInfo` from the boot sector, validates the three FSInfo
 *          signatures (MS FAT spec sec 5), and only then compares the stored free
 *          count to `clusters_free`. A volume with no or an untrusted FSInfo
 *          sector is not a fault -- there is simply nothing to compare.
 *
 * @param[in,out] ctx The scan context.
 * @return Error code.
 * @retval k_ra8_ok    Compared, or nothing to compare.
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx is non-NULL; the classify pass has run.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post A trustworthy FSInfo count that disagrees raises a fault.
 * @post No volume state is modified.
 * @note Two sector reads; no loop.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fat_fsinfo(ra8_fs_check_ctx_t* ctx)
{
  if (ctx->m->type != k_ra8_fs_type_fat32) {
    return k_ra8_ok;
  }
  uint8_t* const  boot = priv_sec_walk();
  const ra8_err_t be   = priv_read_sector(ctx->m, 0U, boot);
  if (be != k_ra8_ok) {
    return be; /* GCOVR_EXCL_LINE -- boot-sector read failure */
  }
  const uint32_t lba = (uint32_t)priv_rd16(&boot[k_fmt_off_f32_fsinfo]);
  if ((lba == 0U) || (lba >= ctx->m->reserved_sectors)) {
    return k_ra8_ok;
  }
  uint8_t* const  sec = priv_sec_walk();
  const ra8_err_t se  = priv_read_sector(ctx->m, lba, sec);
  if (se != k_ra8_ok) {
    return se; /* GCOVR_EXCL_LINE -- FSInfo-sector read failure */
  }
  if (priv_rd32(&sec[k_fmt_fsi_off_lead]) != (uint32_t)k_fmt_fsi_lead_sig) {
    return k_ra8_ok;
  }
  if (priv_rd32(&sec[k_fmt_fsi_off_struct]) != (uint32_t)k_fmt_fsi_struct_sig) {
    return k_ra8_ok;
  }
  if (priv_rd32(&sec[k_fmt_fsi_off_trail]) != (uint32_t)k_fmt_fsi_trail_sig) {
    return k_ra8_ok;
  }
  const uint32_t stored = priv_rd32(&sec[k_fmt_fsi_off_free]);
  if ((stored != (uint32_t)k_fs_free_unknown) && (stored != ctx->rep->clusters_free)) {
    priv_check_fault(ctx, k_ra8_fs_check_fault_free_count_bad, 0U, lba, 0U);
  }
  return k_ra8_ok;
}

/* `priv_check_fat()`: see header for the documented contract. */
ra8_err_t priv_check_fat(ra8_fs_check_ctx_t* ctx)
{
  ra8_err_t err = internal_fat_classify(ctx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (ctx->bitmap != nullptr) {
    priv_check_zero_bitmap(ctx);
    err = internal_fat_tree(ctx);
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_fat_diff(ctx);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return internal_fat_fsinfo(ctx);
}

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
