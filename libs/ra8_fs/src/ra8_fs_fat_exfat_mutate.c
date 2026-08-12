/**
 * @file ra8_fs_fat_exfat_mutate.c
 * @brief exFAT unlink / rename / listdir mutation helpers.
 *
 * @details
 * Locates a directory-entry set, frees its clusters and bitmap bits, and
 * renames it -- rewriting the whole set under the new name, in place when the
 * entry count is unchanged and by relocation when the name needs more or fewer
 * Name entries -- plus the exFAT directory listing.
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

/* `priv_exfat_bitmap_clear()`: see header for the documented contract. */
ra8_err_t
priv_exfat_bitmap_clear(const ra8_fs_mount_t* m, uint64_t bmp_lba, uint32_t clus, uint32_t count)
{
  uint64_t       loaded = UINT64_MAX;
  uint8_t* const sec    = priv_sec_io();
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t idx  = (clus - k_cluster_first_data) + k;
    const uint64_t lba  = bmp_lba + ((idx >> k_exfat_bit_shift) / priv_bps(m));
    const uint32_t byte = (idx >> k_exfat_bit_shift) % priv_bps(m);
    const uint32_t bit  = idx & k_exfat_bit_mask;
    ra8_err_t      e    = priv_exfat_bmp_switch(m, lba, &loaded, sec);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    sec[byte] = (uint8_t)(sec[byte] & (uint8_t)~(uint8_t)(1U << bit));
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
 * @param[in]     name      Target name as UTF-16 code units.
 * @param[in]     nlen      Number of units in @p name.
 * @param[in]     sc        SecondaryCount from the File entry.
 * @param[out]    pos       Position array (slot 0 already holds the File).
 * @param[out]    strm_copy Receives the 32-byte Stream entry when matched.
 * @param[out]    out_match Receives 1 when the whole set matches @p name.
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
                                     const uint16_t*       name,
                                     uint32_t              nlen,
                                     uint32_t              sc,
                                     exfat_setpos_t*       pos,
                                     uint8_t*              strm_copy,
                                     uint8_t*              out_match)
{
  uint8_t matched = 1U;
  for (uint32_t k = 0U; k < sc; k++) {
    uint8_t         se[k_exfat_entry_bytes] = {};
    const ra8_err_t r                       = priv_exfat_next_entry(m, cur, se);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    /* AFTER the read: ::priv_exfat_next_entry may cross a cluster boundary before
     * reading, so the entry lives at the cursor's CURRENT cluster, one slot back
     * (it has already advanced past what it read). Capturing before the call
     * records (old cluster, one-past-end) for the first entry of a new cluster --
     * harmless while directories were one cluster, a mis-retired slot once they
     * grow (#677). */
    const exfat_setpos_t sp = {.cluster = cur->cluster, .index = cur->entry_in_cluster - 1U};
    pos[1U + k]             = sp;
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
    if (priv_exfat_name_chunk_eq(se, name, cpos, nlen) == 0U) {
      matched = 0U;
    }
  }
  *out_match = matched;
  return k_ra8_ok;
}

