/**
 * @file ra8_fs_fat_exfat_mutate.c
 * @brief exFAT unlink / rename / listdir mutation helpers.
 *
 * @details
 * Locates a directory-entry set, frees its clusters and bitmap bits, and
 * applies in-place rename, plus the exFAT directory listing.
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

/* ---- exFAT mutation helpers (unlink / rename / listdir) ------------------ */

/**
 * @enum exfat_mutate_const_t
 * @brief Sizing constants for the exFAT mutation helpers.
 *
 * @details The entry-set bounds these build on (::k_exfat_set_max_entries) and
 *          the ::exfat_setpos_t coordinates they hand around are shared with
 *          the streaming writer, so they live in
 *          `ra8_fs_fat_exfat_stream_internal.h` rather than here.
 *
 * @invariant `k_exfat_rename_bytes` is `k_exfat_rename_entries * 32`.
 *
 * @par Example:
 * @code
 * char name[k_exfat_list_name_cap] = {};
 * @endcode
 *
 * @see priv_exfat_rename()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_exfat_list_name_cap  = 64U, /**< Listdir name buffer (truncated + NUL). */
  k_exfat_rename_entries = 3U,  /**< In-place rename set: File+Stream+Name. */
  k_exfat_rename_bytes   = 96U, /**< Three 32-byte entries.                 */
} exfat_mutate_const_t;

/**
 * @brief Clear a contiguous cluster run in the allocation bitmap.
 *
 * @details Mirror of ::priv_exfat_bitmap_mark: clears one bit per cluster,
 * batching read-modify-write per bitmap sector. Per the exFAT spec the
 * bitmap alone is authoritative for allocation state, so freeing does not
 * require FAT edits.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark free.
 * @return Error code.
 * @retval k_ra8_ok    All bits cleared + written.
 * @retval k_ra8_err_* Backend read/write failure.
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre The bitmap region is contiguous on disk.
 * @post The @p count bits for the run read as 0.
 * @post Only the affected bitmap sectors are rewritten.
 * @note Read-modify-write, one sector at a time.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_bitmap_clear(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count)
{
  uint32_t loaded                         = UINT32_MAX;
  uint8_t  sec[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t idx  = (clus - k_cluster_first_data) + k;
    const uint32_t lba  = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra8_fs_bytes_per_sector);
    const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra8_fs_bytes_per_sector;
    const uint32_t bit  = idx & k_exfat_bit_mask;
    ra8_err_t      e    = priv_exfat_bmp_switch(m, lba, &loaded, sec);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    sec[byte] = (uint8_t)(sec[byte] & (uint8_t) ~(uint8_t)(1U << bit));
    /* Pull the scan hint back to the space just released, or the next create
     * would step over it and only find it after a full rescan (#607). */
    priv_alloc_hint_lower(m, idx + (uint32_t)k_cluster_first_data);
  }
  return priv_write_sector(m, loaded, sec);
}

