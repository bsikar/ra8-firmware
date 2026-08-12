/**
 * @file fs_exfat_dir_test_util.h
 * @brief Shared fixture for the exFAT directory (`mkdir` / `rmdir`) suites.
 *
 * @details
 * Builds on `fs_fat_exfat_mutate_test_util.h` (the RAM-backed 64 MiB exFAT
 * volume, the raw entry patchers and the allocation-bitmap census) and adds the
 * two things directory work needs.
 *
 * **A structural scanner** -- ::exfat_check. It walks the whole directory TREE
 * from the root, and for every live entry set it verifies the SetChecksum and
 * the Stream entry's NameHash, marks the clusters the set claims, and descends
 * into it when the Directory attribute is set. Afterwards it compares the map
 * of referenced clusters against the allocation bitmap in both directions:
 *
 * - a cluster the bitmap calls used that no entry set references is an ORPHAN
 *   (leaked space -- exactly what a repeated create used to produce, #603, and
 *   what a `rmdir` that freed nothing would produce now);
 * - a cluster an entry set references that the bitmap calls free is a
 *   DANGLING reference (the reverse fault: a set left pointing at space the
 *   volume may hand out again).
 *
 * Asked in C, so every scenario is checked on every run of every host, on a
 * machine that may have no exFAT tools installed at all. The scanner is proved
 * to fire by a deliberate negative control
 * (`test_scanner_catches_orphaned_entry_set`), because a checker that has
 * quietly stopped checking reports a clean tree too.
 *
 * It is also not redundant with the real tool. `fsck.exfat` (exfatprogs 1.2.0)
 * validates what it can REACH from the root, so a cluster the bitmap calls used
 * that no entry set references is invisible to it: hand-retiring a whole entry
 * set while leaving its bitmap bits alone still reports `clean`. That leak is
 * exactly the #603 failure mode, and catching it is this scanner's job.
 *
 * **An image dumper** -- ::exfat_dump. With `RA8_FS_EXFAT_DUMP` set to a
 * directory, every scenario writes the volume out as `<tag>.img`, sliced from
 * `partition_base_lba` so the file is a bare exFAT volume rather than a
 * partitioned disk, which is what `fsck.exfat` expects::
 *
 *     RA8_FS_EXFAT_DUMP=/tmp/imgs ./test_ra8_fs_exfat_dirs
 *     for f in /tmp/imgs/ *.img; do fsck.exfat -n "$f" || echo "DIRTY: $f"; done
 *
 * Unset (the default, and what CI runs) it costs one `getenv` per scenario.
 *
 * Everything here has internal linkage, so each including test executable owns
 * a private copy of the fixture state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_exfat_upcase_test_util.h"
#include "support/fs_fat_exfat_mutate_test_util.h"
#include "unity_minimal.h"

/**
 * @enum chk_const_t
 * @brief Offsets, type tags and caps used by the structural scanner.
 *
 * @details All magic numbers the scanner would otherwise carry appear here as
 *          named values, per the repository's no-magic-numbers rule.
 *
 * @invariant k_chk_pending_cap bounds how many directories may be pending.
 * @see exfat_check()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_chk_type_eod       = 0x00U,       /**< End-of-directory. */
  k_chk_type_bitmap    = 0x81U,       /**< Bitmap entry.     */
  k_chk_type_upcase    = 0x82U,       /**< Up-case entry.    */
  k_chk_type_label     = 0x83U,       /**< Label entry.      */
  k_chk_inuse_bit      = 0x80U,       /**< Live-entry bit.   */
  k_chk_off_secnt      = 1U,          /**< SecondaryCount.   */
  k_chk_off_csum       = 2U,          /**< SetChecksum.      */
  k_chk_off_attr       = 4U,          /**< FileAttributes.   */
  k_chk_off_strm_flags = 1U,          /**< SecondaryFlags.   */
  k_chk_off_strm_nlen  = 3U,          /**< NameLength.       */
  k_chk_off_strm_hash  = 4U,          /**< NameHash.         */
  k_chk_off_name_chars = 2U,          /**< First name unit.  */
  k_chk_name_per_entry = 15U,         /**< Units per entry.  */
  k_chk_attr_directory = 0x10U,       /**< Directory bit.    */
  k_chk_nofat_bit      = 0x02U,       /**< NoFatChain bit.   */
  k_chk_csum_hi_bit    = 0x8000U,     /**< Rotate-add wrap.  */
  k_chk_pending_cap    = 512U,        /**< Pending dirs cap. */
  k_chk_list_cap       = 32U,         /**< Listing cap.      */
  k_chk_name_cap       = 80U,         /**< Name buffer.      */
  k_chk_msg_cap        = 160U,        /**< Message buffer.   */
  k_chk_entries_cap    = 65536U,      /**< Entry-scan bound. */
  k_chk_eoc_min        = 0xFFFFFFF8U, /**< End-of-chain.     */
} chk_const_t;

