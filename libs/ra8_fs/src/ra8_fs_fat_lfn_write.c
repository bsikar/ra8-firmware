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
  return k_ra8_ok;
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
