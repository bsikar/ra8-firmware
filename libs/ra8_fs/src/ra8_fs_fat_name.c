/**
 * @file ra8_fs_fat_name.c
 * @brief FAT 8.3 short-name pack/unpack and directory walking.
 *
 * @details
 * 8.3 short-name encode/decode, the directory-walk cursor, and the
 * short-name directory lookup primitives for the `ra8_fs` FAT adapter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/* =============================================================================
 * 8.3 short name pack / unpack
 * =============================================================================
 */

/**
 * @brief Convert "FILE.TXT" (caller-supplied path) to packed 11-byte 8.3.
 *
 * @details
 * Result is space-padded as on-disk. Lower-case input is upper-cased.
 * Returns 0 on bad name (>8 base, >3 ext, missing chars), 1 on success.
 */
/* `priv_to_upper()`: see header for the documented contract. */
char priv_to_upper(char c)
{
  if (c >= 'a' && c <= 'z') {
    return (char)(c - 'a' + 'A');
  }
  return c;
}

/**
 * @brief Pack the base portion of a path into out11[0..7]. Returns 0 on error.
 *
 * @details Reads characters from `*path_io` up to a `.` or NUL,
 *          upper-cases them, and writes them into `out11[0..7]`.
 *
 * @param[in,out] path_io Cursor into the input path; advanced on success.
 * @param[out]    out11   11-byte buffer; first 8 bytes are written.
 *
 * @return 1 on success, 0 on overflow or empty base.
 * @retval 1  Base name packed.
 * @retval 0  Base too long or zero-length.
 *
 * @pre `path_io`, `*path_io`, and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[0..7]` holds the upper-cased base.
 * @post On success, `*path_io` points at the `.` or terminator.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_pack_base(const char** path_io, uint8_t* out11)
{
  const char* path     = *path_io;
  uint8_t     base_len = 0;
  while (*path != '\0' && *path != '.') {
    if (base_len >= k_filename_base_len) {
      return 0U;
    }
    out11[base_len++] = (uint8_t)priv_to_upper(*path++);
  }
  if (base_len == 0U) {
    return 0U;
  }
  *path_io = path;
  return 1U;
}

/**
 * @brief Pack the extension portion of a path into out11[8..10]. Returns 0 on error.
 *
 * @details If `*path` is not `.`, returns success with no writes.
 *
 * @param[in]  path  Cursor at the `.` or terminator following the base.
 * @param[out] out11 11-byte buffer; bytes 8..10 are written.
 *
 * @return 1 on success, 0 on overflow.
 * @retval 1  Extension packed (or absent).
 * @retval 0  Extension too long.
 *
 * @pre `path` and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[8..10]` holds the upper-cased extension.
 * @post `*path` is not modified.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_pack_ext(const char* path, uint8_t* out11)
{
  if (*path != '.') {
    return 1U;
  }
  path++;
  uint32_t ext_len = 0U;
  while (*path != '\0') {
    if (ext_len >= (uint32_t)k_filename_ext_len) {
      return 0U;
    }
    out11[k_filename_base_len + ext_len] = (uint8_t)priv_to_upper(*path++);
    ext_len++;
  }
  return 1U;
}

/* `priv_path_to_83()`: see header for the documented contract. */
uint8_t priv_path_to_83(const char* path, uint8_t* out11)
{
  if (path == nullptr || out11 == nullptr) {
    return 0U;
  }
  while (*path == '/') {
    path++;
  }
  for (uint32_t i = 0; i < (uint32_t)k_max_8_3_name; i++) {
    out11[i] = ' ';
  }
  if (priv_pack_base(&path, out11) == 0U) {
    return 0U;
  }
  if (priv_pack_ext(path, out11) == 0U) {
    return 0U;
  }
  if (out11[0] == k_dir_marker_free_used) {
    out11[0] = k_dir_marker_kanji_e5;
  }
  return 1U;
}

/* `priv_83_to_str()`: see header for the documented contract. */
void priv_83_to_str(const uint8_t* in11, char* out12)
{
  uint32_t i = 0;
  uint32_t j = 0;
  for (i = 0; i < (uint32_t)k_filename_base_len; i++) {
    if (in11[i] == ' ') {
      break;
    }
    out12[j++] = (char)in11[i];
  }
  /* Restore kanji escape. */
  /* mcdc-deactivated: 3-condition AND on Shift-JIS kanji-escape directory entry; only reachable from a kanji-named FAT image, none of which exist in the test corpus. */
  if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 && in11[0] == k_dir_marker_kanji_e5) {
    out12[0] = (char)k_dir_marker_free_used;
  }
  uint8_t has_ext = 0;
  for (i = 0; i < k_filename_ext_len; i++) {
    if (in11[k_filename_base_len + i] != ' ') {
      has_ext = 1;
      break;
    }
  }
  if (has_ext != 0U) {
    out12[j++] = '.';
    for (i = 0; i < k_filename_ext_len; i++) {
      if (in11[k_filename_base_len + i] == ' ') {
        break;
      }
      out12[j++] = (char)in11[k_filename_base_len + i];
    }
  }
  out12[j] = '\0';
}