/**
 * @struct exfat_chk_t
 * @brief Scanner state: the referenced-cluster map plus the first diagnostic.
 *
 * @details `seen` holds one byte per heap cluster, indexed from cluster 2, and
 *          counts how many live entry sets claim it -- so a second claim is a
 *          cross-link and is reported as one.
 *
 * @invariant `ok` is 0 only when `msg` holds a non-empty diagnostic.
 * @see exfat_check()
 * @since 0.1.0
 */
typedef struct {
  uint8_t* seen;               /**< Per-cluster reference count (index = clus - 2). */
  uint32_t clusters;           /**< Length of `seen`.                               */
  uint8_t  ok;                 /**< 1 while no fault has been recorded.             */
  char     msg[k_chk_msg_cap]; /**< First fault found, for the failure message.     */
} exfat_chk_t;

/**
 * @brief Absolute byte offset of a cluster's first sector in the RAM disk.
 *
 * @details Partition-adjusted for the same reason as `root_byte()`: every LBA
 *          cached in the mount is partition-relative, and the formatter lays the
 *          volume down inside an MBR partition.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number (>= 2).
 *
 * @return Absolute byte offset within `s_disk.bytes`.
 * @retval 0..UINT32_MAX The offset.
 *
 * @pre @p h is non-NULL and mounted; @p clus is a heap cluster.
 * @pre `s_disk.bytes` holds the image.
 * @post No state is modified.
 * @post The result addresses a sector inside the cluster heap.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static inline uint32_t clus_byte(const ra8_fs_mount_t* h, uint32_t clus)
{
  const uint32_t lba = h->partition_base_lba + h->first_data_lba +
                       ((uint64_t)(clus - (uint32_t)k_mut_cluster_first) * h->sectors_per_cluster);
  return lba * (uint32_t)k_mut_block_size;
}

/**
 * @brief Byte offset of directory entry @p idx within cluster @p clus.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Directory cluster.
 * @param[in] idx  Entry index within that cluster.
 *
 * @return Absolute byte offset within `s_disk.bytes`.
 * @retval 0..UINT32_MAX The offset.
 *
 * @pre @p h is non-NULL and mounted; @p idx fits the cluster.
 * @pre `s_disk.bytes` holds the image.
 * @post No state is modified.
 * @post The result addresses a 32-byte entry.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static inline uint32_t dir_entry_byte(const ra8_fs_mount_t* h, uint32_t clus, uint32_t idx)
{
  return clus_byte(h, clus) + (idx * (uint32_t)k_mut_entry_bytes);
}

/**
 * @brief Entries one cluster of this volume holds.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Directory entries per cluster.
 * @retval 1..UINT32_MAX The count.
 *
 * @pre @p h is non-NULL and mounted.
 * @pre `h->sectors_per_cluster` is non-zero.
 * @post No state is modified.
 * @post The result is a multiple of 16 (512 / 32).
 *
 * @note Pure function.
 * @since 0.1.0
 */
static inline uint32_t entries_per_cluster(const ra8_fs_mount_t* h)
{
  return (h->sectors_per_cluster * (uint32_t)k_mut_block_size) / (uint32_t)k_mut_entry_bytes;
}

/**
 * @brief One step of exFAT's rotate-right-then-add 16-bit checksum.
 *
 * @details Reimplemented here rather than reached into the library for, so the
 *          scanner is an INDEPENDENT check: a driver whose checksum routine was
 *          wrong would agree with itself.
 *
 * @param[in] cs Running checksum.
 * @param[in] b  Next byte.
 *
 * @return Updated checksum.
 * @retval 0..0xFFFF The folded value.
 *
 * @pre @p cs is the prior running value.
 * @pre None on @p b.
 * @post No state is modified.
 * @post The result depends only on the inputs.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static inline uint16_t chk_csum_step(uint16_t cs, uint8_t b)
{
  const uint16_t hi = ((cs & 1U) != 0U) ? (uint16_t)k_chk_csum_hi_bit : (uint16_t)0U;
  return (uint16_t)(hi + (uint16_t)(cs >> 1U) + (uint16_t)b);
}

/**
 * @brief Record the scanner's first fault.
 *
 * @param[in,out] c   Scanner state.
 * @param[in]     msg Diagnostic text.
 *
 * @pre @p c and @p msg are non-NULL.
 * @pre @p msg is NUL-terminated.
 * @post `c->ok` is 0.
 * @post `c->msg` holds the FIRST fault, later ones are dropped.
 *
 * @since 0.1.0
 */
static inline void chk_fail(exfat_chk_t* c, const char* msg)
{
  if (c->ok == 0U) {
    return;
  }
  c->ok = 0U;
  (void)snprintf(c->msg, sizeof(c->msg), "%s", msg);
}