/**
 * @brief Test one File entry set against a needle, recording its positions.
 *
 * @details The per-File-entry body of ::priv_exfat_find_set, extracted so the
 * walk stays under the statement-count gate. On a match it fills @p pos and
 * @p out_count; otherwise it reports ::k_ra8_err_not_found so the caller keeps
 * scanning.
 *
 * @param[in]     m         Mounted exFAT volume.
 * @param[in,out] cur       Cursor positioned just after the File entry @p e.
 * @param[in]     at        Position of @p e itself.
 * @param[in]     e         The 32-byte File entry.
 * @param[in]     need      Needle name as UTF-16 units.
 * @param[in]     nlen      Number of units in @p need.
 * @param[out]    pos       Receives the set's positions (File entry first).
 * @param[in]     max_pos   Capacity of @p pos.
 * @param[out]    out_count Receives the entry count on a match.
 * @param[out]    file_copy Receives the 32-byte File entry.
 * @param[out]    strm_copy Receives the 32-byte Stream-extension entry.
 * @return Error code.
 * @retval k_ra8_ok            This set matches; outputs populated.
 * @retval k_ra8_err_not_found This set is not @p need; keep scanning.
 * @retval k_ra8_err_no_mem    The set has more entries than @p max_pos.
 * @retval k_ra8_err_*         Backend read failure mid-set.
 * @pre All pointers are non-NULL; @p e[0] is ::k_exfat_entry_file.
 * @pre @p cur sits immediately after @p e.
 * @post @p cur sits past the set's last secondary.
 * @post On a match @p out_count is 1 + SecondaryCount.
 * @note Helper of ::priv_exfat_find_set (statement-count split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_try_set(const ra8_fs_mount_t* m,
                                    exfat_cursor_t*       cur,
                                    exfat_setpos_t        at,
                                    const uint8_t*        e,
                                    const uint16_t*       need,
                                    uint32_t              nlen,
                                    exfat_setpos_t*       pos,
                                    uint32_t              max_pos,
                                    uint32_t*             out_count,
                                    uint8_t*              file_copy,
                                    uint8_t*              strm_copy)
{
  const uint32_t sc    = (uint32_t)e[k_exfat_off_file_secnt];
  const uint32_t total = 1U + sc;
  if (total > max_pos) {
    return k_ra8_err_no_mem;
  }
  pos[0] = at;
  priv_byte_copy(file_copy, e, (uint32_t)k_exfat_entry_bytes);
  uint8_t         matched = 0U;
  const ra8_err_t r       = priv_exfat_take_set(m, cur, need, nlen, sc, pos, strm_copy, &matched);
  if (r != k_ra8_ok) {
    return r; /* GCOVR_EXCL_LINE */
  }
  if (matched == 1U) {
    *out_count = total;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/** @brief Implementation of `priv_exfat_find_set()` -- one aligned walk of a directory. */
ra8_err_t priv_exfat_find_set(const ra8_fs_mount_t* m,
                              const exfat_dir_t*    dir,
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
  uint16_t        need[k_exfat_name_cap] = {};
  uint32_t        nlen                   = 0U;
  const ra8_err_t ne = priv_exfat_needle_units(m, path, need, &nlen, k_ra8_err_not_found);
  if (ne != k_ra8_ok) {
    return ne;
  }
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(dir, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t   e[k_exfat_entry_bytes] = {};
    ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    /* AFTER the read (see ::priv_exfat_take_set): the File entry lives at the
     * cursor's current cluster, one slot back, so a set whose File entry is the
     * first of a grown cluster is recorded where it truly is (#677). */
    const exfat_setpos_t at = {.cluster = cur.cluster, .index = cur.entry_in_cluster - 1U};
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_err_not_found;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    r =
      priv_exfat_try_set(m, &cur, at, e, need, nlen, pos, max_pos, out_count, file_copy, strm_copy);
    if (r != k_ra8_err_not_found) {
      return r;
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
  /* The full 64-bit DataLength: reading only the low word here under-counted a
   * >4 GiB file's clusters and leaked the rest on unlink/truncate (#676). */
  const uint64_t size = priv_rd64(&strm[k_exfat_strm_off_dlen]);
  if (first < k_cluster_first_data) {
    return k_ra8_ok;
  }
  if (size == 0U) {
    return k_ra8_ok;
  }
  uint64_t  bmp_lba = 0U;
  ra8_err_t e       = priv_exfat_bitmap_lba(m, &bmp_lba);
  if (e != k_ra8_ok) {
    return e; /* GCOVR_EXCL_LINE */
  }
  const uint32_t cluster_bytes = priv_cluster_bytes(m);
  const uint8_t  nofat =
    ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  if (nofat == 1U) {
    const uint32_t count = (uint32_t)((size + cluster_bytes - 1U) / cluster_bytes);
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

/* `priv_exfat_drop_set()`: see header for the documented contract. */
ra8_err_t priv_exfat_drop_set(const ra8_fs_mount_t* m, const exfat_setpos_t* pos, uint32_t count)
{
  for (uint32_t k = 0U; k < count; k++) {
    /* A single-entry read at a recorded position: contig_end is irrelevant
     * because this cursor never advances past the entry it was aimed at. */
    exfat_cursor_t one                        = {.cluster          = pos[k].cluster,
                                                 .entry_in_cluster = pos[k].index,
                                                 .scanned          = 0U,
                                                 .contig_end       = 0U};
    uint8_t        entry[k_exfat_entry_bytes] = {};
    ra8_err_t      e                          = priv_exfat_next_entry(m, &one, entry);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
    entry[0] = (uint8_t)(entry[0] & (uint8_t)~(uint8_t)k_exfat_inuse_bit);
    e        = priv_exfat_put_entry(m, &pos[k], entry);
    if (e != k_ra8_ok) {
      return e; /* GCOVR_EXCL_LINE */
    }
  }
  return k_ra8_ok;
}

/* `priv_exfat_unlink_at()`: see header for the documented contract. */
ra8_err_t priv_exfat_unlink_at(const ra8_fs_mount_t* m, const exfat_dir_t* dir, const char* name)
{
  exfat_setpos_t  pos[k_exfat_set_max_entries] = {};
  uint32_t        count                        = 0U;
  uint8_t         file_e[k_exfat_entry_bytes]  = {};
  uint8_t         strm_e[k_exfat_entry_bytes]  = {};
  const ra8_err_t e = priv_exfat_find_set(m,
                                          dir,
                                          name,
                                          pos,
                                          (uint32_t)k_exfat_set_max_entries,
                                          &count,
                                          file_e,
                                          strm_e);
  if (e != k_ra8_ok) {
    return e;
  }
  /* A directory's entry set looks like a file's, so without this the chain
   * holding every child is handed to priv_exfat_free_clusters and the children
   * become unreachable allocated clusters -- silent, unrecoverable loss (#604). */
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* Honor the read-only attribute (#681): a read-only file is refused before the
   * entry set is dropped or its clusters freed, so a denied unlink is a no-op. */
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_read_only) != 0U) {
    return k_ra8_err_access_denied;
  }
  const ra8_err_t de = priv_exfat_drop_set(m, pos, count);
  if (de != k_ra8_ok) {
    return de; /* GCOVR_EXCL_LINE */
  }
  return priv_exfat_free_clusters(m, strm_e);
}

/* `priv_exfat_unlink()`: see header for the documented contract. */
ra8_err_t priv_exfat_unlink(const ra8_fs_mount_t* m, const char* path)
{
  exfat_dir_t     parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t e      = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (e != k_ra8_ok) {
    return e;
  }
  if (priv_strlen(leaf) == 0U) {
    return k_ra8_err_invalid_arg; /* "/" names the root, which is not a file */
  }
  return priv_exfat_unlink_at(m, &parent, leaf);
}

/**
 * @brief Assemble a file's directory set under a new name, keeping its data.
 *
 * @details Starts from the file's current File + Stream entries, so every field
 * that is NOT the name rides across untouched: the create / modify timestamps,
 * the attributes, `FirstCluster`, `DataLength`, `ValidDataLength` and the
 * secondary flags. It then overwrites exactly what a rename changes -- the File
 * entry's `SecondaryCount`, the Stream entry's `NameLength` + `NameHash`, and a
 * run of Name entries carrying fifteen UTF-16 units apiece. The access stamp is
 * refreshed just before the `SetChecksum` is recomputed over the whole set.
 *
 * A name of 16+ units needs more than one Name entry, so the built set can be
 * longer OR shorter than the one on the volume. That is precisely why the old
 * in-place patch could not do this and ::priv_exfat_place_rename has to decide
 * where the result lands (#603).
 *
 * @param[out] set      Buffer of at least ::k_exfat_max_set_bytes.
 * @param[in]  file_e   The file's current 32-byte File entry.
 * @param[in]  strm_e   The file's current 32-byte Stream-extension entry.
 * @param[in]  new_name Replacement name as UTF-16 code units.
 * @param[in]  new_len  Number of units in @p new_name (1..::k_exfat_name_cap).
 * @return The total byte length of the built set.
 * @retval >0 Number of bytes written into @p set.
 * @pre @p set, @p file_e, @p strm_e and @p new_name are non-NULL.
 * @pre @p new_len is at least 1 and at most ::k_exfat_name_cap.
 * @post @p set holds a complete entry set with a valid SetChecksum.
 * @post No volume state is modified.
 * @note Access stamp only; the create + modify stamps ride along in @p file_e.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_exfat_build_rename_set(uint8_t*        set,
                                            const uint8_t*  file_e,
                                            const uint8_t*  strm_e,
                                            const uint16_t* new_name,
                                            uint32_t        new_len)
{
  const uint32_t name_entries =
    (new_len + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t sec_count = 1U + name_entries;
  const uint32_t total     = (1U + sec_count) * (uint32_t)k_exfat_entry_bytes;
  for (uint32_t i = 0U; i < total; i++) {
    set[i] = 0U;
  }
  priv_byte_copy(&set[0], file_e, (uint32_t)k_exfat_entry_bytes);
  priv_byte_copy(&set[k_exfat_entry_bytes], strm_e, (uint32_t)k_exfat_entry_bytes);
  set[k_exfat_off_file_secnt] = (uint8_t)sec_count;
  uint8_t* strm               = &set[k_exfat_entry_bytes];
  strm[k_exfat_strm_off_nlen] = (uint8_t)new_len;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(new_name, new_len));
  for (uint32_t n = 0U; n < name_entries; n++) {
    uint8_t* ne = &set[(size_t)(2U + n) * (size_t)k_exfat_entry_bytes];
    ne[0]       = (uint8_t)k_exfat_entry_name;
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t p = (n * (uint32_t)k_exfat_name_per_entry) + c;
      if (p < new_len) {
        priv_wr16(&ne[k_exfat_name_off + (c * 2U)], new_name[p]);
      }
    }
  }
  /* Access stamp only, and before the checksum that covers it. Same reasoning
   * as the FAT rename: the name moved, the bytes did not, so LastModified must
   * not move or every backup tool concludes the file changed (#601). */
  priv_exfat_file_stamp_access(set);
  priv_wr16(&set[k_exfat_off_file_csum], priv_exfat_set_checksum(set, total));
  return total;
}

/**
 * @brief Resolve a rename's two paths to one shared parent plus two leaves.
 *
 * @details Resolves both parents and requires them to be the same directory:
 * this rename rewrites an entry set where it lies, so it cannot move one
 * between directories. Both leaves must be real names -- the volume root is
 * neither renameable nor a legal destination. Extracted from
 * ::priv_exfat_rename so that function stays inside the size gate, and the
 * exFAT mirror of ::priv_rename_prepare.
 *
 * @param[in]  m          Mounted exFAT volume.
 * @param[in]  old_path   Existing path.
 * @param[in]  new_path   Replacement path (same directory).
 * @param[out] out_parent Receives the shared parent directory.
 * @param[out] out_old    Receives a pointer to the old leaf name.
 * @param[out] out_new    Receives a pointer to the new leaf name.
 * @return Error code.
 * @retval k_ra8_ok                Resolved; outputs populated.
 * @retval k_ra8_err_invalid_arg   A path names the volume root.
 * @retval k_ra8_err_not_supported The two paths are in different directories.
 * @retval k_ra8_err_*             Resolution / backend failure.
 * @pre Every pointer argument is non-NULL; the mount is exFAT.
 * @pre Neither path is currently held open.
 * @post On success both leaves point into their caller's path strings.
 * @post No volume state is modified.
 * @note Not thread-safe; callers serialise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_rename_prepare(const ra8_fs_mount_t* m,
                                           const char*           old_path,
                                           const char*           new_path,
                                           exfat_dir_t*          out_parent,
                                           const char**          out_old,
                                           const char**          out_new)
{
  exfat_dir_t     op = {};
  const char*     ol = nullptr;
  const ra8_err_t e1 = priv_exfat_resolve_parent(m, old_path, &op, &ol);
  if (e1 != k_ra8_ok) {
    return e1;
  }
  exfat_dir_t     np = {};
  const char*     nl = nullptr;
  const ra8_err_t e2 = priv_exfat_resolve_parent(m, new_path, &np, &nl);
  if (e2 != k_ra8_ok) {
    return e2;
  }
  if (op.cluster != np.cluster) {
    return k_ra8_err_not_supported;
  }
  if (priv_strlen(ol) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_strlen(nl) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  *out_parent = op;
  *out_old    = ol;
  *out_new    = nl;
  return k_ra8_ok;
}

/**
 * @brief Write a rebuilt rename set, in place when it fits, else by relocation.
 *
 * @details When the new set has the SAME entry count as the old one, every slot
 * is rewritten where it already sits -- the cheap path a same-length rename
 * keeps, and the only path a checksum-and-name edit ever needed. Otherwise the
 * set changed length: a fresh run of @p new_count free slots is located (growing
 * the directory if it must), the whole set is written there, and only THEN is
 * the old set retired by clearing bit 7 of each of its type bytes.
 *
 * That order is the crash-safe one. The new set names the same clusters as the
 * old, so a failure after the write but before the drop leaves two names for one
 * file -- a transient the next scan resolves -- whereas dropping first would
 * orphan the clusters if the write never completed (#603).
 *
 * @param[in] m         Mounted exFAT volume.
 * @param[in] parent    Directory that holds the set.
 * @param[in] pos       Positions of the OLD set's entries.
 * @param[in] old_count Number of entries in the old set.
 * @param[in] set       The rebuilt set bytes (File + Stream + Name entries).
 * @param[in] bytes     Length of @p set; @p bytes / 32 is the new entry count.
 * @return Error code.
 * @retval k_ra8_ok         The set was placed and any old set retired.
 * @retval k_ra8_err_no_mem The directory has no room for a longer set.
 * @retval k_ra8_err_*      Backend read / write failure.
 * @pre @p pos came from ::priv_exfat_find_set for @p old_count entries.
 * @pre @p bytes is a positive multiple of ::k_exfat_entry_bytes.
 * @post On k_ra8_ok exactly one entry set answers to the new name.
 * @post On relocation the old set's slots read as deleted.
 * @note Helper of ::priv_exfat_rename (statement-count split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_place_rename(const ra8_fs_mount_t* m,
                                         const exfat_dir_t*    parent,
                                         const exfat_setpos_t* pos,
                                         uint32_t              old_count,
                                         const uint8_t*        set,
                                         uint32_t              bytes)
{
  const uint32_t new_count = bytes / (uint32_t)k_exfat_entry_bytes;
  if (new_count == old_count) {
    for (uint32_t k = 0U; k < new_count; k++) {
      const ra8_err_t e =
        priv_exfat_put_entry(m, &pos[k], &set[(size_t)k * (size_t)k_exfat_entry_bytes]);
      if (e != k_ra8_ok) {
        return e; /* GCOVR_EXCL_LINE */
      }
    }
    return k_ra8_ok;
  }
  uint32_t        nclus = 0U;
  uint32_t        nidx  = 0U;
  const ra8_err_t se    = priv_exfat_find_dir_space(m, parent, new_count, &nclus, &nidx);
  if (se != k_ra8_ok) {
    return se;
  }
  const ra8_err_t we = priv_exfat_write_dir_set(m, nclus, nidx, set, bytes);
  if (we != k_ra8_ok) {
    return we; /* GCOVR_EXCL_LINE */
  }
  return priv_exfat_drop_set(m, pos, old_count);
}

