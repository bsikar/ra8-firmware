/**
 * @file ra8_fs_fat_lfn_write.c
 * @brief Writing VFAT long names: reserve the slots, build the chain, take it away again.
 *
 * @details
 * `ra8_fs_fat_lfn.c` has always been able to READ a long name -- reassemble a
 * run of attr-0x0F entries, check the checksum against the 8.3 entry behind
 * them, and match it. This file is the other direction, and with it the
 * filesystem can finally store the name the caller asked for instead of
 * refusing it: `ra8_fs_open()`, `ra8_fs_mkdir()` and `ra8_fs_rename()` used to
 * answer `k_ra8_err_invalid_arg` for anything that was not 8.3-representable.
 *
 * Three pieces, in the order a create runs them:
 *
 * 1. **An alias nothing else answers to.** A long name still needs an 8.3
 *    entry, because that is where the size, the attributes and the first
 *    cluster live. ::priv_lfn_alias_basis() derives `LONGNA~1.TXT` from
 *    `Long Name.txt`, and ::priv_alias_unique() keeps raising the tail until
 *    the directory does not already hold that name.
 * 2. **A run of consecutive free slots.** A chain is only a chain because its
 *    entries physically precede their 8.3 entry, so the slots have to be found
 *    together rather than one at a time -- and when the directory has no run
 *    that long, it grows by a cluster (a FAT12/16 root cannot, and says so).
 * 3. **The chain itself**, written back-to-front with the 0x40 flag on the
 *    physically-first entry, each slot carrying the checksum of the alias.
 *
 * The fourth piece runs in the other direction. ::priv_dir_erase_chain() is
 * what stops `unlink` and `rename` leaving ORPHANS: clearing only the 8.3
 * entry left the whole attr-0x0F run on disk with a checksum that now matched
 * nothing, which in-tree readers skip but `fsck.fat` and `chkdsk` both report.
 * It walks forward to the doomed entry keeping the run of long-name slots that
 * immediately precede it AND carry its checksum, then 0xE5s all of them
 * together with it.
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

/**
 * @enum ra8_fs_lfnw_local_t
 * @brief Bounds used only by this translation unit's directory manipulation.
 *
 * @details `k_lfnw_grow_max` is the loop bound that keeps the "no run this
 *          long -- grow and look again" retry finite (NASA Power of 10 Rule 2).
 *          Two grows always suffice: the longest run this code ever asks for
 *          is `k_lfn_erase_max + 1` = 21 slots, and the smallest cluster this
 *          driver mounts is one 512-byte sector = 16 slots, so a run can need
 *          at most one fresh cluster beyond a completely full one. The third
 *          attempt is slack, not a requirement.
 *
 * @invariant `k_lfnw_grow_max` is at least 2.
 * @see priv_dir_reserve()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lfnw_grow_max = 3U, /**< Cluster-growth retries before reporting no_mem. */
} ra8_fs_lfnw_local_t;

/* =============================================================================
 * Alias generation
 * =============================================================================
 */

/**
 * @brief Pick the lowest `~N` alias the directory does not already hold.
 *
 * @details Generates the basis name for N = 1, 2, 3 ... and looks each one up
 *          by its packed 8.3 form, stopping at the first miss. Looking the
 *          alias up (rather than only the long name) is the point: two
 *          different long names routinely produce the same basis, and an alias
 *          collision would give one file two 8.3 entries with the same name.
 *
 * @param[in]  m    Mounted FAT12/16/32 volume.
 * @param[in]  loc  Directory the entry is going into.
 * @param[in]  leaf Long name being filed, as UTF-16 code units.
 * @param[in]  n    Number of units in @p leaf.
 * @param[out] out11 Receives the packed 11-byte alias.
 *
 * @return Error code.
 * @retval k_ra8_ok         An unused alias was found; @p out11 holds it.
 * @retval k_ra8_err_no_mem Every alias in the supported range is taken.
 * @retval k_ra8_err_*      Backend read error while probing.
 *
 * @pre All pointers are non-NULL; @p out11 addresses `k_max_8_3_name` bytes.
 * @pre @p leaf is the unit array ::priv_name_classify() filled, and it reported
 *      `k_name_kind_long` for it.
 * @post On success no entry in @p loc carries the name in @p out11.
 * @post No on-disk state is modified.
 *
 * @note Bounded by ::k_lfn_alias_tail_max probes (NASA Power of 10 Rule 2).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
extern ra8_err_t internal_alias_unique(const ra8_fs_mount_t* m,
                                       const dir_loc_t*      loc,
                                       const uint16_t*       leaf,
                                       uint32_t              n,
                                       uint8_t*              out11);

/* =============================================================================
 * Free-slot runs and directory growth
 * =============================================================================
 */

