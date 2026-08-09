/**
 * @file ra8_fs_fat_exfat_check.c
 * @brief Read-only consistency check (fsck): the exFAT half.
 *
 * @details
 * The exFAT side of ::ra8_fs_check, sharing the scaffolding in
 * `ra8_fs_fat_check.c` (the finding recorder and the visited-cluster bitmap).
 * exFAT makes the allocation bitmap the sole authority on which clusters are in
 * use (exFAT spec sec 7.1), so the check has a different shape from the FAT one:
 *
 *   1. **Walk the directory tree**, from the root, marking in the caller's
 *      scratch bitmap every cluster anything references -- each file's data run,
 *      each directory's own run, and the three system runs (the allocation
 *      bitmap, the up-case table, the root directory). Every File entry set is
 *      verified as it is walked: its `SetChecksum` must recompute to the stored
 *      value, and its Stream entry's `NameHash` must recompute to the stored one.
 *   2. **Diff against the allocation bitmap**, in BOTH directions: a bit the
 *      bitmap sets that nothing referenced is a lost cluster, and a cluster
 *      something referenced whose bit is clear is a bitmap mismatch.
 *
 * With no scratch bitmap supplied the walk is skipped and the pass reports only
 * the allocation bitmap's own used / free population count.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, March 2021. Entry sets are sec 6; the allocation bitmap
 *     sec 7.1; the checksums sec 6.3.3 (SetChecksum) and 7.5 (NameHash).
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 1: the directory tree is walked with an explicit worklist, never
 *     recursion.
 *   - Rule 2: every cluster walk is bounded by `count_of_clusters` and cut short
 *     by the visited bitmap; the entry walk by `k_exfat_scan_limit`; the worklist
 *     depth by ::k_ra8_fs_check_max_dirs.
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
 * @enum ra8_fs_exfat_check_const_t
 * @brief Bit-arithmetic constants for the exFAT allocation-bitmap diff.
 *
 * @details The allocation bitmap packs one cluster per bit, LSB first, exactly
 *          like the visited bitmap the check marks, so cluster index `>> 3` is a
 *          byte and `& 7` is a bit -- and the two can be compared bit for bit.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_exchk_bits_per_byte = 8U, /**< Bits in one allocation-bitmap byte.    */
  k_exchk_bit_mask      = 7U, /**< Cluster index -> bit within its byte.  */
  k_exchk_byte_shift    = 3U, /**< log2(8): cluster index -> bitmap byte. */
} ra8_fs_exfat_check_const_t;

/**
 * @struct exfat_dir_stack_t
 * @brief Bounded worklist of exFAT directories still to walk (NASA P10 Rule 1).
 *
 * @details A stack of ::exfat_dir_t rather than recursion. Pushing past
 *          ::k_ra8_fs_check_max_dirs sets @p truncated -- recorded once as
 *          ::k_ra8_fs_check_fault_scan_truncated -- and drops the entry.
 *
 * @invariant `top <= k_ra8_fs_check_max_dirs`.
 * @since 0.1.0
 */
typedef struct {
  exfat_dir_t items[k_ra8_fs_check_max_dirs]; /**< The pending directories.        */
  uint32_t    top;                            /**< Count of pending entries.       */
  uint8_t     truncated;                      /**< 1 once an overflow was dropped. */
} exfat_dir_stack_t;

/**
 * @brief Count the set bits in one byte (portable, no compiler builtin).
 *
 * @details A bounded eight-iteration bit count, so the result is identical on the host and on the target.
 *
 * @param[in] b Byte to count.
 * @return The number of 1 bits in @p b.
 * @retval 0..8 The population count.
 * @pre None (total over uint8_t).
 * @pre @p b is a whole byte.
 * @post No state modified.
 * @post Result is at most 8.
 * @note Pure; trivially thread-safe.
 * @note Bounded loop (NASA Rule 2): exactly ::k_exchk_bits_per_byte iterations.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_exchk_popcount8(uint8_t b)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_exchk_bits_per_byte; i++) {
    n += ((uint32_t)b >> i) & 1U;
  }
  return n;
}

/* =============================================================================
 * Marking referenced clusters
 * =============================================================================
 */