/**
 * @brief Consume a set's secondary entries, recording positions + matching.
 *
 * @details Reads the @p sc secondaries following a File entry, snapshots
 * each entry's (cluster, index) into @p pos starting at slot 1, and checks
 * the set against @p path: the first secondary must be a Stream entry with
 * the right NameLength, the rest must be Name entries whose UTF-16 chunks
 * equal the path. The cursor always consumes all @p sc entries so the
 * caller's walk stays aligned.
 *
 * @param[in]     m         Mounted exFAT volume.
 * @param[in,out] cur       Directory cursor (just past the File entry).
 * @param[in]     path      Target name (ASCII, root-level).
 * @param[in]     nlen      Length of @p path.
 * @param[in]     sc        SecondaryCount from the File entry.
 * @param[out]    pos       Position array (slot 0 already holds the File).
 * @param[out]    strm_copy Receives the 32-byte Stream entry when matched.
 * @param[out]    out_match Receives 1 when the whole set matches @p path.
 * @return Error code.
 * @retval k_ra8_ok    All @p sc secondaries were consumed.
 * @retval k_ra8_err_* Backend read failure mid-set.
 * @pre @p cur sits immediately after the set's File entry.
 * @pre @p pos has at least 1 + @p sc slots.
 * @post @p cur sits immediately after the set's last secondary.
 * @post On k_ra8_ok @p out_match is 0 or 1.
 * @note Helper of ::priv_exfat_find_set (complexity split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_take_set(const ra8_fs_mount_t* m,
                                     exfat_cursor_t*       cur,
                                     const char*           path,
                                     uint32_t              nlen,
                                     uint32_t              sc,
                                     exfat_setpos_t*       pos,
                                     uint8_t*              strm_copy,
                                     uint8_t*              out_match)
{
  uint8_t matched = 1U;
  for (uint32_t k = 0U; k < sc; k++) {
    const exfat_setpos_t sp = {.cluster = cur->cluster, .index = cur->entry_in_cluster};
    uint8_t              se[k_exfat_entry_bytes] = {};
    const ra8_err_t      r                       = priv_exfat_next_entry(m, cur, se);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    pos[1U + k] = sp;
    if (k == 0U) {
      if (se[0] != (uint8_t)k_exfat_entry_stream) {
        matched = 0U;
        continue;
      }
      if ((uint32_t)se[k_exfat_strm_off_nlen] != nlen) {
        matched = 0U;
        continue;
      }
      priv_byte_copy(strm_copy, se, (uint32_t)k_exfat_entry_bytes);
      continue;
    }
    if (matched == 0U) {
      continue;
    }
    if (se[0] != (uint8_t)k_exfat_entry_name) {
      matched = 0U;
      continue;
    }
    const uint32_t cpos = (k - 1U) * (uint32_t)k_exfat_name_per_entry;
    if (priv_exfat_name_chunk_eq(se, path, cpos, nlen) == 0U) {
      matched = 0U;
    }
  }
  *out_match = matched;
  return k_ra8_ok;
}

/** @brief Implementation of `priv_exfat_find_set()` -- one aligned walk of the root. */
ra8_err_t priv_exfat_find_set(const ra8_fs_mount_t* m,
                              const char*           path,
                              exfat_setpos_t*       pos,
                              uint32_t              max_pos,
                              uint32_t*             out_count,
                              uint8_t*              file_copy,
                              uint8_t*              strm_copy)
{
  /* Strip leading slashes so a "/name" path matches (#93), as FAT does. */
  while (*path == '/') {
    path++;
  }
  exfat_cursor_t cur  = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  const uint32_t nlen = priv_strlen(path);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    const exfat_setpos_t at = {.cluster = cur.cluster, .index = cur.entry_in_cluster};
    uint8_t              e[k_exfat_entry_bytes] = {};
    ra8_err_t            r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_err_not_found;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    const uint32_t sc    = (uint32_t)e[k_exfat_off_file_secnt];
    const uint32_t total = 1U + sc;
    if (total > max_pos) {
      return k_ra8_err_no_mem;
    }
    pos[0] = at;
    priv_byte_copy(file_copy, e, (uint32_t)k_exfat_entry_bytes);
    uint8_t matched = 0U;
    r               = priv_exfat_take_set(m, &cur, path, nlen, sc, pos, strm_copy, &matched);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (matched == 1U) {
      *out_count = total;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found; /* GCOVR_EXCL_LINE */
}

/**
 * @brief Rewrite one 32-byte directory entry at a recorded position.
 *
 * @details Thin wrapper over ::priv_exfat_write_dir_set for a single entry.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] where Entry position from ::priv_exfat_find_set.
 * @param[in] entry The 32 bytes to write.
 * @return Error code.
 * @retval k_ra8_ok    Entry rewritten.
 * @retval k_ra8_err_* Backend read/write failure.
 * @pre @p m and @p entry are non-NULL.
 * @pre @p where came from ::priv_exfat_find_set.
 * @post The on-disk entry equals @p entry.
 * @post No other directory bytes change.
 * @note Read-modify-write of one sector.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_put_entry(const ra8_fs_mount_t* m, const exfat_setpos_t* where, const uint8_t* entry)
{
  return priv_exfat_write_dir_set(m,
                                  where->cluster,
                                  where->index,
                                  entry,
                                  (uint32_t)k_exfat_entry_bytes);
}

/** @brief Implementation of `priv_exfat_free_clusters()` -- run or chain, bitmap only. */
ra8_err_t priv_exfat_free_clusters(const ra8_fs_mount_t* m, const uint8_t* strm)
{
  const uint32_t first = priv_rd32(&strm[k_exfat_strm_off_clus]);
  const uint32_t size  = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  if (first < k_cluster_first_data) {
    return k_ra8_ok;
  }
  if (size == 0U) {
    return k_ra8_ok;
  }
  uint32_t  bmp_lba = 0U;
  ra8_err_t e       = priv_exfat_bitmap_lba(m, &bmp_lba);
  if (e != k_ra8_ok) {
    return e; /* GCOVR_EXCL_LINE */
  }
  const uint32_t cluster_bytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  const uint8_t  nofat =
    ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  if (nofat == 1U) {
    const uint32_t count = (size + cluster_bytes - 1U) / cluster_bytes;
    return priv_exfat_bitmap_clear(m, bmp_lba, first, count);
  }
  uint32_t clus = first;
  for (uint32_t guard = 0U; guard < m->count_of_clusters; guard++) {
    e = priv_exfat_bitmap_clear(m, bmp_lba, clus, 1U);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    uint32_t next = 0U;
    e             = priv_fat_get(m, clus, &next);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra8_ok;
    }
    clus = next;
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE */
}

/* `priv_exfat_unlink()`: see header for the documented contract. */
ra8_err_t priv_exfat_unlink(const ra8_fs_mount_t* m, const char* path)
{
  exfat_setpos_t pos[k_exfat_set_max_entries] = {};
  uint32_t       count                        = 0U;
  uint8_t        file_e[k_exfat_entry_bytes]  = {};
  uint8_t        strm_e[k_exfat_entry_bytes]  = {};
  ra8_err_t      e =
    priv_exfat_find_set(m, path, pos, (uint32_t)k_exfat_set_max_entries, &count, file_e, strm_e);
  if (e != k_ra8_ok) {
    return e;
  }
  /* A directory's entry set looks like a file's, so without this the chain
   * holding every child is handed to priv_exfat_free_clusters and the children
   * become unreachable allocated clusters -- silent, unrecoverable loss (#604). */
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t k = 0U; k < count; k++) {
    exfat_cursor_t one                        = {.cluster          = pos[k].cluster,
                                                 .entry_in_cluster = pos[k].index,
                                                 .scanned          = 0U};
    uint8_t        entry[k_exfat_entry_bytes] = {};
    e                                         = priv_exfat_next_entry(m, &one, entry);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    entry[0] = (uint8_t)(entry[0] & (uint8_t) ~(uint8_t)k_exfat_inuse_bit);
    e        = priv_exfat_put_entry(m, &pos[k], entry);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
  }
  return priv_exfat_free_clusters(m, strm_e);
}