/**
 * @brief Append one zeroed cluster to a directory's chain.
 *
 * @details Walks to the chain's last cluster with the shared directory
 *          iterator, allocates a fresh cluster, zeroes every sector of it so it
 *          reads as end-of-directory, and only then links it in -- a directory
 *          is never visible in a half-initialised state. A FAT12/16 volume root
 *          has no chain to extend and is refused.
 *
 * @param[in] m   Mounted FAT12/16/32 volume.
 * @param[in] loc Directory to extend.
 *
 * @return Error code.
 * @retval k_ra8_ok                 The directory is one cluster longer.
 * @retval k_ra8_err_no_mem         A FAT12/16 fixed root, or the volume is full.
 * @retval k_ra8_err_protocol_error The existing chain revisits a cluster.
 * @retval k_ra8_err_*              Backend or FAT error.
 *
 * @pre @p m and @p loc are non-NULL; @p m is a mounted FAT volume.
 * @pre @p loc names a directory that exists on @p m.
 * @post On success the new cluster is zeroed and linked at the chain's end.
 * @post On failure no cluster is left allocated but unreferenced.
 *
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dir_grow(const ra8_fs_mount_t* m, const dir_loc_t* loc)
{
  if ((loc->is_root != 0U) && (m->type != k_ra8_fs_type_fat32)) {
    return k_ra8_err_no_mem; /* the FAT12/16 root is a fixed sector region */
  }
  /* Walk to the chain's last cluster with the shared directory iterator rather
   * than a second FAT walk of this file's own: the iterator already carries
   * the cycle guard, and two walks that could disagree about where a directory
   * ends is exactly the kind of duplication that goes wrong quietly. */
  dir_walk_t w = {};
  priv_dir_walk_init_loc(m, loc, &w);
  uint32_t last = w.cluster;
  uint8_t  eod  = 0U;
  while (eod == 0U) {
    last                 = w.cluster;
    const ra8_err_t werr = priv_dir_walk_next_sector(m, &w, &eod);
    if (werr != k_ra8_ok) {
      return werr;
    }
  }
  uint32_t  fresh = 0U;
  ra8_err_t err   = priv_alloc_eoc_cluster(m, &fresh);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint64_t base = priv_cluster_to_lba(m, fresh);
  for (uint32_t s = 0U; s < m->sectors_per_cluster; s++) {
    err = priv_write_sector(m, base + s, k_zero_sector);
    if (err != k_ra8_ok) {
      (void)priv_free_chain(m, fresh);
      return err;
    }
  }
  err = priv_fat_set(m, last, fresh);
  if (err != k_ra8_ok) {
    (void)priv_free_chain(m, fresh);
    return err;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Reserve / commit
 * =============================================================================
 */

/* `priv_dir_reserve()`: see header for the documented contract. */
ra8_err_t
priv_dir_reserve(const ra8_fs_mount_t* m, const dir_loc_t* loc, const char* leaf, dir_insert_t* out)
{
  out->lfn_entries = 0U;
  out->nunits      = 0U;
  const ra8_fs_name_kind_t kind =
    priv_name_classify(leaf, out->units, &out->nunits, out->name83, &out->ntres);
  if (kind == k_name_kind_invalid) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t need = 1U;
  if (kind == k_name_kind_long) {
    const ra8_err_t aerr = internal_alias_unique(m, loc, out->units, out->nunits, out->name83);
    if (aerr != k_ra8_ok) {
      return aerr;
    }
    out->ntres = 0U; /* the chain carries the case; the alias is upper-case */
    /* Groups of THIRTEEN CODE UNITS -- the slot's capacity. Deriving this from
     * a byte count put a 2-byte character's worth of slots on a 1-unit
     * character, and the chain then disagreed with its own contents. */
    out->lfn_entries = (uint8_t)(((out->nunits + (uint32_t)k_lfn_chars_per_ent) - 1U) /
                                 (uint32_t)k_lfn_chars_per_ent);
    need             = (uint32_t)out->lfn_entries + 1U;
  }
  for (uint32_t attempt = 0U; attempt <= (uint32_t)k_lfnw_grow_max; attempt++) {
    const ra8_err_t ferr = priv_dir_find_free_run(m, loc, need, &out->start);
    if (ferr != k_ra8_err_no_mem) {
      return ferr;
    }
    if (attempt == (uint32_t)k_lfnw_grow_max) {
      break; /* growing again would only link a cluster nothing then uses */
    }
    const ra8_err_t gerr = internal_dir_grow(m, loc);
    if (gerr != k_ra8_ok) {
      return gerr;
    }
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Step a slot cursor forward one entry, flushing across sector edges.
 *
 * @details The write buffer holds one sector; crossing into the next one means
 *          committing what is in it first, because the run being written may
 *          already have modified this sector's earlier slots.
 *
 * @param[in]     m       Mounted FAT12/16/32 volume.
 * @param[in,out] cur     Cursor to advance.
 * @param[in,out] buf     Sector buffer; written out and reloaded on a crossing.
 * @param[in,out] lba_io  Holds the buffer's LBA; updated on a crossing.
 *
 * @return Error code.
 * @retval k_ra8_ok         The cursor addresses the next slot.
 * @retval k_ra8_err_no_mem The directory ended mid-run (corrupt reservation).
 * @retval k_ra8_err_*      Backend read/write error.
 *
 * @pre All pointers are non-NULL; @p buf holds the sector at `*lba_io`.
 * @pre @p cur is positioned inside the reserved run.
 * @post On success @p buf holds the sector `cur` now points into.
 * @post On failure the run is left partially written.
 *
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
extern ra8_err_t
internal_slot_advance(const ra8_fs_mount_t* m, dir_slot_t* cur, uint8_t* buf, uint64_t* lba_io);

/* `priv_dir_commit()`: see header for the documented contract. */
ra8_err_t priv_dir_commit(const ra8_fs_mount_t* m,
                          const dir_insert_t*   plan,
                          const uint8_t*        tmpl,
                          uint64_t*             out_lba,
                          uint32_t*             out_off)
{
  const uint32_t nlen  = (plan->lfn_entries != 0U) ? plan->nunits : 0U;
  const uint8_t  csum  = priv_sfn_checksum(plan->name83);
  const uint32_t total = (uint32_t)plan->lfn_entries + 1U;
  dir_slot_t     cur   = plan->start;
  uint64_t       lba   = cur.w.cur_lba;
  uint8_t* const buf   = priv_sec_walk();
  ra8_err_t      err   = priv_read_sector(m, lba, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < total; i++) {
    uint8_t* slot = &buf[(size_t)cur.ent * (size_t)k_ra8_fs_dir_entry_bytes];
    if (i < (uint32_t)plan->lfn_entries) {
      priv_lfn_fill_slot(slot,
                         plan->units,
                         nlen,
                         (uint32_t)plan->lfn_entries - i,
                         (uint8_t)((i == 0U) ? 1U : 0U),
                         csum);
    } else {
      priv_byte_copy(slot, tmpl, (uint32_t)k_ra8_fs_dir_entry_bytes);
      priv_byte_copy(&slot[k_dir_off_name], plan->name83, (uint32_t)k_dir_name_field_len);
      slot[k_dir_off_ntres] = plan->ntres;
      *out_lba              = lba;
      *out_off              = cur.ent * (uint32_t)k_ra8_fs_dir_entry_bytes;
    }
    if ((i + 1U) < total) {
      err = internal_slot_advance(m, &cur, buf, &lba);
      if (err != k_ra8_ok) {
        return err;
      }
    }
  }
  return priv_write_sector(m, lba, buf);
}

/* =============================================================================
 * Deletion -- the 8.3 entry AND the chain in front of it
 * =============================================================================
 */

/* Ported to Zig (ra8_fs_dir_erase_abi.zig); the wrapper below links them. */
extern void internal_run_push(dir_pos_t* run, uint32_t* len_io, uint64_t lba,
                              uint32_t off);
extern ra8_err_t internal_dir_collect_chain(const ra8_fs_mount_t* m, const dir_loc_t* loc,
                                            uint64_t tlba, uint32_t toff, uint8_t csum,
                                            dir_pos_t* run, uint32_t* out_len);
extern ra8_err_t internal_dir_erase_positions(const ra8_fs_mount_t* m,
                                              const dir_pos_t* pos, uint32_t count);

/* `priv_dir_erase_chain()`: see header for the documented contract. */
ra8_err_t priv_dir_erase_chain(const ra8_fs_mount_t* m,
                               const dir_loc_t*      loc,
                               uint64_t              lba,
                               uint32_t              off,
                               const uint8_t*        name83)
{
  dir_pos_t       all[(uint32_t)k_lfn_erase_max + 1U] = {};
  uint32_t        len                                 = 0U;
  const ra8_err_t cerr =
    internal_dir_collect_chain(m, loc, lba, off, priv_sfn_checksum(name83), all, &len);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  all[len].lba = lba;
  all[len].off = off;
  return internal_dir_erase_positions(m, all, len + 1U);
}

/* `priv_dir_lookup_any()`: see header for the documented contract. */
ra8_err_t priv_dir_lookup_any(const ra8_fs_mount_t* m,
                              const dir_loc_t*      loc,
                              const char*           leaf,
                              uint64_t*             out_lba,
                              uint32_t*             out_off,
                              uint8_t               out_entry[k_ra8_fs_dir_entry_bytes])
{
  uint8_t   name83[k_max_8_3_name] = {};
  ra8_err_t err                    = k_ra8_err_not_found;
  if (priv_path_to_83(leaf, name83) != 0U) {
    err = priv_dir_find(m, loc, name83, out_lba, out_off, out_entry);
  }
  if (err == k_ra8_err_not_found) {
    /* A name that is not 8.3-representable, or one whose 8.3 lookup missed
     * because the file is filed under a generated `~N` alias, can still match
     * the reassembled long name. */
    err = priv_dir_find_long(m, loc, leaf, out_lba, out_off, out_entry);
  }
  return err;
}