/* =============================================================================
 * Directory walking
 * =============================================================================
 */

/**
 * @brief Initialise a walker that iterates the volume root directory.
 *
 * @details FAT12/16 use a fixed root region; FAT32 uses a cluster
 *          chain rooted at `m->root_cluster`.
 *
 * @param[in]  m Mount providing geometry and FAT type.
 * @param[out] w Walker cursor to initialise.
 *
 * @pre `m` and `w` are non-NULL.
 * @pre `m` has been fully populated by `priv_compute_geometry`.
 * @post `w` points at the first sector of the root directory.
 * @post `w->entry_idx` is zero.
 *
 * @note Pure init -- does not touch the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_dir_walk_init_root(const ra8_fs_mount_t* m, dir_walk_t* w)
{
  if (m->type == k_ra8_fs_type_fat32) {
    w->is_root_fixed     = 0;
    w->fixed_remaining   = 0;
    w->cluster           = m->root_cluster;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    const uint32_t root_dir_sectors =
      ((m->root_entries * k_ra8_fs_dir_entry_bytes) + (k_ra8_fs_bytes_per_sector - 1U)) /
      k_ra8_fs_bytes_per_sector;
    w->is_root_fixed     = 1;
    w->fixed_remaining   = root_dir_sectors;
    w->cluster           = 0;
    w->sector_in_cluster = 0;
    w->cur_lba           = m->first_root_lba;
  }
  w->entry_idx    = 0;
  w->cluster_hops = 0;
}

/* `priv_dir_walk_init_loc()`: see header for the documented contract. */
void priv_dir_walk_init_loc(const ra8_fs_mount_t* m, const dir_loc_t* loc, dir_walk_t* w)
{
  if (loc->is_root != 0U) {
    priv_dir_walk_init_root(m, w);
    return;
  }
  w->is_root_fixed     = 0;
  w->fixed_remaining   = 0;
  w->cluster           = loc->cluster;
  w->sector_in_cluster = 0;
  w->cur_lba           = priv_cluster_to_lba(m, loc->cluster);
  w->entry_idx         = 0;
  w->cluster_hops      = 0;
}

/* `priv_dir_walk_next_sector()`: see header for the documented contract. */
ra8_err_t priv_dir_walk_next_sector(const ra8_fs_mount_t* m, dir_walk_t* w, uint8_t* out_eod)
{
  *out_eod = 0;
  if (w->is_root_fixed != 0U) {
    if (w->fixed_remaining <= 1U) {
      *out_eod = 1;
      return k_ra8_ok;
    }
    w->fixed_remaining--;
    w->cur_lba++;
    w->entry_idx = 0;
    return k_ra8_ok;
  }
  /* FAT32 cluster-chain root. */
  w->sector_in_cluster++;
  if (w->sector_in_cluster >= m->sectors_per_cluster) {
    uint32_t  next = 0;
    ra8_err_t err  = priv_fat_get(m, w->cluster, &next);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      *out_eod = 1;
      return k_ra8_ok;
    }
    /* Cycle guard (NASA Rule 2): a healthy chain visits at most
     * count_of_clusters distinct clusters; more means a corrupt loop. */
    w->cluster_hops++;
    if (w->cluster_hops > m->count_of_clusters) {
      return k_ra8_err_protocol_error;
    }
    w->cluster           = next;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    w->cur_lba++;
  }
  w->entry_idx = 0;
  return k_ra8_ok;
}

/* `priv_dir_find()`: see header for the documented contract. */
ra8_err_t priv_dir_find(const ra8_fs_mount_t* m,
                        const dir_loc_t*      loc,
                        const uint8_t*        name83,
                        uint32_t*             out_lba,
                        uint32_t*             out_entry_off,
                        uint8_t               out_entry[k_ra8_fs_dir_entry_bytes])
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
      if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
        return k_ra8_err_not_found;
      }
      if (ent[k_dir_off_name] == k_dir_marker_free_used) {
        continue;
      }
      if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn) {
        continue;
      }
      if (priv_byte_equal(ent, name83, k_dir_name_field_len) != 0U) {
        *out_lba       = w.cur_lba;
        *out_entry_off = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
        priv_byte_copy(out_entry, ent, k_ra8_fs_dir_entry_bytes);
        return k_ra8_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_not_found;
}
