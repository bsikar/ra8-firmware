/**
 * @file ra8_fs_fat_lfn.c
 * @brief VFAT long-filename (LFN) entry layout: reading a chain, and filling one slot.
 *
 * @details
 * Matches a long file name during directory scans (::priv_dir_find_long). The
 * layout helpers it uses -- the 8.3 checksum, chain reassembly and
 * ::priv_lfn_fill_slot() for the writer -- live in `ra8_fs_lfn_abi.zig`, next
 * to the one character-offset table both directions index. The verbs that
 * decide WHEN to write a chain live in `ra8_fs_fat_lfn_write.c`.
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

/* ===========================================================================
 * VFAT long-filename (LFN) read support
 *
 * A long name is stored as a chain of attr-0x0F entries IMMEDIATELY before the
 * 8.3 short entry, in reverse order: the entry tagged 0x40 (last logical group)
 * is physically first, then group N-1 ... group 1, then the 8.3 entry. Each LFN
 * entry carries 13 UTF-16LE chars (offsets 1/3/5/7/9, 14/16/18/20/22/24, 28/30)
 * and a checksum of the 8.3 name (offset 13) that ties the chain to its entry.
 * The layout table and the chain helpers are in `ra8_fs_lfn_abi.zig`.
 * ===========================================================================
 */

/**
 * @enum ra8_fs_lfn_scan_t
 * @brief Outcome of scanning one directory sector for a long-name match.
 * @details Lets `priv_dir_find_long_sector` report "keep walking", "found",
 *          or "end of directory reached" without unwinding the caller's loop
 *          state, keeping each function under the cognitive-complexity gate.
 */
typedef enum : uint8_t {
  k_lfn_scan_continue = 0U, /**< No match in this sector; advance to the next. */
  k_lfn_scan_found    = 1U, /**< Long name matched; out parameters populated.  */
  k_lfn_scan_eod      = 2U, /**< End-of-directory marker hit; stop the walk.   */
} ra8_fs_lfn_scan_t;

/**
 * @brief Scan one directory sector for a long-name match, updating the chain.
 *
 * @details Folds any LFN sub-entries into @p lfn and, on the trailing 8.3
 *          entry, compares its reassembled long name against @p needle. The
 *          per-sector body of `priv_dir_find_long`, extracted so both the
 *          scan and the walk stay under the function-size / complexity gates.
 *
 * @param[in]     m             Mounted volume (entries-per-sector bound).
 * @param[in]     needle        Requested name as UTF-16 code units.
 * @param[in]     nneedle       Number of units in @p needle.
 * @param[in]     buf           One whole directory sector.
 * @param[in]     cur_lba       LBA of @p buf (recorded into @p out_lba on hit).
 * @param[in,out] lfn           Reassembly state carried across sectors.
 * @param[out]    out_lba       Sector of the matched 8.3 entry (on found).
 * @param[out]    out_entry_off Byte offset within the sector (on found).
 * @param[out]    out_entry     32 bytes of the matched 8.3 entry (on found).
 *
 * @return Scan outcome.
 * @retval k_lfn_scan_found    Match; out parameters populated.
 * @retval k_lfn_scan_eod      Free-permanent marker hit; directory ended.
 * @retval k_lfn_scan_continue No match in this sector.
 *
 * @pre All pointers are non-NULL; @p buf holds one full sector.
 * @pre @p lfn was initialised by priv_lfn_reset() before the first sector.
 * @post On found, the out parameters identify the on-disk 8.3 entry.
 * @post @p lfn reflects any LFN entries accumulated from this sector.
 *
 * @note Not thread-safe; the caller serialises directory access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fs_lfn_scan_t internal_dir_find_long_sector(const ra8_fs_mount_t* m,
                                                       const uint16_t*       needle,
                                                       uint32_t              nneedle,
                                                       const uint8_t*        buf,
                                                       uint64_t              cur_lba,
                                                       lfn_state_t*          lfn,
                                                       uint64_t*             out_lba,
                                                       uint32_t*             out_entry_off,
                                                       uint8_t out_entry[k_ra8_fs_dir_entry_bytes])
{
  for (uint32_t e = 0; e < priv_dir_eps(m); e++) {
    const uint8_t* ent = &buf[(size_t)e * (size_t)k_ra8_fs_dir_entry_bytes];
    if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
      return k_lfn_scan_eod;
    }
    if (ent[k_dir_off_name] == k_dir_marker_free_used) {
      priv_lfn_reset(lfn); /* a deleted slot breaks the chain */
      continue;
    }
    if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn) {
      priv_lfn_add(lfn, ent);
      continue;
    }
    /* Compared as UTF-16, which is the domain the name is stored in and the
     * domain the up-case table folds. Comparing the reassembled text meant
     * comparing against a name the reader had already mangled, so a file whose
     * name held an accent could not be opened by its real name. */
    uint32_t        lnunits = 0U;
    const uint16_t* lunits  = priv_lfn_units_for(lfn, ent, &lnunits);
    if ((lunits != nullptr) && (priv_utf16_ieq(needle, nneedle, lunits, lnunits) != 0U)) {
      *out_lba       = cur_lba;
      *out_entry_off = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
      priv_byte_copy(out_entry, ent, k_ra8_fs_dir_entry_bytes);
      return k_lfn_scan_found;
    }
    priv_lfn_reset(lfn); /* 8.3 entry consumed -> next chain starts fresh */
  }
  return k_lfn_scan_continue;
}

/* `priv_dir_find_long()`: see header for the documented contract. */
ra8_err_t priv_dir_find_long(const ra8_fs_mount_t* m,
                             const dir_loc_t*      loc,
                             const char*           want,
                             uint64_t*             out_lba,
                             uint32_t*             out_entry_off,
                             uint8_t               out_entry[k_ra8_fs_dir_entry_bytes])
{
  const char* want_leaf = want;
  if (want_leaf[0] == '/') {
    want_leaf++; /* flat root: ignore a leading slash */
  }
  uint16_t  needle[k_lfn_write_max] = {};
  uint32_t  nneedle                 = 0U;
  ra8_err_t nerr = priv_utf8_to_utf16(want_leaf, needle, (uint32_t)k_lfn_write_max, &nneedle);
  if (nerr == k_ra8_err_no_mem) {
    /* Longer than any long name this format stores, so nothing here is it. */
    return k_ra8_err_not_found;
  }
  if (nerr != k_ra8_ok) {
    return nerr;
  }
  dir_walk_t w = {};
  priv_dir_walk_init_loc(m, loc, &w);
  lfn_state_t lfn = {};
  priv_lfn_reset(&lfn);
  uint8_t        eod = 0;
  uint8_t* const buf = priv_sec_walk();
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    const ra8_fs_lfn_scan_t scan = internal_dir_find_long_sector(m,
                                                                 needle,
                                                                 nneedle,
                                                                 buf,
                                                                 w.cur_lba,
                                                                 &lfn,
                                                                 out_lba,
                                                                 out_entry_off,
                                                                 out_entry);
    if (scan == k_lfn_scan_found) {
      return k_ra8_ok;
    }
    if (scan == k_lfn_scan_eod) {
      return k_ra8_err_not_found;
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_not_found;
}
