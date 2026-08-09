/**
 * @file ra8_fs_fat_exfat_read.c
 * @brief exFAT directory scan and read-path open.
 *
 * @details
 * Linear cursor over 32-byte exFAT directory entries plus name matching
 * and the read-side open path.
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
 * exFAT support (read, whole-file write, mkfs)
 * ===========================================================================
 */

/* `priv_strlen()`: see header for the documented contract. */
uint32_t priv_strlen(const char* s)
{
  uint32_t n = 0U;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

/* `priv_ascii_upper()`: see header for the documented contract. */
char priv_ascii_upper(char c)
{
  if (c < 'a') {
    return c;
  }
  if (c > 'z') {
    return c;
  }
  return (char)(c - ('a' - 'A'));
}

/**
 * @brief Detect an exFAT volume from its boot sector.
 *
 * @details exFAT stamps the ASCII string "EXFAT   " (5 chars + 3 spaces) at
 * offset 3 of the VBR, where a FAT BPB carries OEM text.
 *
 * @param[in] buf Sector-0 (VBR) contents (>= 512 bytes).
 * @return 1 if @p buf is an exFAT VBR, else 0.
 * @retval 1 exFAT signature present.
 * @retval 0 Not exFAT.
 * @pre @p buf is non-NULL.
 * @pre @p buf holds at least one sector.
 * @post No state modified.
 * @post @p buf is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_exfat_is_volume(const uint8_t* buf)
{
  static const char sig[k_exfat_fsname_len] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_fsname_len; i++) {
    if (buf[(uint32_t)k_exfat_off_fsname + i] != (uint8_t)sig[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Parse an exFAT VBR into the mount's geometry fields.
 *
 * @details Maps exFAT's sector-relative region offsets + log2 geometry onto
 * the shared FAT fields so ::priv_cluster_to_lba / ::priv_fat_get (FAT32 path)
 * serve the data + FAT regions unchanged. Only 512-byte sectors are supported.
 *
 * @param[in,out] m   Mount with backend bound and base LBA set.
 * @param[in]     buf VBR contents (sector 0 at the volume base).
 * @return Error code.
 * @retval k_ra8_ok                exFAT geometry stored; ``m->type`` set.
 * @retval k_ra8_err_not_supported BytesPerSectorShift is not 9 (512 B).
 * @pre @p m and @p buf are non-NULL.
 * @pre @p buf is a validated exFAT VBR (::priv_exfat_is_volume true).
 * @post On success ``m`` carries the exFAT region geometry.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe; serialize mount operations.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_parse(ra8_fs_mount_t* m, const uint8_t* buf)
{
  if (buf[k_exfat_off_bps_shift] != (uint8_t)k_exfat_bps_shift_512) {
    return k_ra8_err_not_supported;
  }
  m->type                = k_ra8_fs_type_exfat;
  m->bytes_per_sector    = k_ra8_fs_bytes_per_sector;
  m->sectors_per_cluster = 1U << buf[k_exfat_off_spc_shift];
  m->num_fats            = (uint32_t)buf[k_exfat_off_num_fats];
  m->fat_size_sectors    = priv_rd32(&buf[k_exfat_off_fat_len]);
  m->first_fat_lba       = priv_rd32(&buf[k_exfat_off_fat_lba]);
  m->first_data_lba      = priv_rd32(&buf[k_exfat_off_heap_lba]);
  m->root_cluster        = priv_rd32(&buf[k_exfat_off_root_clus]);
  m->count_of_clusters   = priv_rd32(&buf[k_exfat_off_clus_count]);
  m->reserved_sectors    = 0U;
  m->root_entries        = 0U;
  m->first_root_lba      = 0U;
  m->total_sectors       = 0U;
  /* Last, because it walks the root directory and so needs every field above.
   * It cannot fail the mount: a volume it could not interrogate is simply one
   * whose fold this build will not vouch for (#606). */
  priv_exfat_upcase_verify(m);
  return k_ra8_ok;
}

/* `priv_exfat_upcase_verify()`: see header for the documented contract. */
void priv_exfat_upcase_verify(ra8_fs_mount_t* m)
{
  m->exfat_upcase_ok = 0U;
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t         e[k_exfat_entry_bytes] = {};
    const ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra8_ok) {
      return;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return;
    }
    if (e[0] != (uint8_t)k_exfat_entry_upcase) {
      continue;
    }
    if (priv_rd32(&e[k_exfat_upc_off_csum]) == priv_exfat_upcase_checksum()) {
      m->exfat_upcase_ok = 1U;
    }
    return;
  }
  /* Falls out of the scan loop only after 65536 directory entries ahead of the
   * up-case one, which no volume this driver mounts has. The loop's own exits
   * (the up-case match and the EOD marker) are the real ends; there is no
   * trailing return, because a void function does not need one and clang-tidy
   * flags it if it has one. */
}

