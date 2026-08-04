/**
 * @file ra8_fs_fat_lfn.c
 * @brief VFAT long-filename (LFN) read support for the FAT adapter.
 *
 * @details
 * Reassembles a chain of attr-0x0F entries into a long file name, then
 * matches it during directory scans (read-only).
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
 * This is READ-only: 8.3 creation/writes are unchanged (see #101).
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
 * @brief Compute the 8.3 short-name checksum carried in each LFN directory entry.
 *
 * @details Implements the rotate-right-add algorithm from the Microsoft FAT spec
 *          section 7 ("Long File Name Directory Entries"). Each of the eleven
 *          bytes of the 8.3 name field is folded into a running sum: the previous
 *          sum is rotated right by one bit (preserving the low bit) and the next
 *          byte is added. The result binds a set of LFN slots to their trailing
 *          8.3 entry so that a chain cannot alias a different short name.
 *
 * @param[in] name83 Pointer to the 11-byte 8.3 name field (DIR_Name in the
 *                   FAT spec, space-padded, not NUL-terminated).
 *
 * @return Computed 8-bit checksum.
 * @retval 0..255 The folded checksum; all values are possible.
 *
 * @pre @p name83 is non-NULL and points to at least 11 valid bytes.
 * @pre The 11-byte field is the raw DIR_Name from a valid FAT directory entry.
 * @post Return value matches the Checksum field in every associated LFN entry.
 * @post No bytes of @p name83 are modified (read-only access).
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_sfn_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_dir_name_field_len; i++) {
    sum = (uint8_t)((((sum & 1U) != 0U) ? (uint32_t)k_sfn_csum_high_bit : 0U) +
                    (uint32_t)(sum >> 1U) + (uint32_t)name83[i]);
  }
  return sum;
}

/* `priv_lfn_reset()`: see header for the documented contract. */
void priv_lfn_reset(lfn_state_t* s)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_name_cap; i++) {
    s->name[i] = '\0';
  }
  s->checksum = 0U;
  s->have     = 0U;
}

/* `priv_lfn_add()`: see header for the documented contract. */
void priv_lfn_add(lfn_state_t* s, const uint8_t* ent)
{
  static const uint8_t k_off[k_lfn_chars_per_ent] = {(uint8_t)k_lfn_char_off_0,
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
  const uint32_t       order = (uint32_t)(ent[k_lfn_off_seq] & (uint8_t)k_lfn_seq_order_mask);
  if ((order < 1U) || (order > (uint32_t)k_lfn_max_entries)) {
    return; /* out-of-range sequence -> corrupt chain, ignore this entry */
  }
  s->checksum         = ent[k_lfn_off_checksum];
  s->have             = 1U;
  const uint32_t base = (order - 1U) * (uint32_t)k_lfn_chars_per_ent;
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_chars_per_ent; i++) {
    const uint32_t off = (uint32_t)k_off[i];
    const uint32_t val = (uint32_t)ent[off] | ((uint32_t)ent[off + 1U] << 8U);
    const uint32_t pos = base + i;
    if (pos >= ((uint32_t)k_lfn_name_cap - 1U)) {
      /* Unreachable: max pos = (k_lfn_max_entries-1)*k_lfn_chars_per_ent +
       * (k_lfn_chars_per_ent-1) = 18*13+12 = 246 < k_lfn_name_cap-1 = 255. */
      break; /* GCOVR_EXCL_LINE */
    }
    if ((val == 0U) || (val == (uint32_t)k_lfn_unicode_pad)) {
      s->name[pos] = '\0'; /* terminator / padding ends this group's name */
      break;
    }
    s->name[pos] =
      (char)((val <= (uint32_t)k_lfn_ascii_max) ? (unsigned char)val : (unsigned char)'?');
  }
}

/* `priv_lfn_name_for()`: see header for the documented contract. */
const char* priv_lfn_name_for(lfn_state_t* s, const uint8_t* name83)
{
  if ((s->have == 0U) || (s->name[0] == '\0')) {
    return nullptr;
  }
  if (s->checksum != priv_sfn_checksum(name83)) {
    return nullptr;
  }
  return s->name;
}