/* `priv_exfat_rename()`: see header for the documented contract. */
ra8_err_t priv_exfat_rename(const ra8_fs_mount_t* m, const char* old_path, const char* new_path)
{
  exfat_dir_t     parent   = {};
  const char*     old_name = nullptr;
  const char*     new_name = nullptr;
  const ra8_err_t pe =
    priv_exfat_rename_prepare(m, old_path, new_path, &parent, &old_name, &new_name);
  if (pe != k_ra8_ok) {
    return pe;
  }
  /* The new leaf in UTF-16 up front: the built Name entries and the NameHash
   * both count CODE UNITS, and a UTF-8 argument of the same unit count can be
   * three times as many bytes (#606). A name over the cap is an argument fault,
   * not a full disk. The old leaf stays UTF-8: ::priv_exfat_find_set converts it
   * itself, and the entry count it reports is all the placement below needs. */
  uint16_t        new_units[k_exfat_name_cap] = {};
  uint32_t        new_len                     = 0U;
  const ra8_err_t nue =
    priv_exfat_needle_units(m, new_name, new_units, &new_len, k_ra8_err_invalid_arg);
  if (nue != k_ra8_ok) {
    return nue;
  }
  uint8_t e_strm[k_exfat_entry_bytes] = {};
  uint8_t e_attr                      = 0U;
  if (priv_exfat_find(m, &parent, new_name, e_strm, &e_attr) == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  exfat_setpos_t  pos[k_exfat_set_max_entries] = {};
  uint32_t        count                        = 0U;
  uint8_t         file_e[k_exfat_entry_bytes]  = {};
  uint8_t         strm_e[k_exfat_entry_bytes]  = {};
  const ra8_err_t fe = priv_exfat_find_set(m,
                                           &parent,
                                           old_name,
                                           pos,
                                           (uint32_t)k_exfat_set_max_entries,
                                           &count,
                                           file_e,
                                           strm_e);
  if (fe != k_ra8_ok) {
    return fe;
  }
  /* Honor the read-only attribute (#681). file_e is the File entry read back by
   * priv_exfat_find_set above, so its FileAttributes low byte carries the
   * read-only bit; a read-only file is refused before the rename set is rebuilt
   * or placed. */
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_read_only) != 0U) {
    return k_ra8_err_access_denied;
  }
  uint8_t        set[k_exfat_max_set_bytes] = {};
  const uint32_t bytes = priv_exfat_build_rename_set(set, file_e, strm_e, new_units, new_len);
  return priv_exfat_place_rename(m, &parent, pos, count, set, bytes);
}