/**
 * @brief Mark a contiguous cluster run visited, detecting cross-links.
 *
 * @details Used for a NoFatChain file and for the bitmap / up-case system runs.
 *          A cluster already visited is a cross-link; a cluster out of range is a
 *          bad entry. The run length is capped at `clusters_total` so a corrupt
 *          DataLength cannot spin the loop.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     first First cluster of the run.
 * @param[in]     nclus Cluster count of the run.
 *
 * @return Nothing.
 *
 * @pre @p ctx and its bitmap are non-NULL.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every in-range cluster of the run up to the first fault is marked.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): capped at `clusters_total` iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exchk_mark_run(ra8_fs_check_ctx_t* ctx, uint32_t first, uint32_t nclus)
{
  uint32_t n = nclus;
  if (n > ctx->rep->clusters_total) {
    n = ctx->rep->clusters_total;
  }
  for (uint32_t i = 0U; i < n; i++) {
    const uint32_t c = first + i;
    if (priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_dir_entry)) {
      return;
    }
  }
}

/**
 * @brief Mark a FAT-chained file's whole chain visited, detecting cross-links.
 *
 * @details A file whose run fragmented carries a real FAT chain instead of
 *          NoFatChain; this follows it, marking each cluster and flagging a
 *          revisit (cross-link) or a next-cluster out of range (bad chain).
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     first The file's first cluster (in range).
 *
 * @return Error code.
 * @retval k_ra8_ok    The chain was walked (a fault may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p ctx and its bitmap are non-NULL; @p first is in range.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every cluster of the chain up to the first fault is marked.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `clusters_total` hops, bitmap-terminated.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_mark_fatchain(ra8_fs_check_ctx_t* ctx, uint32_t first)
{
  uint32_t c = first;
  for (uint32_t hop = 0U; hop < ctx->rep->clusters_total; hop++) {
    if (priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_chain)) {
      return k_ra8_ok; /* GCOVR_EXCL_LINE -- a cross-linked FAT-chained exFAT file */
    }
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(ctx->m, c, &v);
    if (err != k_ra8_ok) {
      return err; /* GCOVR_EXCL_LINE -- FAT read failure walking a chained file */
    }
    if (priv_is_eoc(ctx->m, v) != 0U) {
      return k_ra8_ok;
    }
    if (!priv_check_in_range(ctx, v)) {
      /* GCOVR_EXCL_START -- a chained exFAT file whose FAT runs off the volume */
      priv_check_fault(ctx, k_ra8_fs_check_fault_bad_chain, c, 0U, 0U);
      return k_ra8_ok;
      /* GCOVR_EXCL_STOP */
    }
    c = v;
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE -- hop bound exceeds count_of_clusters */
}

/**
 * @brief Mark a directory's own cluster allocation visited.
 *
 * @details Walks the directory's run (contiguous or FAT-chained, decided by
 *          `dir->contig_end`) with ::priv_exfat_step_cluster, marking each cluster
 *          -- so a directory whose contents stop at an early end-of-directory
 *          marker still has its trailing allocated clusters counted as referenced,
 *          which the entry walk alone would miss.
 *
 * @param[in,out] ctx The scan context.
 * @param[in]     dir The directory whose allocation is marked.
 *
 * @return Error code.
 * @retval k_ra8_ok    The allocation was marked (a fault may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p ctx and its bitmap are non-NULL; `dir->cluster` is a heap cluster.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every cluster of the directory's run up to the first fault is marked.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `clusters_total` hops, bitmap-terminated.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_mark_dir_alloc(ra8_fs_check_ctx_t* ctx, const exfat_dir_t* dir)
{
  uint32_t c = dir->cluster;
  for (uint32_t hop = 0U; hop < ctx->rep->clusters_total; hop++) {
    if (priv_check_visit(ctx, c, k_ra8_fs_check_fault_bad_dir_entry)) {
      return k_ra8_ok; /* GCOVR_EXCL_LINE -- a cross-linked directory cluster */
    }
    uint32_t        next = 0U;
    const ra8_err_t e    = priv_exfat_step_cluster(ctx->m, c, dir->contig_end, &next);
    if (e == k_ra8_err_not_found) {
      return k_ra8_ok; /* the run ended */
    }
    if (e != k_ra8_ok) {
      return e;
    }
    c = next; /* GCOVR_EXCL_LINE -- a directory whose allocation spans >1 cluster */
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE -- hop bound exceeds count_of_clusters */
}