/**
 * @brief Patch a 3-entry set for a new name and rewrite it in place.
 *
 * @details Updates the Stream entry's NameLength + NameHash, rebuilds the
 * single Name entry from @p new_path, recomputes the File entry's
 * SetChecksum over the whole set, and writes all three entries back at
 * their recorded positions.
 *
 * @param[in]     m        Mounted exFAT volume.
 * @param[in]     pos      Positions of the set's three entries.
 * @param[in,out] set      The 96-byte set (File + Stream + Name).
 * @param[in]     new_path Replacement name (<= 15 characters).
 * @param[in]     new_len  Length of @p new_path.
 * @return Error code.
 * @retval k_ra8_ok    All three entries rewritten.
 * @retval k_ra8_err_* Backend write failure.
 * @pre @p set holds the file's current File + Stream entries.
 * @pre @p new_len fits one Name entry.
 * @post On k_ra8_ok the on-disk set carries the new name + checksum.
 * @post @p set mirrors the on-disk bytes.
 * @note Helper of ::priv_exfat_rename (complexity split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_apply_rename(const ra8_fs_mount_t* m,
                                         const exfat_setpos_t* pos,
                                         uint8_t*              set,
                                         const char*           new_path,
                                         uint32_t              new_len)
{
  uint8_t* strm               = &set[k_exfat_entry_bytes];
  uint8_t* name               = &set[(size_t)2U * (size_t)k_exfat_entry_bytes];
  strm[k_exfat_strm_off_nlen] = (uint8_t)new_len;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(new_path, new_len));
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_entry_bytes; i++) {
    name[i] = 0U;
  }
  name[0] = (uint8_t)k_exfat_entry_name;
  for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
    if (c < new_len) {
      name[k_exfat_name_off + (c * 2U)] = (uint8_t)new_path[c];
    }
  }
  /* Access stamp only, and before the checksum that covers it. Same reasoning
   * as the FAT rename: the name moved, the bytes did not, so LastModified must
   * not move or every backup tool concludes the file changed (#601). The
   * create and modify stamps ride along untouched in `set`, which is the File
   * entry read back off the volume. */
  priv_exfat_file_stamp_access(set);
  priv_wr16(&set[k_exfat_off_file_csum],
            priv_exfat_set_checksum(set, (uint32_t)k_exfat_rename_bytes));
  for (uint32_t k = 0U; k < (uint32_t)k_exfat_rename_entries; k++) {
    const ra8_err_t e =
      priv_exfat_put_entry(m, &pos[k], &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
  }
  return k_ra8_ok;
}