/**
 * @brief Consume a set's Name entries and assemble the name as UTF-8.
 *
 * @details Reads the @p sc - 1 secondaries that follow the Stream entry,
 * collecting whole UTF-16 code units, then converts the lot to UTF-8 in one
 * step. Non-Name secondaries are consumed and skipped so the caller's cursor
 * stays aligned.
 *
 * Collecting units and converting once is what makes the conversion possible at
 * all. The old loop copied the LOW BYTE of each unit straight into the output,
 * so U+00E9 came back as the single byte 0xE9 -- not valid UTF-8 -- and U+4F60
 * came back as a backtick (#606). A variable-width encoding cannot be produced
 * one fixed-width unit at a time into a byte-indexed buffer.
 *
 * A set whose units cannot be expressed in UTF-8 at all -- an unpaired
 * surrogate, which no conforming writer produces -- yields an empty name, and
 * ::priv_exfat_listdir reports nothing for that entry rather than a name that
 * would not re-open the file.
 *
 * @param[in]     m    Mounted exFAT volume.
 * @param[in,out] cur  Directory cursor (just past the Stream entry).
 * @param[in]     sc   SecondaryCount from the File entry.
 * @param[in]     nlen NameLength from the Stream entry, in UTF-16 units.
 * @param[out]    name Receives the NUL-terminated UTF-8 name.
 * @param[in]     cap  Capacity of @p name in bytes.
 * @return Error code.
 * @retval k_ra8_ok    All secondaries consumed; @p name terminated.
 * @retval k_ra8_err_* Backend read failure mid-set.
 * @pre @p cur sits immediately after the set's Stream entry.
 * @pre @p cap is at least 1.
 * @post @p cur sits immediately after the set's last secondary, whatever the
 *       name turned out to be -- an unconvertible one must not desynchronise
 *       the walk.
 * @post @p name is NUL-terminated, and empty when the units were not UTF-8.
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
  uint16_t units[k_exfat_name_cap] = {};
  uint32_t got                     = 0U;
  name[0]                          = '\0';
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
      if (got < (uint32_t)k_exfat_name_cap) {
        units[got] = priv_rd16(&ne[k_exfat_name_off + (c * 2U)]);
        got++;
      }
    }
  }
  if (priv_utf16_to_utf8(units, got, name, cap) != k_ra8_ok) {
    name[0] = '\0';
  }
  return k_ra8_ok;
}

/**
 * @brief Emit one File entry set to the listdir callback.
 *
 * @details The per-set body of ::priv_exfat_listdir, extracted so the walk
 * stays under the statement-count gate. Reads the Stream entry, decides the
 * size a directory reports (0, not its allocation), gathers the name as UTF-8
 * and fires @p cb -- unless the name is one no UTF-8 string encodes, which is
 * reported as nothing rather than as mojibake (#606).
 *
 * @param[in]     m    Mounted exFAT volume.
 * @param[in,out] cur  Cursor positioned just after the File entry.
 * @param[in]     e    The 32-byte File entry.
 * @param[in]     cb   Per-entry callback.
 * @param[in]     ctx  Opaque pointer forwarded to @p cb.
 * @return Error code.
 * @retval k_ra8_ok    The set was consumed (and emitted, unless nameless).
 * @retval k_ra8_err_* Backend read failure mid-set.
 * @pre All pointers are non-NULL; @p cur follows @p e.
 * @pre @p e[0] is ::k_exfat_entry_file.
 * @post @p cur sits past the set's last secondary.
 * @post @p cb ran at most once for this set.
 * @note Helper of ::priv_exfat_listdir (complexity split).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_list_emit(const ra8_fs_mount_t* m,
                                      exfat_cursor_t*       cur,
                                      const uint8_t*        e,
                                      ra8_fs_listdir_cb_t   cb,
                                      void*                 ctx)
{
  const uint32_t sc                        = (uint32_t)e[k_exfat_off_file_secnt];
  const uint8_t  attr                      = e[k_exfat_off_file_attr];
  uint8_t        strm[k_exfat_entry_bytes] = {};
  ra8_err_t      r                         = priv_exfat_next_entry(m, cur, strm);
  if (r != k_ra8_ok) {
    return r; /* GCOVR_EXCL_LINE */
  }
  if (strm[0] != (uint8_t)k_exfat_entry_stream) {
    return k_ra8_ok;
  }
  /* A directory's exFAT DataLength is its ALLOCATION, not a byte count, so
   * reporting it as a size would tell every caller that an empty folder held
   * a cluster's worth of bytes. FAT reports 0 for a directory and so does
   * ra8_fs_stat(); this is the third place that has to agree (#605). */
  uint64_t size = priv_rd64(&strm[k_exfat_strm_off_dlen]);
  if ((attr & (uint8_t)k_exfat_attr_directory) != 0U) {
    size = 0U;
  }
  const uint32_t nlen                      = (uint32_t)strm[k_exfat_strm_off_nlen];
  char           name[k_exfat_name_u8_cap] = {};
  r = priv_exfat_gather_name(m, cur, sc, nlen, name, (uint32_t)k_exfat_name_u8_cap);
  if (r != k_ra8_ok) {
    return r; /* GCOVR_EXCL_LINE */
  }
  if (name[0] == '\0') {
    return k_ra8_ok; /* units no UTF-8 string encodes: report nothing, not a lie */
  }
  cb(name, attr, size, ctx);
  return k_ra8_ok;
}

/* `priv_exfat_listdir()`: see header for the documented contract. */
ra8_err_t priv_exfat_listdir(const ra8_fs_mount_t* m,
                             const exfat_dir_t*    dir,
                             ra8_fs_listdir_cb_t   cb,
                             void*                 ctx)
{
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(dir, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t   e[k_exfat_entry_bytes] = {};
    ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r == k_ra8_err_not_found) {
      return k_ra8_ok; /* the directory's run ended without a 0x00 marker */
    }
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_ok;
    }
    if (e[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    r = priv_exfat_list_emit(m, &cur, e, cb, ctx);
    if (r != k_ra8_ok) {
      return r; /* GCOVR_EXCL_LINE */
    }
  }
  return k_ra8_ok; /* GCOVR_EXCL_LINE */
}