/**
 * @brief Mark the clusters a bitmap (0x81) or up-case (0x82) system entry owns.
 *
 * @details Both system entries carry `FirstCluster` and `DataLength` in the same
 *          fields a Stream entry uses, so the run is `ceil(DataLength / cluster
 *          bytes)` clusters from `FirstCluster`.
 *
 * @param[in,out] ctx The scan context.
 * @param[in]     e   The 32-byte system directory entry.
 *
 * @return Nothing.
 *
 * @pre @p ctx and its bitmap are non-NULL; @p e is a 0x81 or 0x82 entry.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post The system run is marked referenced.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exchk_system_run(ra8_fs_check_ctx_t* ctx, const uint8_t* e)
{
  const uint32_t first  = priv_rd32(&e[k_exfat_strm_off_clus]);
  const uint32_t bytes  = priv_rd32(&e[k_exfat_strm_off_dlen]);
  const uint32_t cbytes = ctx->m->sectors_per_cluster * (uint32_t)k_ra8_fs_bytes_per_sector;
  const uint32_t nclus  = (bytes + cbytes - 1U) / cbytes;
  if (first == 0U) {
    return; /* GCOVR_EXCL_LINE -- a system entry that names no cluster */
  }
  priv_exchk_mark_run(ctx, first, nclus);
}

/* =============================================================================
 * Entry-set verification (SetChecksum + NameHash)
 * =============================================================================
 */

/**
 * @brief Extract a File entry set's name into UTF-16 units for hashing.
 *
 * @details Reads @p nlen units out of the Name (0xC1) entries, which begin at the
 *          third entry of the set (byte offset 64) and pack 15 units each.
 *
 * @param[in]  set   The gathered entry-set bytes.
 * @param[in]  nlen  Name length in UTF-16 units (clamped to ::k_exfat_name_cap).
 * @param[out] units Receives @p nlen UTF-16 code units.
 *
 * @return Nothing.
 *
 * @pre @p set and @p units are non-NULL; @p nlen <= ::k_exfat_name_cap.
 * @pre @p set holds the File + Stream + Name entries.
 * @post @p units[0..nlen) mirror the on-disk name.
 * @post No state outside @p units is touched.
 *
 * @note Bounded loop (NASA Rule 2): @p nlen (<= 64) iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exchk_extract_name(const uint8_t* set, uint32_t nlen, uint16_t* units)
{
  for (uint32_t i = 0U; i < nlen; i++) {
    const uint32_t ent = 2U + (i / (uint32_t)k_exfat_name_per_entry);
    const uint32_t col = i % (uint32_t)k_exfat_name_per_entry;
    const uint32_t off =
      (ent * (uint32_t)k_exfat_entry_bytes) + (uint32_t)k_exfat_name_off + (col * 2U);
    units[i] = priv_rd16(&set[off]);
  }
}

/**
 * @brief Verify a gathered entry set's SetChecksum and NameHash.
 *
 * @details Recomputes both from the set's own bytes and compares to the stored
 *          values -- the two integrity fields exFAT keeps over an entry set --
 *          recording a fault for each mismatch. The name is clamped to the
 *          driver's name cap for the hash extraction; a longer foreign name still
 *          has its checksum verified over the full set.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     set   The gathered entry-set bytes (File + Stream + Name).
 * @param[in]     count Entry count of the set.
 * @param[in]     lba   Volume-relative sector the File entry was read from.
 * @param[in]     off   Byte offset of the File entry in that sector.
 *
 * @return Nothing.
 *
 * @pre @p ctx and @p set are non-NULL; `2 <= count <= k_exfat_set_max_entries`.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post A SetChecksum or NameHash mismatch raises a fault and bumps `entries_bad`.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exchk_verify_set(ra8_fs_check_ctx_t* ctx,
                                  const uint8_t*      set,
                                  uint32_t            count,
                                  uint32_t            lba,
                                  uint32_t            off)
{
  const uint16_t stored_cs = priv_rd16(&set[k_exfat_off_file_csum]);
  const uint16_t calc_cs   = priv_exfat_set_checksum(set, count * (uint32_t)k_exfat_entry_bytes);
  if (stored_cs != calc_cs) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_set_checksum, 0U, lba, off);
  }
  const uint8_t* strm = &set[k_exfat_entry_bytes];
  uint32_t       nlen = (uint32_t)strm[k_exfat_strm_off_nlen];
  if (nlen > (uint32_t)k_exfat_name_cap) {
    nlen = (uint32_t)k_exfat_name_cap; /* GCOVR_EXCL_LINE -- a name over the driver cap */
  }
  uint16_t units[k_exfat_name_cap] = {};
  priv_exchk_extract_name(set, nlen, units);
  const uint16_t stored_hash = priv_rd16(&strm[k_exfat_off_strm_hash]);
  const uint16_t calc_hash   = priv_exfat_name_hash(units, nlen);
  if (stored_hash != calc_hash) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_name_hash, 0U, lba, off);
  }
}