/**
 * @brief Compare two NUL-terminated ASCII names for case-insensitive equality.
 *
 * @details Walks both strings simultaneously, uppercasing each character via
 *          priv_to_upper() before comparing. Returns 1 only if both strings
 *          reach their NUL terminator at the same step (same length, same
 *          content ignoring case). A length mismatch or any differing character
 *          yields 0.
 *
 * @param[in] a First NUL-terminated name.
 * @param[in] b Second NUL-terminated name.
 *
 * @return Equality flag.
 * @retval 1U The names are equal (case-insensitive).
 * @retval 0U The names differ in length or at least one character.
 *
 * @pre @p a is non-NULL and NUL-terminated.
 * @pre @p b is non-NULL and NUL-terminated.
 * @post Neither @p a nor @p b is modified.
 * @post Return value reflects only the ASCII case-fold comparison result.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_name_ieq(const char* a, const char* b)
{
  while ((*a != '\0') && (*b != '\0')) {
    if (priv_to_upper(*a) != priv_to_upper(*b)) {
      return 0U;
    }
    a++;
    b++;
  }
  return ((*a == '\0') && (*b == '\0')) ? 1U : 0U;
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
 * @param[in]     needle        Requested name (already slash-stripped).
 * @param[in]     buf           One directory sector (k_ra8_fs_bytes_per_sector).
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
static ra8_fs_lfn_scan_t priv_dir_find_long_sector(const char*    needle,
                                                   const uint8_t* buf,
                                                   uint32_t       cur_lba,
                                                   lfn_state_t*   lfn,
                                                   uint32_t*      out_lba,
                                                   uint32_t*      out_entry_off,
                                                   uint8_t out_entry[k_ra8_fs_dir_entry_bytes])
{
  for (uint32_t e = 0; e < k_dir_entries_per_sector; e++) {
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
    const char* lname = priv_lfn_name_for(lfn, ent);
    if ((lname != nullptr) && (priv_name_ieq(needle, lname) != 0U)) {
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
                             uint32_t*             out_lba,
                             uint32_t*             out_entry_off,
                             uint8_t               out_entry[k_ra8_fs_dir_entry_bytes])
{
  const char* needle = want;
  if (needle[0] == '/') {
    needle++; /* flat root: ignore a leading slash */
  }
  dir_walk_t w = {};
  priv_dir_walk_init_loc(m, loc, &w);
  lfn_state_t lfn = {};
  priv_lfn_reset(&lfn);
  uint8_t eod                            = 0;
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    const ra8_fs_lfn_scan_t scan =
      priv_dir_find_long_sector(needle, buf, w.cur_lba, &lfn, out_lba, out_entry_off, out_entry);
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

/* `priv_dir_find_free()`: see header for the documented contract. */
ra8_err_t priv_dir_find_free(const ra8_fs_mount_t* m,
                             const dir_loc_t*      loc,
                             uint32_t*             out_lba,
                             uint32_t*             out_entry_off)
{
  dir_walk_t w = {};
  priv_dir_walk_init_loc(m, loc, &w);
  uint8_t eod                            = 0;
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    for (uint32_t e = 0; e < k_dir_entries_per_sector; e++) {
      uint8_t* ent = &buf[(size_t)e * (size_t)k_ra8_fs_dir_entry_bytes];
      if (ent[k_dir_off_name] == k_dir_marker_free_perm ||
          ent[k_dir_off_name] == k_dir_marker_free_used) {
        *out_lba       = w.cur_lba;
        *out_entry_off = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
        return k_ra8_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_no_mem;
}

/* `priv_free_chain()`: see header for the documented contract. */
ra8_err_t priv_free_chain(const ra8_fs_mount_t* m, uint32_t start)
{
  uint32_t cur   = start;
  uint32_t guard = 0;
  /* mcdc-deactivated: loop bound; `cur < k_cluster_first_data` only on a corrupt FAT chain (the first-cluster reservation 0/1 is enforced at allocation). */
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
