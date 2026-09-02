/**
 * @file ra8_fs_fat_lfn.c
 * @brief VFAT long-filename (LFN) entry layout: reading a chain, and filling one slot.
 *
 * @details
 * Reassembles a chain of attr-0x0F entries into a long file name and matches it
 * during directory scans. It also owns the two things the WRITE side needs from
 * the same layout -- the 8.3 checksum that binds a chain to its entry, and
 * ::priv_lfn_fill_slot(), which lays one slot out from the same character-offset
 * table the reassembler indexes. Keeping both directions against one table is
 * the point: a writer with its own copy of those thirteen offsets would be a
 * second chance to disagree about where character 7 lives. The verbs that
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
 * Reading a chain came first (#101); writing one came later (#600) and shares
 * this file's layout table rather than restating it.
 * ===========================================================================
 */

/**
 * @enum ra8_fs_lfn_char_off_t
 * @brief Byte offsets of the 13 UTF-16 name characters in a 32-byte LFN entry.
 * @details MS FAT spec sec 7 "Long Directory Entries": LDIR_Name1 holds five
 *          chars at offsets 1,3,5,7,9; LDIR_Name2 holds six at 14,16,18,20,22,24;
 *          LDIR_Name3 holds two at 28,30. Each char is two bytes (low byte first).
 */
typedef enum : uint8_t {
  k_lfn_char_off_0  = 1U,  /**< LDIR_Name1 char 0. */
  k_lfn_char_off_1  = 3U,  /**< LDIR_Name1 char 1. */
  k_lfn_char_off_2  = 5U,  /**< LDIR_Name1 char 2. */
  k_lfn_char_off_3  = 7U,  /**< LDIR_Name1 char 3. */
  k_lfn_char_off_4  = 9U,  /**< LDIR_Name1 char 4. */
  k_lfn_char_off_5  = 14U, /**< LDIR_Name2 char 0. */
  k_lfn_char_off_6  = 16U, /**< LDIR_Name2 char 1. */
  k_lfn_char_off_7  = 18U, /**< LDIR_Name2 char 2. */
  k_lfn_char_off_8  = 20U, /**< LDIR_Name2 char 3. */
  k_lfn_char_off_9  = 22U, /**< LDIR_Name2 char 4. */
  k_lfn_char_off_10 = 24U, /**< LDIR_Name2 char 5. */
  k_lfn_char_off_11 = 28U, /**< LDIR_Name3 char 0. */
  k_lfn_char_off_12 = 30U, /**< LDIR_Name3 char 1. */
} ra8_fs_lfn_char_off_t;

/**
 * @var s_lfn_char_off
 * @brief Byte offset of each of an LFN entry's thirteen name characters.
 * @details Built from ::ra8_fs_lfn_char_off_t so the reassembler and the
 *          writer index the same layout from one table -- a second table would
 *          be a second chance to disagree about where character 7 lives.
 * @note Read-only; shared by ::priv_lfn_add() and ::priv_lfn_fill_slot().
 * @since 0.1.0
 */
static const uint8_t s_lfn_char_off[k_lfn_chars_per_ent] = {(uint8_t)k_lfn_char_off_0,
                                                            (uint8_t)k_lfn_char_off_1,
                                                            (uint8_t)k_lfn_char_off_2,
                                                            (uint8_t)k_lfn_char_off_3,
                                                            (uint8_t)k_lfn_char_off_4,
                                                            (uint8_t)k_lfn_char_off_5,
                                                            (uint8_t)k_lfn_char_off_6,
                                                            (uint8_t)k_lfn_char_off_7,
                                                            (uint8_t)k_lfn_char_off_8,
                                                            (uint8_t)k_lfn_char_off_9,
                                                            (uint8_t)k_lfn_char_off_10,
                                                            (uint8_t)k_lfn_char_off_11,
                                                            (uint8_t)k_lfn_char_off_12};

/* `priv_sfn_checksum()`: see header for the documented contract. */
uint8_t priv_sfn_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_dir_name_field_len; i++) {
    sum = (uint8_t)((((sum & 1U) != 0U) ? (uint32_t)k_sfn_csum_high_bit : 0U) +
                    (uint32_t)(sum >> 1U) + (uint32_t)name83[i]);
  }
  return sum;
}