/* `priv_parse_volume()`: see header for the documented contract. */
ra8_err_t priv_parse_volume(ra8_fs_mount_t* m)
{
  if (priv_exfat_is_volume(s_scratch) != 0U) {
    return priv_exfat_parse(m, s_scratch);
  }
  return priv_parse_bpb_into_mount(m);
}

/* `priv_exfat_dir_root()`: see header for the documented contract. */
void priv_exfat_dir_root(const ra8_fs_mount_t* m, exfat_dir_t* out)
{
  out->cluster    = m->root_cluster;
  out->contig_end = 0U; /* the root is always FAT-chained; the format has no other shape */
  /* The root owns no File-entry set: its extent is the boot sector's FAT chain,
   * not a Stream entry, so ::priv_exfat_grow_dir extends the chain and patches
   * no metadata. 0 is the "no entry set" sentinel (#677). */
  out->self_cluster = 0U;
  out->self_index   = 0U;
}

/* `priv_exfat_dir_from_set()`: see header for the documented contract. */
void priv_exfat_dir_from_set(const ra8_fs_mount_t* m, const uint8_t* strm, exfat_dir_t* out)
{
  out->cluster    = priv_rd32(&strm[k_exfat_strm_off_clus]);
  out->contig_end = 0U;
  if ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) == 0U) {
    return;
  }
  const uint32_t cbytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  const uint32_t size   = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  out->contig_end       = out->cluster + ((size + cbytes - 1U) / cbytes);
}

/* `priv_exfat_cursor_init()`: see header for the documented contract. */
void priv_exfat_cursor_init(const exfat_dir_t* dir, exfat_cursor_t* out)
{
  out->cluster          = dir->cluster;
  out->entry_in_cluster = 0U;
  out->scanned          = 0U;
  out->contig_end       = dir->contig_end;
}

/* `priv_exfat_step_cluster()`: see header for the documented contract. */
ra8_err_t priv_exfat_step_cluster(const ra8_fs_mount_t* m,
                                  uint32_t              cluster,
                                  uint32_t              contig_end,
                                  uint32_t*             out_next)
{
  if (contig_end != 0U) {
    const uint32_t adjacent = cluster + 1U;
    if (adjacent >= contig_end) {
      return k_ra8_err_not_found;
    }
    *out_next = adjacent;
    return k_ra8_ok;
  }
  uint32_t        next = 0U;
  const ra8_err_t e    = priv_fat_get(m, cluster, &next);
  if (e != k_ra8_ok) {
    return e;
  }
  if (priv_is_eoc(m, next) != 0U) {
    return k_ra8_err_not_found;
  }
  *out_next = next;
  return k_ra8_ok;
}

/* `priv_exfat_next_entry()`: see header for the documented contract. */
ra8_err_t priv_exfat_next_entry(const ra8_fs_mount_t* m, exfat_cursor_t* cur, uint8_t* out)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra8_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  if (cur->entry_in_cluster >= per_cluster) {
    uint32_t        next = 0U;
    const ra8_err_t se   = priv_exfat_step_cluster(m, cur->cluster, cur->contig_end, &next);
    if (se != k_ra8_ok) {
      return se;
    }
    cur->cluster          = next;
    cur->entry_in_cluster = 0U;
  }
  const uint32_t byte_off = cur->entry_in_cluster * (uint32_t)k_exfat_entry_bytes;
  const uint32_t lba =
    priv_cluster_to_lba(m, cur->cluster) + (byte_off / k_ra8_fs_bytes_per_sector);
  uint8_t   sec[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t e                              = priv_read_sector(m, lba, sec);
  if (e != k_ra8_ok) {
    return e;
  }
  priv_byte_copy(out, &sec[byte_off % k_ra8_fs_bytes_per_sector], (uint32_t)k_exfat_entry_bytes);
  cur->entry_in_cluster++;
  cur->scanned++;
  return k_ra8_ok;
}

/* `priv_exfat_name_chunk_eq()`: see header for the documented contract. */
uint8_t
priv_exfat_name_chunk_eq(const uint8_t* entry, const uint16_t* name, uint32_t pos, uint32_t nlen)
{
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_name_per_entry; i++) {
    if ((pos + i) >= nlen) {
      return 1U;
    }
    const uint32_t b    = (uint32_t)k_exfat_name_off + (i * 2U);
    const uint16_t unit = priv_rd16(&entry[b]);
    if (priv_exfat_upcase_unit(unit) != priv_exfat_upcase_unit(name[pos + i])) {
      return 0U;
    }
  }
  return 1U;
}