/* =============================================================================
 * Directory-tree walk
 * =============================================================================
 */

/**
 * @brief Push an exFAT subdirectory onto the walk stack, capping the depth.
 *
 * @details Guards the directory worklist against overflow, recording one truncation fault.
 *
 * @param[in,out] ctx   The scan context (for the truncation fault).
 * @param[in,out] stack The directory worklist.
 * @param[in]     dir   The subdirectory to queue.
 * @return Nothing.
 * @pre @p ctx, @p stack are non-NULL.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post The subdirectory is queued, or dropped with a one-time truncation fault.
 * @post No volume state is modified.
 * @note Not thread-safe; the check holds the library lock.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
priv_exchk_push(ra8_fs_check_ctx_t* ctx, exfat_dir_stack_t* stack, const exfat_dir_t* dir)
{
  if (stack->top >= (uint32_t)k_ra8_fs_check_max_dirs) {
    /* GCOVR_EXCL_START -- a directory tree more than k_ra8_fs_check_max_dirs deep */
    if (stack->truncated == 0U) {
      stack->truncated = 1U;
      priv_check_fault(ctx, k_ra8_fs_check_fault_scan_truncated, dir->cluster, 0U, 0U);
    }
    return;
    /* GCOVR_EXCL_STOP */
  }
  stack->items[stack->top] = *dir;
  stack->top++;
}