/**
 * @brief Claim one cluster for the entry set being scanned.
 *
 * @details Bumps the per-cluster reference count and reports a second claim as
 *          a cross-link, which is how two entry sets sharing a run would show up.
 *
 * @param[in,out] c    Scanner state.
 * @param[in]     clus Cluster claimed.
 *
 * @pre @p c is non-NULL with a `seen` map allocated.
 * @pre @p clus is a heap cluster.
 * @post The cluster's reference count is incremented.
 * @post A count above 1, or an out-of-range cluster, records a fault.
 *
 * @since 0.1.0
 */
static inline void chk_claim(exfat_chk_t* c, uint32_t clus)
{
  if (clus < (uint32_t)k_mut_cluster_first) {
    chk_fail(c, "entry set claims a cluster below the heap");
    return;
  }
  const uint32_t idx = clus - (uint32_t)k_mut_cluster_first;
  if (idx >= c->clusters) {
    chk_fail(c, "entry set claims a cluster past the end of the volume");
    return;
  }
  if (c->seen[idx] != 0U) {
    chk_fail(c, "two entry sets claim the same cluster (cross-link)");
    return;
  }
  c->seen[idx] = 1U;
}

/**
 * @brief Claim every cluster of a run or chain of @p bytes bytes.
 *
 * @details A NoFatChain run is ceil(bytes / cluster bytes) adjacent clusters; a
 *          FAT-chained one is walked through the FAT. Mirrors what the driver
 *          itself frees, so a mismatch between the two shows up as an orphan or
 *          a dangling reference in the final comparison.
 *
 * @param[in,out] c     Scanner state.
 * @param[in]     h     Mounted exFAT volume.
 * @param[in]     first First cluster of the run.
 * @param[in]     bytes DataLength recorded for it.
 * @param[in]     nofat 1 when the run is contiguous.
 *
 * @pre @p c and @p h are non-NULL.
 * @pre @p first is a heap cluster.
 * @post Every cluster of the run is claimed.
 * @post The walk is bounded by the volume's cluster count.
 *
 * @since 0.1.0
 */
static inline void chk_claim_run(exfat_chk_t*          c,
                                 const ra8_fs_mount_t* h,
                                 uint32_t              first,
                                 uint32_t              bytes,
                                 uint8_t               nofat)
{
  if (bytes == 0U) {
    return;
  }
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t count  = (bytes + cbytes - 1U) / cbytes;
  if (nofat != 0U) {
    for (uint32_t k = 0U; k < count; k++) {
      chk_claim(c, first + k);
    }
    return;
  }
  uint32_t clus = first;
  for (uint32_t k = 0U; k < count; k++) {
    chk_claim(c, clus);
    const uint32_t next = disk_get_u32le(fat_byte(h, clus));
    if (next >= (uint32_t)k_chk_eoc_min) {
      if ((k + 1U) != count) {
        chk_fail(c, "FAT chain ends before the recorded DataLength");
      }
      return;
    }
    clus = next;
  }
}

/**
 * @brief Fold the specification's SetChecksum over a whole entry set.
 *
 * @details Every byte of the set except the two SetChecksum bytes themselves,
 *          rotate-added in order. Computed from the IMAGE, independently of the
 *          library, so a writer whose own checksum routine was wrong would not
 *          agree with itself here.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] clus  Directory cluster holding the set.
 * @param[in] idx   Entry index of the File entry.
 * @param[in] total Entries the set occupies.
 *
 * @return The folded checksum.
 * @retval 0..0xFFFF The value the File entry should carry.
 *
 * @pre @p h is non-NULL; the set lies inside @p clus.
 * @pre @p total is 1 + SecondaryCount.
 * @post No state is modified.
 * @post The fold visits every byte of the set exactly once.
 *
 * @since 0.1.0
 */
static inline uint16_t
chk_set_checksum(const ra8_fs_mount_t* h, uint32_t clus, uint32_t idx, uint32_t total)
{
  uint16_t cs = 0U;
  for (uint32_t e = 0U; e < total; e++) {
    const uint32_t off = dir_entry_byte(h, clus, idx + e);
    for (uint32_t b = 0U; b < (uint32_t)k_mut_entry_bytes; b++) {
      const uint8_t skip_lo = (uint8_t)(((e == 0U) && (b == (uint32_t)k_chk_off_csum)) ? 1U : 0U);
      const uint8_t skip_hi =
        (uint8_t)(((e == 0U) && (b == ((uint32_t)k_chk_off_csum + 1U))) ? 1U : 0U);
      if ((skip_lo | skip_hi) != 0U) {
        continue;
      }
      cs = chk_csum_step(cs, s_disk.bytes[off + b]);
    }
  }
  return cs;
}