/* `priv_exfat_name_to_units()`: see header for the documented contract. */
ra8_err_t priv_exfat_name_to_units(const ra8_fs_mount_t* m,
                                   const char*           path,
                                   uint16_t*             out,
                                   uint32_t*             out_units)
{
  const ra8_err_t err = priv_utf8_to_utf16(path, out, (uint32_t)k_exfat_name_cap, out_units);
  if (err != k_ra8_ok) {
    return err;
  }
  /* An ASCII name folds the same way under every conforming up-case table, so
   * it never depends on the volume carrying the one this build embeds. Anything
   * else does, and a NameHash computed with the wrong fold is a disagreement
   * with the on-disk format rather than a limitation of this API (#606). */
  if (m->exfat_upcase_ok != 0U) {
    return k_ra8_ok;
  }
  if (priv_utf16_all_ascii(out, *out_units) != 0U) {
    return k_ra8_ok;
  }
  *out_units = 0U;
  return k_ra8_err_not_supported;
}

/* `priv_exfat_needle_units()`: see header for the documented contract. */
ra8_err_t priv_exfat_needle_units(const ra8_fs_mount_t* m,
                                  const char*           name,
                                  uint16_t*             out,
                                  uint32_t*             out_units,
                                  ra8_err_t             too_long)
{
  const ra8_err_t e = priv_exfat_name_to_units(m, name, out, out_units);
  return (e == k_ra8_err_no_mem) ? too_long : e;
}