/* `priv_lfn_fill_slot()`: see header for the documented contract. */
void priv_lfn_fill_slot(uint8_t*        ent,
                        const uint16_t* name,
                        uint32_t        nlen,
                        uint32_t        order,
                        uint8_t         is_last,
                        uint8_t         csum)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_fs_dir_entry_bytes; i++) {
    ent[i] = 0U;
  }
  ent[k_lfn_off_seq]      = (uint8_t)(order | ((is_last != 0U) ? (uint32_t)k_lfn_seq_last : 0U));
  ent[k_dir_off_attr]     = (uint8_t)k_ra8_fs_attr_lfn;
  ent[k_lfn_off_type]     = 0U;
  ent[k_lfn_off_checksum] = csum;
  priv_wr16(&ent[k_lfn_off_clus_lo], 0U);
  const uint32_t base = (order - 1U) * (uint32_t)k_lfn_chars_per_ent;
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_chars_per_ent; i++) {
    const uint32_t pos = base + i;
    uint16_t       val = (uint16_t)k_lfn_unicode_pad;
    if (pos < nlen) {
      /* The unit itself. Taking one byte of the caller's name per slot was
       * correct only for ASCII, and silently wrong for everything else (#606). */
      val = name[pos];
    } else if (pos == nlen) {
      val = 0U; /* the NUL that terminates the last populated group */
    } else {
      /* past the terminator: 0xFFFF padding, already in `val` */
    }
    priv_wr16(&ent[s_lfn_char_off[i]], val);
  }
}

/* `priv_lfn_reset()`: see header for the documented contract. */
void priv_lfn_reset(lfn_state_t* s)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_write_max; i++) {
    s->units[i] = 0U;
  }
  s->checksum = 0U;
  s->have     = 0U;
}

/* `priv_lfn_add()`: see header for the documented contract. */
void priv_lfn_add(lfn_state_t* s, const uint8_t* ent)
{
  const uint32_t order = (uint32_t)(ent[k_lfn_off_seq] & (uint8_t)k_lfn_seq_order_mask);
  if ((order < 1U) || (order > (uint32_t)k_lfn_max_entries)) {
    return; /* out-of-range sequence -> corrupt chain, ignore this entry */
  }
  s->checksum         = ent[k_lfn_off_checksum];
  s->have             = 1U;
  const uint32_t base = (order - 1U) * (uint32_t)k_lfn_chars_per_ent;
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_chars_per_ent; i++) {
    const uint32_t off = (uint32_t)s_lfn_char_off[i];
    const uint32_t val = (uint32_t)ent[off] | ((uint32_t)ent[off + 1U] << 8U);
    const uint32_t pos = base + i;
    if (pos >= (uint32_t)k_lfn_write_max) {
      /* Unreachable: max pos = (k_lfn_max_entries-1)*k_lfn_chars_per_ent +
       * (k_lfn_chars_per_ent-1) = 18*13+12 = 246 < k_lfn_write_max = 247. */
      break; /* GCOVR_EXCL_LINE -- loop index proof keeps pos below write bound */
    }
    if ((val == 0U) || (val == (uint32_t)k_lfn_unicode_pad)) {
      s->units[pos] = 0U; /* terminator / padding ends this group's name */
      break;
    }
    /* The unit as stored. Substituting '?' for everything above 0x7F -- which
     * is what this did -- made the reported name one the caller could not hand
     * back to `ra8_fs_open()`, so the file was listed and unopenable (#606). */
    s->units[pos] = (uint16_t)val;
  }
}

/* `priv_lfn_units_for()`: see header for the documented contract. */
const uint16_t* priv_lfn_units_for(const lfn_state_t* s, const uint8_t* name83, uint32_t* out_units)
{
  *out_units = 0U;
  if ((s->have == 0U) || (s->units[0] == 0U)) {
    return nullptr;
  }
  if (s->checksum != priv_sfn_checksum(name83)) {
    return nullptr;
  }
  uint32_t n = 0U;
  while ((n < (uint32_t)k_lfn_write_max) && (s->units[n] != 0U)) {
    n++;
  }
  *out_units = n;
  return s->units;
}

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
     * name held an accent could not be opened by its real name (#606). */
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

/* `priv_free_chain()`: see header for the documented contract. */
ra8_err_t priv_free_chain(const ra8_fs_mount_t* m, uint32_t start)
{
  uint32_t cur   = start;
  uint32_t guard = 0;
  while (cur >= k_cluster_first_data && (cur - k_cluster_first_data) < m->count_of_clusters) {
    uint32_t  next = 0;
    ra8_err_t err  = priv_fat_get(m, cur, &next);
    if (err != k_ra8_ok) {
      return err;
    }
    err = priv_fat_set(m, cur, k_cluster_free);
    if (err != k_ra8_ok) {
      return err;
    }
    /* The volume just got a cluster back, so say so (#607): the free count
     * feeds FSInfo, and pulling the hint back is what makes the freed space
     * the next thing allocated rather than something only found after a full
     * wrap of the scan. */
    priv_free_count_gave(m, 1U);
    priv_alloc_hint_lower(m, cur);
    if (priv_is_eoc(m, next) != 0U) {
      break;
    }
    cur = next;
    /* Bounded loop -- can't visit more clusters than exist. */
    guard++;
    if (guard > m->count_of_clusters) {
      return k_ra8_err_protocol_error;
    }
  }
  return k_ra8_ok;
}