/**
 * @brief Fold the exFAT NameHash over the name a set stores.
 *
 * @details Walks the Name entries, up-cases each unit through the table the
 *          VOLUME carries (::upc_load expands it from the volume's own
 *          bytes) and rotate-adds both of its bytes -- the hash a host computes
 *          when it looks the name up, so a set whose stored hash disagrees is
 *          one a host would miss.
 *
 *          The fold used to up-case only 'a'..'z' of each unit's LOW byte,
 *          which matches the specification for an ASCII name and nothing else:
 *          once names could hold anything (#606), it reported every correctly
 *          hashed non-ASCII name as broken. Reading the volume's table rather
 *          than calling the driver's fold is what keeps this an independent
 *          check of the driver rather than a restatement of it.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Directory cluster holding the set.
 * @param[in] idx  Entry index of the File entry.
 * @param[in] nlen NameLength from the Stream entry, in UTF-16 units.
 *
 * @return The folded hash.
 * @retval 0..0xFFFF The value the Stream entry should carry.
 *
 * @pre @p h is non-NULL; the set lies inside @p clus.
 * @pre ::upc_load has run for this volume (::exfat_scan does it).
 * @post No state is modified.
 * @post The fold visits @p nlen units.
 *
 * @since 0.1.0
 */
static inline uint16_t
chk_name_hash(const ra8_fs_mount_t* h, uint32_t clus, uint32_t idx, uint32_t nlen)
{
  uint16_t hash = 0U;
  for (uint32_t k = 0U; k < nlen; k++) {
    const uint32_t which = k / (uint32_t)k_chk_name_per_entry;
    const uint32_t slot  = k % (uint32_t)k_chk_name_per_entry;
    const uint32_t noff  = dir_entry_byte(h, clus, idx + 2U + which);
    const uint32_t bpos  = noff + (uint32_t)k_chk_off_name_chars + (slot * 2U);
    /* Folded through the table the VOLUME carries, expanded from its own bytes
     * above -- not through the driver's fold, which is the thing this scan
     * exists to disagree with when it is wrong. Up-casing only 'a'..'z' of the
     * low byte, which is what this did, matches the specification for an ASCII
     * name and nothing else: it reported every correctly-hashed non-ASCII name
     * as broken (#605 fixture, #606). */
    const uint32_t raw  = (uint32_t)s_disk.bytes[bpos] |
                          ((uint32_t)s_disk.bytes[bpos + 1U] << (uint32_t)k_mut_shift_byte8);
    const uint32_t unit = s_upc_table[raw];
    hash                = chk_csum_step(hash, (uint8_t)(unit & (uint32_t)k_mut_mask_byte));
    hash =
      chk_csum_step(hash,
                    (uint8_t)((unit >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte));
  }
  return hash;
}

/**
 * @brief Read a 16-bit little-endian field out of the image.
 *
 * @param[in] off Absolute byte offset.
 *
 * @return The decoded value.
 * @retval 0..0xFFFF The stored value.
 *
 * @pre `off + 2` is within the image.
 * @pre `s_disk.bytes` holds the image.
 * @post No state is modified.
 * @post The result depends only on the two bytes at @p off.
 *
 * @since 0.1.0
 */
static inline uint32_t disk_get_u16le(uint32_t off)
{
  return (uint32_t)s_disk.bytes[off] |
         ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_mut_shift_byte8);
}

/**
 * @brief Verify one entry set's SetChecksum and NameHash, and claim its run.
 *
 * @details Both folds are recomputed from the image and compared with what the
 *          set stores; the set's cluster run is then claimed so the final
 *          bitmap comparison can tell referenced space from leaked space.
 *
 * @param[in,out] c        Scanner state.
 * @param[in]     h        Mounted exFAT volume.
 * @param[in]     clus     Directory cluster holding the File entry.
 * @param[in]     idx      Entry index of the File entry.
 * @param[out]    out_dir  Receives the set's first cluster when it is a
 *                         directory, else 0.
 * @param[out]    out_size Receives the set's DataLength when it is a directory.
 * @param[out]    out_flag Receives the set's GeneralSecondaryFlags.
 *
 * @return Number of entries the set occupies (1 + SecondaryCount).
 * @retval 1..20 The set length.
 *
 * @pre @p c and @p h are non-NULL; the entry at (@p clus, @p idx) is 0x85.
 * @pre The set lies entirely inside @p clus.
 * @post Faults are recorded in @p c rather than aborting the walk.
 * @post The set's clusters are claimed.
 *
 * @since 0.1.0
 */