/**
 * @brief Read a stream-ext + name set and test it against @p path.
 *
 * @details Called with @p cur positioned just after a 0x85 File entry. Reads
 * the 0xC0 stream-extension entry and the following 0xC1 name entries; on a
 * full match hands the WHOLE Stream entry back rather than a chosen few of its
 * fields. That is the cheaper contract for everyone: the callers want
 * different subsets of it -- `stat` the length and cluster, an open also
 * `ValidDataLength` and the flags -- and a copy of 32 bytes they already had
 * in hand costs less than another out-parameter each time one is needed.
 *
 * @param[in]     m        Mounted exFAT volume.
 * @param[in,out] cur      Cursor (advanced past the consumed entries).
 * @param[in]     name     Target name as UTF-16 code units.
 * @param[in]     nlen     Number of units in @p name.
 * @param[out]    out_strm Receives the matched 32-byte Stream-extension entry.
 * @return Error code.
 * @retval k_ra8_ok            Match; @p out_strm populated.
 * @retval k_ra8_err_not_found This set is not @p name.
 * @retval k_ra8_err_*         Backend read failure.
 * @pre All pointers are non-NULL; @p cur follows a 0x85 entry.
 * @pre @p name is a flat (root-level) name, already converted to units.
 * @post On match @p out_strm mirrors the on-disk Stream entry.
 * @post On non-match @p out_strm is untouched.
 * @note Leftover name entries self-heal in ::priv_exfat_find.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_match_set(const ra8_fs_mount_t* m,
                                      exfat_cursor_t*       cur,
                                      const uint16_t*       name,
                                      uint32_t              nlen,
                                      uint8_t*              out_strm)
{
  uint8_t   strm[k_exfat_entry_bytes] = {};
  ra8_err_t e                         = priv_exfat_next_entry(m, cur, strm);
  if (e != k_ra8_ok) {
    return e;
  }
  if (strm[0] != (uint8_t)k_exfat_entry_stream) {
    return k_ra8_err_not_found;
  }
  if ((uint32_t)strm[k_exfat_strm_off_nlen] != nlen) {
    return k_ra8_err_not_found;
  }
  for (uint32_t pos = 0U; pos < nlen; pos += (uint32_t)k_exfat_name_per_entry) {
    uint8_t nm[k_exfat_entry_bytes] = {};
    e                               = priv_exfat_next_entry(m, cur, nm);
    if (e != k_ra8_ok) {
      return e;
    }
    if (nm[0] != (uint8_t)k_exfat_entry_name) {
      return k_ra8_err_not_found;
    }
    if (priv_exfat_name_chunk_eq(nm, name, pos, nlen) == 0U) {
      return k_ra8_err_not_found;
    }
  }
  priv_byte_copy(out_strm, strm, (uint32_t)k_exfat_entry_bytes);
  return k_ra8_ok;
}

/* `priv_exfat_find()`: see header for the documented contract. */
ra8_err_t priv_exfat_find(const ra8_fs_mount_t* m,
                          const exfat_dir_t*    dir,
                          const char*           path,
                          uint8_t*              out_strm,
                          uint8_t*              out_attr)
{
  /* Leading slashes are not part of the name; match FAT's priv_path_to_83
   * behavior so ra8_fs_open("/name") resolves on exFAT too (#93). */
  while (*path == '/') {
    path++;
  }
  uint16_t        need[k_exfat_name_cap] = {};
  uint32_t        nlen                   = 0U;
  const ra8_err_t ne                     = priv_exfat_name_to_units(m, path, need, &nlen);
  if (ne == k_ra8_err_no_mem) {
    /* Longer than any name this volume can hold, so it is genuinely absent --
     * a lookup says so rather than reporting a resource failure. */
    return k_ra8_err_not_found;
  }
  if (ne != k_ra8_ok) {
    return ne;
  }
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(dir, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t   entry[k_exfat_entry_bytes] = {};
    ra8_err_t e                          = priv_exfat_next_entry(m, &cur, entry);
    if (e != k_ra8_ok) {
      return e;
    }
    if (entry[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_err_not_found;
    }
    if (entry[0] != (uint8_t)k_exfat_entry_file) {
      continue;
    }
    e = priv_exfat_match_set(m, &cur, need, nlen, out_strm);
    if (e == k_ra8_ok) {
      /* FileAttributes is 16-bit but every bit we act on (directory, archive)
       * lives in the low byte, so the caller gets that byte. */
      *out_attr = entry[k_exfat_off_file_attr];
      return k_ra8_ok;
    }
    if (e != k_ra8_err_not_found) {
      return e;
    }
  }
  return k_ra8_err_not_found; /* GCOVR_EXCL_LINE -- 65536 entries required */
}

/**
 * @brief Populate a read handle from the file's Stream-extension entry.
 *
 * @details Everything a READ needs and nothing a write does: the chain head,
 *          the two lengths, and the contiguity flag. The streaming fields stay
 *          zeroed because a read handle never grows the file -- but
 *          `valid_bytes` is not one of them, because the bytes past it must
 *          read as zero (exFAT spec sec 7.4.5) rather than as whatever the
 *          previous tenant of those clusters left.
 *
 * @param[out] file   Freshly claimed file slot.
 * @param[in]  handle Owning mount.
 * @param[in]  strm   The file's 32-byte Stream-extension entry.
 *
 * @return Nothing.
 *
 * @pre All pointers are non-NULL and @p strm came from ::priv_exfat_find.
 * @pre The slot is not already in use.
 * @post @p file is in use, positioned at byte 0, in read mode.
 * @post The read accelerator is seeded at the chain head.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_exfat_seed_read(ra8_fs_file_t* file, ra8_fs_mount_t* handle, const uint8_t* strm)
{
  const uint32_t first     = priv_rd32(&strm[k_exfat_strm_off_clus]);
  file->mount              = handle;
  file->first_cluster      = first;
  file->cur_cluster        = first;
  file->walk_cache_idx     = 0U; /* read accelerator seeded at the chain head */
  file->walk_cache_cluster = first;
  file->size_bytes         = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  file->offset             = 0U;
  file->dir_entry_lba      = 0U;
  file->dir_entry_idx      = 0U;
  file->valid_bytes        = priv_rd32(&strm[k_exfat_off_strm_valid]);
  file->entry_set_cluster  = 0U;
  file->entry_set_index    = 0U;
  file->entry_set_count    = 0U;
  file->alloc_clusters     = 0U;
  file->tail_cluster       = 0U;
  file->mode               = k_ra8_fs_mode_read;
  file->no_fat_chain =
    ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  file->in_use = 1U;
  file->dirty  = 0U;
}

/* `priv_exfat_open()`: see header for the documented contract. */
ra8_err_t priv_exfat_open(ra8_fs_mount_t* handle,
                          const char*     path,
                          ra8_fs_mode_t   mode,
                          ra8_fs_file_t** out_file)
{
  /* Writing modes need the file's entry-set coordinates, its allocation and
   * its ValidDataLength -- none of which a read open has any use for -- so
   * they get their own path rather than a second half bolted onto this one
   * (#602). */
  if (mode != k_ra8_fs_mode_read) {
    return priv_exfat_open_write(handle, path, mode, out_file);
  }
  uint8_t         strm[k_exfat_entry_bytes] = {};
  uint8_t         attr                      = 0U;
  const ra8_err_t e                         = priv_exfat_lookup(handle, path, strm, &attr);
  if (e != k_ra8_ok) {
    return e;
  }
  /* A directory answers the name lookup exactly like a file and reports
   * DataLength 0, so without this it opens as a bogus empty file (#604). */
  if ((attr & (uint8_t)k_exfat_attr_directory) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  priv_exfat_seed_read(f, handle, strm);
  *out_file = f;
  return k_ra8_ok;
}