/**
 * @brief Validate a verified entry set's first cluster and mark or queue it.
 *
 * @details The cluster half of ::priv_exchk_set, split out to keep both inside
 *          the function-size gate. Reads the Stream entry, validates the first
 *          cluster, then counts an empty file, records an out-of-range entry,
 *          queues a subdirectory, or marks a file's run (contiguous or chained).
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     set   The gathered entry-set bytes (File + Stream + Name).
 * @param[in]     file  The 32-byte File (0x85) entry.
 * @param[in,out] stack The directory worklist.
 * @param[in]     lba   Volume-relative sector the File entry was read from.
 * @param[in]     off   Byte offset of the File entry in that sector.
 *
 * @return Error code.
 * @retval k_ra8_ok    The entry was processed (faults may have been recorded).
 * @retval k_ra8_err_* Backend read failure marking a FAT-chained file.
 *
 * @pre Every pointer is non-NULL; @p set holds a verified entry set.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post A valid file's clusters are marked; a directory is queued.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_set_clusters(ra8_fs_check_ctx_t* ctx,
                                         const uint8_t*      set,
                                         const uint8_t*      file,
                                         exfat_dir_stack_t*  stack,
                                         uint32_t            lba,
                                         uint32_t            off)
{
  const uint8_t* strm  = &set[k_exfat_entry_bytes];
  const uint32_t first = priv_rd32(&strm[k_exfat_strm_off_clus]);
  const uint32_t dlen  = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  const uint8_t  is_dir =
    ((file[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_directory) != 0U) ? 1U : 0U;
  if (first == 0U) {
    if (is_dir == 0U) {
      ctx->rep->files_visited++;
    }
    return k_ra8_ok;
  }
  if (!priv_check_in_range(ctx, first)) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_dir_entry, first, lba, off);
    return k_ra8_ok;
  }
  if (is_dir != 0U) {
    exfat_dir_t sub = {};
    priv_exfat_dir_from_set(ctx->m, strm, &sub);
    priv_exchk_push(ctx, stack, &sub);
    return k_ra8_ok;
  }
  ctx->rep->files_visited++;
  const uint32_t cbytes = ctx->m->sectors_per_cluster * (uint32_t)k_ra8_fs_bytes_per_sector;
  const uint32_t nclus  = (dlen == 0U) ? 1U : ((dlen + cbytes - 1U) / cbytes);
  if ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) {
    priv_exchk_mark_run(ctx, first, nclus);
    return k_ra8_ok;
  }
  return priv_exchk_mark_fatchain(ctx, first);
}

/**
 * @brief Gather and process one File entry set: verify it, then mark or queue it.
 *
 * @details Reads the whole set from @p cur (advancing it past the set), verifies
 *          its SetChecksum and NameHash, validates its first cluster, and then
 *          either marks a file's clusters (contiguous or FAT-chained) or queues a
 *          subdirectory to be walked. A SecondaryCount outside the writable range
 *          is a malformed set: it is faulted and skipped, and the leftover
 *          secondary entries the outer walk then sees are ignored as non-File
 *          types.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in,out] cur   The directory cursor, positioned just after the File entry.
 * @param[in]     file  The 32-byte File (0x85) entry.
 * @param[in,out] stack The directory worklist.
 *
 * @return Error code.
 * @retval k_ra8_ok    The set was processed (faults may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre Every pointer is non-NULL; @p file is a 0x85 entry.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post A valid file's clusters are marked; a directory is queued.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; the check holds the library lock.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_set(ra8_fs_check_ctx_t* ctx,
                                exfat_cursor_t*     cur,
                                const uint8_t*      file,
                                exfat_dir_stack_t*  stack)
{
  const uint32_t count = 1U + (uint32_t)file[k_exfat_off_file_secnt];
  const uint32_t bic   = (cur->entry_in_cluster - 1U) * (uint32_t)k_exfat_entry_bytes;
  const uint32_t lba =
    priv_cluster_to_lba(ctx->m, cur->cluster) + (bic / k_ra8_fs_bytes_per_sector);
  const uint32_t off = bic % (uint32_t)k_ra8_fs_bytes_per_sector;
  if ((count < 2U) || (count > (uint32_t)k_exfat_set_max_entries)) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_dir_entry, 0U, lba, off);
    return k_ra8_ok;
  }
  uint8_t set[(uint32_t)k_exfat_set_max_entries * (uint32_t)k_exfat_entry_bytes] = {};
  priv_byte_copy(set, file, (uint32_t)k_exfat_entry_bytes);
  for (uint32_t k = 1U; k < count; k++) {
    const ra8_err_t e =
      priv_exfat_next_entry(ctx->m, cur, &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (e == k_ra8_err_not_found) {
      ctx->rep->entries_bad++;
      priv_check_fault(ctx, k_ra8_fs_check_fault_bad_dir_entry, 0U, lba, off);
      return k_ra8_ok;
    }
    if (e != k_ra8_ok) {
      return e;
    }
  }
  priv_exchk_verify_set(ctx, set, count, lba, off);
  return priv_exchk_set_clusters(ctx, set, file, stack, lba, off);
}

/**
 * @brief Walk one exFAT directory, verifying and marking every live entry.
 *
 * @details Iterates a directory entry stream, dispatching system, file and directory sets.
 *
 * @param[in,out] ctx   The scan context.
 * @param[in]     dir   The directory to walk.
 * @param[in,out] stack The directory worklist.
 * @return Error code.
 * @retval k_ra8_ok    The directory was walked.
 * @retval k_ra8_err_* Backend read failure.
 * @pre Every pointer is non-NULL; the mount is exFAT.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every referenced cluster it names is marked; every set it holds verified.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): `k_exfat_scan_limit` entries.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exchk_scan_dir(ra8_fs_check_ctx_t* ctx, const exfat_dir_t* dir, exfat_dir_stack_t* stack)
{
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(dir, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t         e[k_exfat_entry_bytes] = {};
    const ra8_err_t r                      = priv_exfat_next_entry(ctx->m, &cur, e);
    if (r == k_ra8_err_not_found) {
      /* GCOVR_EXCL_START -- a directory run ending exactly on a cluster edge */
      return k_ra8_ok; /* run ended without an end-of-directory marker */
      /* GCOVR_EXCL_STOP */
    }
    if (r != k_ra8_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_ok;
    }
    if ((e[0] & (uint8_t)k_exfat_inuse_bit) == 0U) {
      continue; /* a deleted remnant */
    }
    if ((e[0] == (uint8_t)k_exfat_entry_bitmap) || (e[0] == (uint8_t)k_exfat_entry_upcase)) {
      priv_exchk_system_run(ctx, e);
      continue;
    }
    if (e[0] == (uint8_t)k_exfat_entry_file) {
      const ra8_err_t se = priv_exchk_set(ctx, &cur, e, stack);
      if (se != k_ra8_ok) {
        return se;
      }
    }
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE -- k_exfat_scan_limit (65536) entries required */
}