static inline uint32_t chk_visit_set(exfat_chk_t*          c,
                                     const ra8_fs_mount_t* h,
                                     uint32_t              clus,
                                     uint32_t              idx,
                                     uint32_t*             out_dir,
                                     uint32_t*             out_size,
                                     uint8_t*              out_flag)
{
  *out_dir                = 0U;
  *out_size               = 0U;
  *out_flag               = 0U;
  const uint32_t file_off = dir_entry_byte(h, clus, idx);
  const uint32_t total    = 1U + (uint32_t)s_disk.bytes[file_off + (uint32_t)k_chk_off_secnt];
  if ((uint32_t)chk_set_checksum(h, clus, idx, total) !=
      disk_get_u16le(file_off + (uint32_t)k_chk_off_csum)) {
    chk_fail(c, "entry set SetChecksum does not cover its own bytes");
  }
  const uint32_t strm_off = dir_entry_byte(h, clus, idx + 1U);
  const uint32_t nlen     = (uint32_t)s_disk.bytes[strm_off + (uint32_t)k_chk_off_strm_nlen];
  const uint32_t first    = disk_get_u32le(strm_off + (uint32_t)k_mut_strm_off_clus);
  const uint32_t dlen     = disk_get_u32le(strm_off + (uint32_t)k_mut_strm_off_dlen);
  const uint8_t  flags    = s_disk.bytes[strm_off + (uint32_t)k_chk_off_strm_flags];
  if ((uint32_t)chk_name_hash(h, clus, idx, nlen) !=
      disk_get_u16le(strm_off + (uint32_t)k_chk_off_strm_hash)) {
    chk_fail(c, "entry set NameHash disagrees with the name it stores");
  }
  chk_claim_run(c, h, first, dlen, (uint8_t)(((flags & (uint8_t)k_chk_nofat_bit) != 0U) ? 1U : 0U));
  if ((s_disk.bytes[file_off + (uint32_t)k_chk_off_attr] & (uint8_t)k_chk_attr_directory) != 0U) {
    *out_dir  = first;
    *out_size = dlen;
    *out_flag = flags;
  }
  return total;
}

/**
 * @brief Scan one directory cluster, claiming sets and stacking subdirectories.
 *
 * @details Inner half of ::chk_walk_dir. Visits every live File entry set in
 *          the cluster and pushes each subdirectory onto @p stack; stops at the
 *          end-of-directory marker.
 *
 * @param[in,out] c     Scanner state.
 * @param[in]     h     Mounted exFAT volume.
 * @param[in]     clus  Cluster to scan.
 * @param[in,out] stack Pending-directory stack (first cluster, size, nofat).
 * @param[in,out] depth Stack depth, updated in place.
 *
 * @return 1 when the walk should stop here, else 0.
 * @retval 1 End-of-directory reached, or the stack overflowed.
 * @retval 0 The cluster was exhausted; keep walking.
 *
 * @pre @p c, @p h, @p stack and @p depth are non-NULL.
 * @pre @p clus belongs to the directory being walked.
 * @post Every live set in the cluster has been visited.
 * @post Overflowing @p stack records a fault instead of writing past it.
 *
 * @since 0.1.0
 */
static inline uint8_t chk_walk_cluster(exfat_chk_t*          c,
                                       const ra8_fs_mount_t* h,
                                       uint32_t              clus,
                                       uint32_t              stack[][3],
                                       uint32_t*             depth)
{
  const uint32_t per = entries_per_cluster(h);
  uint32_t       idx = 0U;
  while (idx < per) {
    const uint32_t off  = dir_entry_byte(h, clus, idx);
    const uint8_t  type = s_disk.bytes[off];
    if (type == (uint8_t)k_chk_type_eod) {
      return 1U;
    }
    if (type != (uint8_t)k_mut_type_file) {
      if ((type == (uint8_t)k_chk_type_bitmap) || (type == (uint8_t)k_chk_type_upcase)) {
        chk_claim_run(c,
                      h,
                      disk_get_u32le(off + (uint32_t)k_mut_strm_off_clus),
                      disk_get_u32le(off + (uint32_t)k_mut_strm_off_dlen),
                      0U);
      }
      idx++;
      continue;
    }
    uint32_t sub   = 0U;
    uint32_t ssize = 0U;
    uint8_t  sflag = 0U;
    idx += chk_visit_set(c, h, clus, idx, &sub, &ssize, &sflag);
    if (sub == 0U) {
      continue;
    }
    if (*depth >= (uint32_t)k_chk_pending_cap) {
      chk_fail(c, "more directories pending than the scanner can hold");
      return 1U;
    }
    stack[*depth][0] = sub;
    stack[*depth][1] = ssize;
    stack[*depth][2] = ((sflag & (uint8_t)k_chk_nofat_bit) != 0U) ? 1U : 0U;
    (*depth)++;
  }
  return 0U;
}

/**
 * @brief Step to the next cluster of a directory being scanned.
 *
 * @details A contiguous run steps to the adjacent cluster and stops at
 *          @p run_end; a FAT-chained one follows the FAT and stops at
 *          end-of-chain. The scanner's own copy of the rule the driver applies,
 *          so a driver that got it wrong would disagree with this walk.
 *
 * @param[in]  h        Mounted exFAT volume.
 * @param[in]  clus     Current cluster.
 * @param[in]  run_end  One past the run's last cluster; 0 => follow the FAT.
 * @param[out] out_next Receives the successor cluster.
 *
 * @return 1 when there is a next cluster, else 0.
 * @retval 1 ``*out_next`` is the next cluster.
 * @retval 0 The directory ends here.
 *
 * @pre @p h and @p out_next are non-NULL.
 * @pre @p clus belongs to the directory being walked.
 * @post No state is modified.
 * @post On 0 the caller stops walking.
 *
 * @since 0.1.0
 */
