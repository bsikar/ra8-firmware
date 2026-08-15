/**
 * @file ra8_fs_fat_exfat_label.c
 * @brief exFAT volume-label read/write (the exFAT half of `ra8_fs_{get,set}_label`).
 *
 * @details
 * exFAT keeps the volume label in a single root-directory entry -- the Volume
 * Label entry, type 0x83 when a label is present, 0x03 (in-use bit clear) when
 * it is not (exFAT spec sec 7.7). Unlike a File entry set, it stands alone with
 * no secondary entries and no SetChecksum spanning several entries, so reading
 * and rewriting it is a single-entry operation. The FAT half, and the public
 * lock-bracketed entry points that dispatch here, live in `ra8_fs_fat_label.c`.
 *
 * References (every shorthand citation in this file):
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00, section 7.7 "Volume Label Directory Entry".
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the root scan is bounded by ::k_exfat_scan_limit; the decode /
 *     encode loops by ::k_exfat_fmt_label_max (11).
 *   - Rule 3: zero malloc; one 32-byte entry buffer on the stack.
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
#include "ra8_fs_fat_internal.h"
#include "ra8_fs_meta.h"

/**
 * @brief Locate the exFAT Volume Label entry (or the slot to create one at).
 *
 * @details Scans the root directory. On the first entry whose type -- ignoring
 *          the in-use bit -- is the Volume Label type (0x83 present, or 0x03
 *          cleared), reports it: @p out_pos its position, @p out_entry its 32
 *          bytes, @p out_present true. If end-of-directory is reached first the
 *          volume carries no label entry: @p out_pos is the EOD slot a new one
 *          would go in and @p out_present is false.
 *
 * @param[in]  m           Mounted exFAT volume.
 * @param[out] out_pos     Position of the label entry, or of the EOD slot.
 * @param[out] out_entry   The 32-byte label entry (only meaningful when present).
 * @param[out] out_present Receives true when a label entry exists.
 *
 * @return Error code.
 * @retval k_ra8_ok            @p out_pos / @p out_present populated.
 * @retval k_ra8_err_not_found The scan limit was hit without an entry or EOD.
 * @retval k_ra8_err_*         Backend read failure.
 *
 * @pre All pointers are non-NULL; `m->type` is exFAT.
 * @pre The mount is in use.
 * @post On k_ra8_ok @p out_pos addresses a writable root-directory slot.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): ::k_exfat_scan_limit entries.
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_locate_label(const ra8_fs_mount_t* m,
                                             exfat_setpos_t*       out_pos,
                                             uint8_t*              out_entry,
                                             bool*                 out_present)
{
  const uint8_t label_type =
    (uint8_t)((uint32_t)k_exfat_entry_label & (uint32_t)~(uint32_t)k_exfat_inuse_bit);
  exfat_dir_t root = {};
  priv_exfat_dir_root(m, &root);
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(&root, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    const exfat_setpos_t at = {.cluster = cur.cluster, .index = cur.entry_in_cluster};
    uint8_t              e[k_exfat_entry_bytes] = {};
    const ra8_err_t      err                    = priv_exfat_next_entry(m, &cur, e);
    if (err != k_ra8_ok) {
      return err;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      *out_pos     = at;
      *out_present = false;
      return k_ra8_ok;
    }
    if ((uint8_t)(e[0] & (uint8_t)~(uint8_t)k_exfat_inuse_bit) == label_type) {
      *out_pos = at;
      priv_byte_copy(out_entry, e, (uint32_t)k_exfat_entry_bytes);
      *out_present = true;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found; /* GCOVR_EXCL_LINE */
}

/**
 * @brief Decode an exFAT Volume Label entry's UTF-16LE label into ASCII.
 *
 * @details Reads the CharacterCount and copies the low byte of each UTF-16 code
 *          unit -- the ASCII character for a Latin-1 label -- into @p out,
 *          truncated to the entry's cap and to @p out_len, and NUL-terminated.
 *
 * @param[in]  entry   A 32-byte Volume Label entry (type 0x83).
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes (at least 1).
 *
 * @return Nothing.
 *
 * @pre @p entry and @p out are non-NULL; `out_len >= 1`.
 * @pre @p entry is an in-use Volume Label entry.
 * @post @p out is NUL-terminated (possibly truncated).
 * @post No byte past @p out[out_len-1] is written.
 *
 * @note Bounded loop (NASA Rule 2): ::k_exfat_fmt_label_max iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_exfat_label_decode(const uint8_t* entry, char* out, uint32_t out_len)
{
  uint32_t n = (uint32_t)entry[k_exfat_de_lbl_cnt];
  if (n > (uint32_t)k_exfat_fmt_label_max) {
    n = (uint32_t)k_exfat_fmt_label_max;
  }
  uint32_t w = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    if ((w + 1U) >= out_len) {
      break;
    }
    out[w] = (char)entry[(uint32_t)k_exfat_de_lbl_name + (i * 2U)];
    w++;
  }
  out[w] = '\0';
}

/* `priv_exfat_get_label()`: see header for the documented contract. */
ra8_err_t priv_exfat_get_label(const ra8_fs_mount_t* m, char* out, uint32_t out_len)
{
  exfat_setpos_t  pos                        = {};
  uint8_t         entry[k_exfat_entry_bytes] = {};
  bool            present                    = false;
  const ra8_err_t err = internal_exfat_locate_label(m, &pos, entry, &present);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!present || (entry[0] != (uint8_t)k_exfat_entry_label)) {
    out[0] = '\0'; /* no entry, or a cleared (0x03) one: unlabelled */
    return k_ra8_ok;
  }
  internal_exfat_label_decode(entry, out, out_len);
  return k_ra8_ok;
}

/* `priv_exfat_set_label()`: see header for the documented contract. */
ra8_err_t priv_exfat_set_label(const ra8_fs_mount_t* m, const char* label)
{
  exfat_setpos_t  pos                        = {};
  uint8_t         found[k_exfat_entry_bytes] = {};
  bool            present                    = false;
  const ra8_err_t err = internal_exfat_locate_label(m, &pos, found, &present);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t entry[k_exfat_entry_bytes] = {};
  entry[0]                           = (uint8_t)k_exfat_entry_label;
  uint32_t n                         = 0U;
  if (label != nullptr) {
    while (n < (uint32_t)k_exfat_fmt_label_max) {
      if (label[n] == '\0') {
        break;
      }
      entry[(uint32_t)k_exfat_de_lbl_name + (n * 2U)] = (uint8_t)label[n];
      n++;
    }
  }
  entry[k_exfat_de_lbl_cnt] = (uint8_t)n;
  return priv_exfat_write_dir_set(m, pos.cluster, pos.index, entry, (uint32_t)k_exfat_entry_bytes);
}
