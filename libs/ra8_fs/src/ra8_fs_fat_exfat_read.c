/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fs_fat_exfat_read.c
 * @brief exFAT directory scan and read-path open.
 *
 * @details
 * Linear cursor over 32-byte exFAT directory entries plus name matching
 * and the read-side open path.
 *
 *
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
  return k_ra8_ok;
}

/* `priv_parse_volume()`: see header for the documented contract. */
ra8_err_t priv_parse_volume(ra8_fs_mount_t* m)
{
  if (priv_exfat_is_volume(s_scratch) != 0U) {
    return priv_exfat_parse(m, s_scratch);
  }
  return priv_parse_bpb_into_mount(m);
}

/* `priv_exfat_next_entry()`: see header for the documented contract. */
ra8_err_t priv_exfat_next_entry(const ra8_fs_mount_t* m, exfat_cursor_t* cur, uint8_t* out)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra8_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  if (cur->entry_in_cluster >= per_cluster) {
    uint32_t  next = 0U;
    ra8_err_t e    = priv_fat_get(m, cur->cluster, &next);
    if (e != k_ra8_ok) {
      return e;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra8_err_not_found;
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
priv_exfat_name_chunk_eq(const uint8_t* entry, const char* path, uint32_t pos, uint32_t nlen)
{
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_name_per_entry; i++) {
    if ((pos + i) >= nlen) {
      return 1U;
    }
    const uint32_t b = (uint32_t)k_exfat_name_off + (i * 2U);
    if (entry[b + 1U] != 0U) {
      return 0U;
    }
    if (priv_ascii_upper((char)entry[b]) != priv_ascii_upper(path[pos + i])) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Read a stream-ext + name set and test it against @p path.
 *
 * @details Called with @p cur positioned just after a 0x85 File entry. Reads
 * the 0xC0 stream-extension entry and the following 0xC1 name entries; on a
 * full match fills the file's location fields.
 *
 * @param[in]     m         Mounted exFAT volume.
 * @param[in,out] cur       Cursor (advanced past the consumed entries).
 * @param[in]     path      Target path (ASCII, flat root name).
 * @param[out]    out_first First cluster of the matched file.
 * @param[out]    out_size  File length in bytes (low 32 bits).
 * @param[out]    out_nofat 1 if the file is contiguous (NoFatChain).
 * @return Error code.
 * @retval k_ra8_ok            Match; outputs populated.
 * @retval k_ra8_err_not_found This set is not @p path.
 * @retval k_ra8_err_*         Backend read failure.
 * @pre All pointers are non-NULL; @p cur follows a 0x85 entry.
 * @pre @p path is a flat (root-level) name.
 * @post On match the out-params describe the file.
 * @post On non-match the out-params are untouched.
 * @note Leftover name entries self-heal in ::priv_exfat_find.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_match_set(const ra8_fs_mount_t* m,
                                      exfat_cursor_t*       cur,
                                      const char*           path,
                                      uint32_t*             out_first,
                                      uint32_t*             out_size,
                                      uint8_t*              out_nofat)
{
  uint8_t   strm[k_exfat_entry_bytes] = {};
  ra8_err_t e                         = priv_exfat_next_entry(m, cur, strm);
  if (e != k_ra8_ok) {
    return e;
  }
  if (strm[0] != (uint8_t)k_exfat_entry_stream) {
    return k_ra8_err_not_found;
  }
  const uint32_t nlen = (uint32_t)strm[k_exfat_strm_off_nlen];
  if (nlen != priv_strlen(path)) {
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
    if (priv_exfat_name_chunk_eq(nm, path, pos, nlen) == 0U) {
      return k_ra8_err_not_found;
    }
  }
  *out_first = priv_rd32(&strm[k_exfat_strm_off_clus]);
  *out_size  = priv_rd32(&strm[k_exfat_strm_off_dlen]);
  *out_nofat = ((strm[k_exfat_strm_off_flags] & (uint8_t)k_exfat_secflag_no_fat) != 0U) ? 1U : 0U;
  return k_ra8_ok;
}

/* `priv_exfat_find()`: see header for the documented contract. */
ra8_err_t priv_exfat_find(const ra8_fs_mount_t* m,
                          const char*           path,
                          uint32_t*             out_first,
                          uint32_t*             out_size,
                          uint8_t*              out_nofat)
{
  /* Leading slashes are not part of the name; match FAT's priv_path_to_83
   * behavior so ra8_fs_open("/name") resolves on exFAT too (#93). */
  while (*path == '/') {
    path++;
  }
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
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
    e = priv_exfat_match_set(m, &cur, path, out_first, out_size, out_nofat);
    if (e == k_ra8_ok) {
      return k_ra8_ok;
    }
    if (e != k_ra8_err_not_found) {
      return e;
    }
  }
  return k_ra8_err_not_found; /* GCOVR_EXCL_LINE -- 65536 entries required */
}

/* `priv_exfat_open()`: see header for the documented contract. */
ra8_err_t priv_exfat_open(ra8_fs_mount_t* handle,
                          const char*     path,
                          ra8_fs_mode_t   mode,
                          ra8_fs_file_t** out_file)
{
  if (mode != k_ra8_fs_mode_read) {
    return k_ra8_err_not_supported;
  }
  uint32_t  first = 0U;
  uint32_t  size  = 0U;
  uint8_t   nofat = 0U;
  ra8_err_t e     = priv_exfat_find(handle, path, &first, &size, &nofat);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_fs_file_t* f = priv_alloc_file_slot();
  if (f == nullptr) {
    return k_ra8_err_no_mem;
  }
  f->mount              = handle;
  f->first_cluster      = first;
  f->cur_cluster        = first;
  f->walk_cache_idx     = 0U; /* read accelerator seeded at the chain head */
  f->walk_cache_cluster = first;
  f->size_bytes         = size;
  f->offset             = 0U;
  f->dir_entry_lba      = 0U;
  f->dir_entry_idx      = 0U;
  f->mode               = mode;
  f->no_fat_chain       = nofat;
  f->in_use             = 1U;
  *out_file             = f;
  return k_ra8_ok;
}