static inline uint8_t
chk_next_cluster(const ra8_fs_mount_t* h, uint32_t clus, uint32_t run_end, uint32_t* out_next)
{
  if (run_end != 0U) {
    const uint32_t adjacent = clus + 1U;
    if (adjacent >= run_end) {
      return 0U;
    }
    *out_next = adjacent;
    return 1U;
  }
  const uint32_t next = disk_get_u32le(fat_byte(h, clus));
  if (next >= (uint32_t)k_chk_eoc_min) {
    return 0U;
  }
  *out_next = next;
  return 1U;
}

/**
 * @brief Walk one directory, claiming its sets and stacking its subdirectories.
 *
 * @details Scans cluster by cluster -- adjacent clusters for a NoFatChain run,
 *          the FAT chain otherwise -- until an end-of-directory marker or the
 *          end of the run.
 *
 * @param[in,out] c     Scanner state.
 * @param[in]     h     Mounted exFAT volume.
 * @param[in]     first The directory's first cluster.
 * @param[in]     bytes The directory's DataLength (0 => FAT-chained root).
 * @param[in]     nofat 1 when the directory's run is contiguous.
 * @param[in,out] stack Pending-directory stack (first cluster, size, nofat).
 * @param[in,out] depth Stack depth, updated in place. The stack holds the
 *                      BREADTH as well as the depth of the tree, because every
 *                      subdirectory found in one pass is pushed before any of
 *                      them is walked.
 *
 * @pre @p c, @p h, @p stack and @p depth are non-NULL.
 * @pre @p first is a heap cluster.
 * @post Every live set in the directory has been visited.
 * @post The walk is bounded by ::k_chk_entries_cap clusters.
 *
 * @since 0.1.0
 */
static inline void chk_walk_dir(exfat_chk_t*          c,
                                const ra8_fs_mount_t* h,
                                uint32_t              first,
                                uint32_t              bytes,
                                uint8_t               nofat,
                                uint32_t              stack[][3],
                                uint32_t*             depth)
{
  const uint32_t cbytes  = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t run_end = (nofat != 0U) ? (first + ((bytes + cbytes - 1U) / cbytes)) : 0U;
  uint32_t       clus    = first;
  for (uint32_t guard = 0U; guard < (uint32_t)k_chk_entries_cap; guard++) {
    if (chk_walk_cluster(c, h, clus, stack, depth) != 0U) {
      return;
    }
    uint32_t next = 0U;
    if (chk_next_cluster(h, clus, run_end, &next) == 0U) {
      return;
    }
    clus = next;
  }
}

/**
 * @brief Claim the root directory's own cluster chain.
 *
 * @details The root is referenced by the boot sector, not by any entry set, so
 *          nothing in the tree walk would claim it -- and the final comparison
 *          would then call every cluster of it leaked space. Claims the WHOLE
 *          chain, however long, so a root that has grown reads back correctly.
 *
 * @param[in,out] c Scanner state.
 * @param[in]     h Mounted exFAT volume.
 *
 * @pre @p c and @p h are non-NULL; `c->seen` is allocated.
 * @pre `s_disk.bytes` holds the image.
 * @post Every cluster of the root's chain is claimed.
 * @post The walk is bounded by the volume's cluster count.
 *
 * @since 0.1.0
 */
static inline void chk_claim_root(exfat_chk_t* c, const ra8_fs_mount_t* h)
{
  uint32_t clus = h->root_cluster;
  for (uint32_t k = 0U; k < c->clusters; k++) {
    chk_claim(c, clus);
    uint32_t next = 0U;
    if (chk_next_cluster(h, clus, 0U, &next) == 0U) {
      return;
    }
    clus = next;
  }
}

/**
 * @brief Compare the claimed clusters against the allocation bitmap.
 *
 * @details Both directions, because each names a different fault: a cluster the
 *          bitmap calls used that nothing references is leaked space, and one an
 *          entry set references that the bitmap calls free is a live reference
 *          into space the volume may hand out again.
 *
 * @param[in,out] c Scanner state, with the tree walk already done.
 * @param[in]     h Mounted exFAT volume.
 *
 * @pre @p c and @p h are non-NULL; the tree walk has completed.
 * @pre The allocation bitmap is contiguous on disk.
 * @post Any disagreement is recorded in @p c.
 * @post No on-disk state is modified.
 *
 * @since 0.1.0
 */
static inline void chk_compare_bitmap(exfat_chk_t* c, const ra8_fs_mount_t* h)
{
  const uint32_t bmp_entry = root_byte(h, (uint32_t)k_mut_root_bitmap_idx);
  const uint32_t bmp_clus  = disk_get_u32le(bmp_entry + (uint32_t)k_mut_strm_off_clus);
  const uint32_t bmp_base  = clus_byte(h, bmp_clus);
  for (uint32_t i = 0U; i < c->clusters; i++) {
    const uint8_t byte = s_disk.bytes[bmp_base + (i / (uint32_t)k_mut_bits_per_byte)];
    const uint8_t used = (uint8_t)((byte >> (i % (uint32_t)k_mut_bits_per_byte)) & 1U);
    if ((used != 0U) && (c->seen[i] == 0U)) {
      chk_fail(c, "a cluster is marked used but no entry set references it (orphan)");
    }
    if ((used == 0U) && (c->seen[i] != 0U)) {
      chk_fail(c, "an entry set references a cluster the bitmap calls free (dangling)");
    }
  }
}