/* `priv_exfat_rename()`: see header for the documented contract. */
ra8_err_t priv_exfat_rename(const ra8_fs_mount_t* m, const char* old_path, const char* new_path)
{
  /* Strip leading slashes on both paths so the old name matches and the new
   * name is stored without a slash (#93), consistent with create/find. */
  while (*old_path == '/') {
    old_path++;
  }
  while (*new_path == '/') {
    new_path++;
  }
  const uint32_t old_len = priv_strlen(old_path);
  const uint32_t new_len = priv_strlen(new_path);
  if (old_len > (uint32_t)k_exfat_name_per_entry) {
    return k_ra8_err_not_supported;
  }
  if (new_len > (uint32_t)k_exfat_name_per_entry) {
    return k_ra8_err_not_supported;
  }
  uint8_t e_strm[k_exfat_entry_bytes] = {};
  uint8_t e_attr                      = 0U;
  if (priv_exfat_find(m, new_path, e_strm, &e_attr) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  exfat_setpos_t  pos[k_exfat_set_max_entries] = {};
  uint32_t        count                        = 0U;
  uint8_t         set[k_exfat_rename_bytes]    = {};
  const ra8_err_t e                            = priv_exfat_find_set(m,
                                          old_path,
                                          pos,
                                          (uint32_t)k_exfat_set_max_entries,
                                          &count,
                                          &set[0],
                                          &set[k_exfat_entry_bytes]);
  if (e != k_ra8_ok) {
    return e;
  }
  if (count != (uint32_t)k_exfat_rename_entries) {
    return k_ra8_err_not_supported;
  }
  return priv_exfat_apply_rename(m, pos, set, new_path, new_len);
}

/**
 * @brief Consume a set's Name entries and assemble the ASCII name.
 *
 * @details Reads the @p sc - 1 secondaries that follow the Stream entry,
 * copying the low byte of each UTF-16 unit into @p name until @p nlen
 * characters (or the buffer cap) are gathered. Non-Name secondaries are
 * consumed and skipped so the caller's cursor stays aligned.
 *
 * @param[in]     m    Mounted exFAT volume.
 * @param[in,out] cur  Directory cursor (just past the Stream entry).
 * @param[in]     sc   SecondaryCount from the File entry.
 * @param[in]     nlen NameLength from the Stream entry.
 * @param[out]    name Receives the NUL-terminated ASCII name.
 * @param[in]     cap  Capacity of @p name in bytes.
 * @return Error code.
 * @retval k_ra8_ok    All secondaries consumed; @p name terminated.
 * @retval k_ra8_err_* Backend read failure mid-set.
 * @pre @p cur sits immediately after the set's Stream entry.
 * @pre @p cap is at least 1.
 * @post @p cur sits immediately after the set's last secondary.
 * @post @p name is NUL-terminated (possibly truncated).
 * @note Helper of ::priv_exfat_listdir (complexity split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_gather_name(const ra8_fs_mount_t* m,
                                        exfat_cursor_t*       cur,
                                        uint32_t              sc,
                                        uint32_t              nlen,
                                        char*                 name,
                                        uint32_t              cap)
{
  uint32_t written = 0U;
  for (uint32_t k = 1U; k < sc; k++) {
    uint8_t         ne[k_exfat_entry_bytes] = {};
    const ra8_err_t r                       = priv_exfat_next_entry(m, cur, ne);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (ne[0] != (uint8_t)k_exfat_entry_name) {
      continue;
    }
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t p = ((k - 1U) * (uint32_t)k_exfat_name_per_entry) + c;
      if (p >= nlen) {
        break;
      }
      if (written < (cap - 1U)) {
        name[written] = (char)ne[k_exfat_name_off + (c * 2U)];
        written++;
      }
    }
  }
  name[written] = '\0';
  return k_ra8_ok;
}

/* `priv_exfat_listdir()`: see header for the documented contract. */
ra8_err_t priv_exfat_listdir(const ra8_fs_mount_t* m, ra8_fs_listdir_cb_t cb, void* ctx)
{
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t   e[k_exfat_entry_bytes] = {};
    ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_ok;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    const uint32_t sc                        = (uint32_t)e[k_exfat_off_file_secnt];
    const uint8_t  attr                      = e[k_exfat_off_file_attr];
    uint8_t        strm[k_exfat_entry_bytes] = {};
    r                                        = priv_exfat_next_entry(m, &cur, strm);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (strm[0] != (uint8_t)k_exfat_entry_stream) {
      continue;
    }
    const uint32_t size                        = priv_rd32(&strm[k_exfat_strm_off_dlen]);
    const uint32_t nlen                        = (uint32_t)strm[k_exfat_strm_off_nlen];
    char           name[k_exfat_list_name_cap] = {};
    r = priv_exfat_gather_name(m, &cur, sc, nlen, name, (uint32_t)k_exfat_list_name_cap);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    cb(name, attr, size, ctx);
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE */
}