/**
 * @brief Walk the whole exFAT directory tree from the root with a worklist.
 *
 * @details Pops directories off the worklist, marking each and scanning its entries.
 *
 * @param[in,out] ctx The scan context.
 * @return Error code.
 * @retval k_ra8_ok    The tree was walked (faults may have been recorded).
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p ctx and its bitmap are non-NULL; the mount is exFAT.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post Every referenced cluster is marked; every entry set verified.
 * @post No volume state is modified.
 * @note Bounded loop (NASA Rule 2): at most ::k_ra8_fs_check_max_dirs pops.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_tree(ra8_fs_check_ctx_t* ctx)
{
  /* Explicit DFS worklist (~2 KiB) kept in module-static storage so this frame
   * stays within the stack-usage budget; the walk is iterative (no recursion)
   * and single-threaded under the fs lock, so the shared buffer never overlaps. */
  static exfat_dir_stack_t s_worklist;
  s_worklist.top       = 0U;
  s_worklist.truncated = 0U;
  priv_exfat_dir_root(ctx->m, &s_worklist.items[0]);
  s_worklist.top = 1U;
  while (s_worklist.top > 0U) {
    s_worklist.top--;
    const exfat_dir_t dir = s_worklist.items[s_worklist.top];
    ctx->rep->dirs_visited++;
    ra8_err_t err = priv_exchk_mark_dir_alloc(ctx, &dir);
    if (err != k_ra8_ok) {
      return err;
    }
    err = priv_exchk_scan_dir(ctx, &dir, &s_worklist);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Allocation-bitmap population count and diff
 * =============================================================================
 */

/**
 * @brief Diff one allocation-bitmap byte against the visited bitmap.
 *
 * @details For each of the byte's clusters: allocated-but-unvisited is a lost
 *          cluster, and visited-but-unallocated is a bitmap mismatch. Run only in
 *          the reference mode; the population count is done by the caller.
 *
 * @param[in,out] ctx  The scan context.
 * @param[in]     b    The (tail-masked) allocation-bitmap byte.
 * @param[in]     base Cluster index of bit 0 of @p b.
 *
 * @return Nothing.
 *
 * @pre @p ctx and its bitmap are non-NULL.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post `clusters_lost` and `bitmap_mismatches` reflect this byte's disagreements.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): ::k_exchk_bits_per_byte iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exchk_diff_byte(ra8_fs_check_ctx_t* ctx, uint8_t b, uint32_t base)
{
  for (uint32_t j = 0U; j < (uint32_t)k_exchk_bits_per_byte; j++) {
    const uint32_t idx = base + j;
    if (idx >= ctx->rep->clusters_total) {
      return;
    }
    const uint8_t alloc = (uint8_t)(((uint32_t)b >> j) & 1U);
    const uint8_t refd =
      (uint8_t)(((uint32_t)ctx->bitmap[idx >> k_exchk_byte_shift] >> (idx & k_exchk_bit_mask)) &
                1U);
    if ((alloc != 0U) && (refd == 0U)) {
      ctx->rep->clusters_lost++;
      priv_check_fault(ctx,
                       k_ra8_fs_check_fault_lost_cluster,
                       idx + (uint32_t)k_cluster_first_data,
                       0U,
                       0U);
    }
    if ((alloc == 0U) && (refd != 0U)) {
      ctx->rep->bitmap_mismatches++;
      priv_check_fault(ctx,
                       k_ra8_fs_check_fault_bitmap_ref_unset,
                       idx + (uint32_t)k_cluster_first_data,
                       0U,
                       0U);
    }
  }
}

/**
 * @brief Read the allocation bitmap: count used / free, and diff if referencing.
 *
 * @details Streams the allocation bitmap sector by sector (masking the tail byte
 *          to the last cluster), summing the set bits into `clusters_used` and,
 *          when a visited bitmap is present, diffing every bit against it.
 *
 * @param[in,out] ctx     The scan context.
 * @param[in]     bmp_lba First (volume-relative) LBA of the allocation bitmap.
 *
 * @return Error code.
 * @retval k_ra8_ok    The bitmap was read.
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p ctx is non-NULL; `bmp_lba` locates the allocation bitmap.
 * @pre No filesystem operation runs concurrently on the mount (single-threaded by contract).
 * @post `clusters_used` + `clusters_free` == `clusters_total`.
 * @post In reference mode, `clusters_lost` / `bitmap_mismatches` are populated.
 *
 * @note Bounded loop (NASA Rule 2): `clusters_total / 8 + 1` bytes.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exchk_bitmap_pass(ra8_fs_check_ctx_t* ctx, uint32_t bmp_lba)
{
  const uint32_t total                          = ctx->rep->clusters_total;
  const uint32_t full_bytes                     = total >> (uint32_t)k_exchk_byte_shift;
  const uint32_t rem_bits                       = total & (uint32_t)k_exchk_bit_mask;
  const uint32_t nbytes                         = full_bytes + ((rem_bits != 0U) ? 1U : 0U);
  uint32_t       used                           = 0U;
  uint32_t       loaded                         = UINT32_MAX;
  uint8_t        sec[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t bi = 0U; bi < nbytes; bi++) {
    const uint32_t lba = bmp_lba + (bi / (uint32_t)k_ra8_fs_bytes_per_sector);
    if (lba != loaded) {
      const ra8_err_t err = priv_read_sector(ctx->m, lba, sec);
      if (err != k_ra8_ok) {
        return err;
      }
      loaded = lba;
    }
    uint8_t b = sec[bi % (uint32_t)k_ra8_fs_bytes_per_sector];
    if (bi == full_bytes) {
      b = (uint8_t)(b & (uint8_t)((1U << rem_bits) - 1U));
    }
    used += priv_exchk_popcount8(b);
    if (ctx->bitmap != nullptr) {
      priv_exchk_diff_byte(ctx, b, bi << (uint32_t)k_exchk_byte_shift);
    }
  }
  ctx->rep->clusters_used = used;
  ctx->rep->clusters_free = total - used;
  return k_ra8_ok;
}

/* `priv_check_exfat()`: see header for the documented contract. */
ra8_err_t priv_check_exfat(ra8_fs_check_ctx_t* ctx)
{
  uint32_t        bclus = 0U;
  uint32_t        blen  = 0U;
  const ra8_err_t fe    = priv_exfat_find_bitmap(ctx->m, &bclus, &blen);
  if (fe == k_ra8_err_not_found) {
    ctx->rep->entries_bad++;
    priv_check_fault(ctx, k_ra8_fs_check_fault_bad_dir_entry, 0U, 0U, 0U);
    return k_ra8_ok;
  }
  if (fe != k_ra8_ok) {
    return fe;
  }
  const uint32_t bmp_lba = priv_cluster_to_lba(ctx->m, bclus);
  if (ctx->bitmap != nullptr) {
    priv_check_zero_bitmap(ctx);
    const ra8_err_t te = priv_exchk_tree(ctx);
    if (te != k_ra8_ok) {
      return te;
    }
  }
  return priv_exchk_bitmap_pass(ctx, bmp_lba);
}