/**
 * @brief Run the whole structural check and REPORT the verdict.
 *
 * @details Walks the directory tree from the root, then compares the referenced
 *          clusters against the allocation bitmap in both directions. Reports
 *          rather than asserts, so a test can assert the scanner FIRES on a
 *          deliberately damaged volume -- the only way to know it is still
 *          checking anything.
 *
 * @param[in]  h   Mounted exFAT volume.
 * @param[out] msg Receives the first diagnostic (empty when clean).
 * @param[in]  cap Capacity of @p msg.
 *
 * @return 1 when the volume is structurally sound, else 0.
 * @retval 1 Every entry set checks out and the bitmap agrees exactly.
 * @retval 0 A checksum, hash, orphan, dangling or cross-link fault was found.
 *
 * @pre @p h and @p msg are non-NULL; `s_disk.bytes` holds the image.
 * @pre @p cap is at least 1.
 * @post No on-disk state is modified.
 * @post @p msg is NUL-terminated.
 *
 * @since 0.1.0
 */
static inline uint8_t exfat_scan(const ra8_fs_mount_t* h, char* msg, uint32_t cap)
{
  upc_load(h); /* the volume's own fold, before anything is hashed */
  exfat_chk_t c = {};
  c.clusters    = h->count_of_clusters;
  c.ok          = 1U;
  c.seen        = (uint8_t*)calloc((size_t)c.clusters, 1U);
  if (c.seen == nullptr) {
    (void)snprintf(msg, (size_t)cap, "%s", "calloc failed for the scanner cluster map");
    return 0U;
  }
  chk_claim_root(&c, h);
  uint32_t stack[k_chk_pending_cap][3] = {};
  uint32_t depth                       = 0U;
  chk_walk_dir(&c, h, h->root_cluster, 0U, 0U, stack, &depth);
  while (depth > 0U) {
    depth--;
    const uint32_t first = stack[depth][0];
    const uint32_t size  = stack[depth][1];
    const uint8_t  nofat = (uint8_t)stack[depth][2];
    chk_walk_dir(&c, h, first, size, nofat, stack, &depth);
  }
  chk_compare_bitmap(&c, h);
  const uint8_t ok = c.ok;
  (void)snprintf(msg, (size_t)cap, "%s", c.msg);
  free(c.seen);
  return ok;
}

/**
 * @brief Assert the mounted volume is structurally sound.
 *
 * @details The assertion wrapper around ::exfat_scan that every scenario ends
 *          with. Kept separate so the scanner's own negative control can call
 *          ::exfat_scan and require a FAILURE.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] tag Scenario name, used in the failure message.
 *
 * @pre @p h is non-NULL and mounted; `s_disk.bytes` holds the image.
 * @pre @p tag is NUL-terminated.
 * @post No on-disk state is modified.
 * @post The calling test has failed if the volume is structurally unsound.
 *
 * @since 0.1.0
 */
static inline void exfat_check(const ra8_fs_mount_t* h, const char* tag)
{
  char msg[k_chk_msg_cap] = {};
  if (exfat_scan(h, msg, (uint32_t)k_chk_msg_cap) == 0U) {
    TEST_FAIL_FMT("exfat_check(%s): %s", tag, msg);
  }
}

/**
 * @brief Write the volume out as a bare exFAT image when dumping is enabled.
 *
 * @details Slices from `partition_base_lba` so the file is the exFAT volume
 *          itself, which is what `fsck.exfat` expects -- pointed at the whole
 *          RAM disk it would see an MBR and refuse. Does nothing unless
 *          `RA8_FS_EXFAT_DUMP` names a directory.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] tag Scenario name; becomes `<tag>.img`.
 *
 * @pre @p h is non-NULL and mounted; @p tag is NUL-terminated.
 * @pre `s_disk.bytes` holds the image.
 * @post With dumping disabled nothing is written and nothing fails.
 * @post With it enabled the named file holds the volume's bytes.
 *
 * @since 0.1.0
 */
static inline void exfat_dump(const ra8_fs_mount_t* h, const char* tag)
{
  const char* dir = getenv("RA8_FS_EXFAT_DUMP");
  if (dir == nullptr) {
    return;
  }
  char path[k_chk_msg_cap];
  (void)snprintf(path, sizeof(path), "%s/%s.img", dir, tag);
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    TEST_FAIL_FMT("could not open dump file %s", path);
    return;
  }
  const size_t base  = (size_t)h->partition_base_lba * (size_t)k_mut_block_size;
  const size_t total = (size_t)s_disk.block_count * (size_t)k_mut_block_size;
  (void)fwrite(&s_disk.bytes[base], 1U, total - base, f);
  (void)fclose(f);
}

/**
 * @brief Structural check plus image dump, the pair every scenario ends with.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] tag Scenario name.
 *
 * @pre @p h is non-NULL and mounted; @p tag is NUL-terminated.
 * @pre `s_disk.bytes` holds the image.
 * @post No on-disk state is modified.
 * @post The calling test has failed if the volume is structurally unsound.
 *
 * @since 0.1.0
 */
static inline void exfat_verify(const ra8_fs_mount_t* h, const char* tag)
{
  exfat_check(h, tag);
  exfat_dump(h, tag);
}

/**
 * @struct name_ctx_t
 * @brief Listdir context that records the names and attributes it is handed.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t count;                                 /**< Entries reported.         */
  char     names[k_chk_list_cap][k_chk_name_cap]; /**< Names, in order.          */
  uint8_t  attrs[k_chk_list_cap];                 /**< Matching attribute bytes. */
  uint32_t sizes[k_chk_list_cap];                 /**< Matching sizes.           */
} name_ctx_t;

/**
 * @brief Listdir callback that records name, attribute and size.
 *
 * @param[in] name Entry name.
 * @param[in] attr Entry attribute byte.
 * @param[in] size Entry size.
 * @param[in] ctx  Pointer to a ::name_ctx_t.
 *
 * @pre @p name and @p ctx are non-NULL.
 * @pre The context has room (asserted by the cap).
 * @post The entry is appended to the context.
 * @post Entries past the cap are counted but not stored.
 *
 * @since 0.1.0
 */
static inline void name_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  name_ctx_t* n = (name_ctx_t*)ctx;
  if (n->count < (uint32_t)k_chk_list_cap) {
    (void)snprintf(n->names[n->count], (size_t)k_chk_name_cap, "%s", name);
    n->attrs[n->count] = attr;
    n->sizes[n->count] = size;
  }
  n->count++;
}

/**
 * @brief List @p path and return how many entries it reported.
 *
 * @param[in]  h   Mounted exFAT volume.
 * @param[in]  path Directory path.
 * @param[out] out Receives the recorded names/attributes.
 *
 * @return The entry count.
 * @retval 0..UINT32_MAX Entries reported.
 *
 * @pre @p h, @p path and @p out are non-NULL.
 * @pre The listing succeeds (asserted).
 * @post No on-disk state is modified.
 * @post @p out holds the reported entries.
 *
 * @since 0.1.0
 */
static inline uint32_t list_names(ra8_fs_mount_t* h, const char* path, name_ctx_t* out)
{
  *out = (name_ctx_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, path, name_cb, out));
  return out->count;
}

/**
 * @brief 1 when @p dir's listing holds @p name with the directory bit set.
 *
 * @param[in] ctx Recorded listing.
 * @param[in] name Name to look for.
 *
 * @return 1 when present as a directory, else 0.
 * @retval 1 Found, with ::k_chk_attr_directory set.
 * @retval 0 Absent, or present as a file.
 *
 * @pre @p ctx and @p name are non-NULL.
 * @pre @p ctx came from ::list_names.
 * @post No state is modified.
 * @post The search visits at most `ctx->count` entries.
 *
 * @since 0.1.0
 */
static inline uint8_t has_dir(const name_ctx_t* ctx, const char* name)
{
  for (uint32_t i = 0U; i < ctx->count; i++) {
    if (strcmp(ctx->names[i], name) != 0) {
      continue;
    }
    return ((ctx->attrs[i] & (uint8_t)k_chk_attr_directory) != 0U) ? 1U : 0U;
  }
  return 0U;
}

/**
 * @brief 1 when the listing holds @p name with the directory bit CLEAR.
 *
 * @details The mirror of ::has_dir, and it exists for the same reason the
 *          directory bit is checked there rather than the name alone: a root
 *          holding both kinds is where a scan that matched the right name
 *          against the wrong entry kind would otherwise pass unnoticed.
 *
 * @param[in] ctx  Recorded listing.
 * @param[in] name Name to look for.
 *
 * @return 1 when present as a plain file, else 0.
 * @retval 1 Found, with ::k_chk_attr_directory clear.
 * @retval 0 Absent, or present as a directory.
 *
 * @pre @p ctx and @p name are non-NULL.
 * @pre @p ctx came from ::list_names.
 * @post No state is modified.
 * @post The search visits at most `ctx->count` entries.
 *
 * @since 0.1.0
 */
static inline uint8_t has_file(const name_ctx_t* ctx, const char* name)
{
  for (uint32_t i = 0U; i < ctx->count; i++) {
    if (strcmp(ctx->names[i], name) != 0) {
      continue;
    }
    return ((ctx->attrs[i] & (uint8_t)k_chk_attr_directory) == 0U) ? 1U : 0U;
  }
  return 0U;
}
