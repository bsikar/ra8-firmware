/**
 * @file ra8_fs_fat_exfat_write.c
 * @brief exFAT allocation-bitmap and whole-file write path.
 *
 * @details
 * Allocation-bitmap scan/mark, cluster data writes, directory-set
 * construction, and the one-shot provisioning create path.
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

/* ---- exFAT one-shot file writer (provisioning) -------------------------- */

/**
 * @brief One step of exFAT's rotate-right-then-add 16-bit checksum.
 *
 * @details Used for both the directory SetChecksum and the name hash.
 * @param[in] cs Running checksum.
 * @param[in] b  Next byte.
 * @return Updated checksum.
 * @retval 0..0xFFFF The folded value.
 * @pre None.
 * @pre @p cs is the prior running value.
 * @post No state modified.
 * @post Result depends only on inputs.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t priv_exfat_csum_add(uint16_t cs, uint8_t b)
{
  uint16_t hi = ((cs & 1U) != 0U) ? (uint16_t)k_exfat_csum_hi_bit : (uint16_t)0U;
  return (uint16_t)(hi + (uint16_t)(cs >> 1) + (uint16_t)b);
}

/* `priv_exfat_name_hash()`: see header for the documented contract. */
uint16_t priv_exfat_name_hash(const char* path, uint32_t nlen)
{
  uint16_t h = 0U;
  for (uint32_t i = 0U; i < nlen; i++) {
    h = priv_exfat_csum_add(h, (uint8_t)priv_ascii_upper(path[i]));
    h = priv_exfat_csum_add(h, 0U);
  }
  return h;
}

/* `priv_exfat_set_checksum()`: see header for the documented contract. */
uint16_t priv_exfat_set_checksum(const uint8_t* set, uint32_t bytes)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < bytes; i++) {
    if (i == (uint32_t)k_exfat_off_file_csum) {
      continue;
    }
    if (i == ((uint32_t)k_exfat_off_file_csum + 1U)) {
      continue;
    }
    cs = priv_exfat_csum_add(cs, set[i]);
  }
  return cs;
}

/* `priv_exfat_find_bitmap()`: see header for the documented contract. */
ra8_err_t priv_exfat_find_bitmap(const ra8_fs_mount_t* m, uint32_t* out_clus, uint32_t* out_len)
{
  exfat_cursor_t cur = {.cluster = m->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t   e[k_exfat_entry_bytes] = {};
    ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r != k_ra8_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      return k_ra8_err_not_found;
    }
    if (e[0] == (uint8_t)k_exfat_entry_bitmap) {
      *out_clus = priv_rd32(&e[k_exfat_strm_off_clus]);
      *out_len  = priv_rd32(&e[k_exfat_strm_off_dlen]);
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found; /* GCOVR_EXCL_LINE -- k_exfat_scan_limit (65536) entries required */
}

/**
 * @brief Scan the allocation bitmap for a contiguous run of free clusters.
 *
 * @details Assumes a contiguous bitmap (true for a freshly formatted volume).
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  bmp_lba  First LBA (volume-relative) of the bitmap.
 * @param[in]  need     Number of contiguous free clusters required.
 * @param[out] out_clus First cluster of the found run.
 * @return Error code.
 * @retval k_ra8_ok         A run of @p need free clusters was found.
 * @retval k_ra8_err_no_mem No such run (volume full / too fragmented).
 * @retval k_ra8_err_*      Backend read failure.
 * @pre @p m and @p out_clus are non-NULL; @p need >= 1.
 * @pre The bitmap region is contiguous on disk.
 * @post On success ``*out_clus`` is the run's first cluster number.
 * @post No volume state modified.
 * @note O(count_of_clusters); bounded by the volume size.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_bitmap_scan(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t need, uint32_t* out_clus)
{
  uint32_t run                            = 0U;
  uint32_t start                          = 0U;
  uint32_t loaded                         = UINT32_MAX;
  uint8_t  sec[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t idx = 0U; idx < m->count_of_clusters; idx++) {
    const uint32_t lba = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra8_fs_bytes_per_sector);
    if (lba != loaded) {
      ra8_err_t e = priv_read_sector(m, lba, sec);
      if (e != k_ra8_ok) {
        return e;
      }
      loaded = lba;
    }
    const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra8_fs_bytes_per_sector;
    const uint32_t bit  = idx & k_exfat_bit_mask;
    if ((((uint32_t)sec[byte] >> bit) & 1U) != 0U) {
      run = 0U;
      continue;
    }
    if (run == 0U) {
      start = idx;
    }
    run++;
    if (run >= need) {
      *out_clus = start + k_cluster_first_data;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_mem;
}

/* `priv_exfat_bmp_switch()`: see header for the documented contract. */
ra8_err_t
priv_exfat_bmp_switch(const ra8_fs_mount_t* m, uint32_t lba, uint32_t* loaded, uint8_t* sec)
{
  if (lba == *loaded) {
    return k_ra8_ok;
  }
  if (*loaded != UINT32_MAX) {
    ra8_err_t we = priv_write_sector(m, *loaded, sec);
    if (we != k_ra8_ok) {
      return we;
    }
  }
  ra8_err_t e = priv_read_sector(m, lba, sec);
  if (e != k_ra8_ok) {
    return e;
  }
  *loaded = lba;
  return k_ra8_ok;
}

/**
 * @brief Mark a contiguous cluster run as allocated in the bitmap.
 *
 * @details Sets one bit per cluster, batching read-modify-write per bitmap sector.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark used.
 * @return Error code.
 * @retval k_ra8_ok    All bits set + written.
 * @retval k_ra8_err_* Backend read/write failure.
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre The bitmap region is contiguous on disk.
 * @post The @p count bits for the run read as 1.
 * @post Only the affected bitmap sectors are rewritten.
 * @note Read-modify-write, one sector at a time.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_bitmap_mark(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count)
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
      return e;
    }
    sec[byte] = (uint8_t)(sec[byte] | (uint8_t)(1U << bit));
  }
  return priv_write_sector(m, loaded, sec);
}

/**
 * @brief Write file data into a contiguous cluster run (zero-padded).
 *
 * @details Copies the bytes sector by sector, zero-filling the final partial sector.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] clus First cluster of the run.
 * @param[in] data File bytes.
 * @param[in] len  Byte count (> 0).
 * @return Error code.
 * @retval k_ra8_ok    All sectors written.
 * @retval k_ra8_err_* Backend write failure.
 * @pre @p m and @p data are non-NULL; @p len > 0.
 * @pre The run has enough clusters for @p len.
 * @post The run holds @p data, last sector zero-padded.
 * @post No directory/bitmap state modified here.
 * @note Writes whole sectors.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_write_data(const ra8_fs_mount_t* m, uint32_t clus, const uint8_t* data, uint32_t len)
{
  const uint32_t base    = priv_cluster_to_lba(m, clus);
  const uint32_t sectors = (len + k_ra8_fs_bytes_per_sector - 1U) / k_ra8_fs_bytes_per_sector;
  for (uint32_t s = 0U; s < sectors; s++) {
    uint8_t        sec[k_ra8_fs_bytes_per_sector] = {};
    const uint32_t off                            = s * k_ra8_fs_bytes_per_sector;
    uint32_t       n                              = (off < len) ? (len - off) : 0U;
    if (n > k_ra8_fs_bytes_per_sector) {
      n = k_ra8_fs_bytes_per_sector;
    }
    if (n > 0U) {
      priv_byte_copy(sec, &data[off], n);
    }
    ra8_err_t e = priv_write_sector(m, base + s, sec);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read one 32-byte directory entry by index within a cluster.
 *
 * @details Reads the containing sector and copies out the addressed entry.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[in]  cluster Directory cluster.
 * @param[in]  idx     Entry index within the cluster.
 * @param[out] out     Receives the entry.
 * @return Error code.
 * @retval k_ra8_ok    Entry read.
 * @retval k_ra8_err_* Backend read failure.
 * @pre All pointers are non-NULL.
 * @pre @p idx is within the cluster's entry capacity.
 * @post ``out`` holds the entry on success.
 * @post No state modified.
 * @note Reads the containing sector.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_read_entry(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t idx, uint8_t* out)
{
  const uint32_t byte_off = idx * (uint32_t)k_exfat_entry_bytes;
  const uint32_t lba = priv_cluster_to_lba(m, cluster) + (byte_off / k_ra8_fs_bytes_per_sector);
  uint8_t        sec[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t      e                              = priv_read_sector(m, lba, sec);
  if (e != k_ra8_ok) {
    return e;
  }
  priv_byte_copy(out, &sec[byte_off % k_ra8_fs_bytes_per_sector], (uint32_t)k_exfat_entry_bytes);
  return k_ra8_ok;
}

/**
 * @brief True if a directory entry slot is free (end-of-dir or deleted).
 *
 * @details A slot is reusable when it is the end-of-directory marker or has bit 7 clear.
 *
 * @param[in] type_byte Entry type byte (entry[0]).
 * @return 1 if the slot is available for reuse, else 0.
 * @retval 1 Slot is free.
 * @retval 0 Slot is in use.
 * @pre None.
 * @pre @p type_byte is entry[0].
 * @post No state modified.
 * @post Result depends only on @p type_byte.
 * @note exFAT marks an in-use entry with bit 7 set.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_exfat_slot_free(uint8_t type_byte)
{
  if (type_byte == (uint8_t)k_exfat_entry_eod) {
    return 1U;
  }
  if ((type_byte & (uint8_t)k_exfat_inuse_bit) == 0U) {
    return 1U;
  }
  return 0U;
}

/**
 * @brief Find @p need consecutive free entries within one root-dir cluster.
 *
 * @details Scans each directory cluster for a run of free slots large enough for the set.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  need     Number of consecutive free entries required.
 * @param[out] out_clus Cluster holding the run.
 * @param[out] out_idx  Entry index of the run start within that cluster.
 * @return Error code.
 * @retval k_ra8_ok         A run was found.
 * @retval k_ra8_err_no_mem No run of @p need entries in the root directory.
 * @retval k_ra8_err_*      Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre @p need >= 1.
 * @post On success the run location is returned.
 * @post No volume state modified.
 * @note The set is kept within a single cluster (no chain spanning).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_find_dir_space(const ra8_fs_mount_t* m,
                                           uint32_t              need,
                                           uint32_t*             out_clus,
                                           uint32_t*             out_idx)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra8_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  uint32_t cluster = m->root_cluster;
  uint32_t guard   = 0U;
  while (guard < (uint32_t)k_exfat_scan_limit) {
    uint32_t run = 0U;
    for (uint32_t i = 0U; i < per_cluster; i++) {
      uint8_t   e[k_exfat_entry_bytes] = {};
      ra8_err_t r                      = priv_exfat_read_entry(m, cluster, i, e);
      if (r != k_ra8_ok) {
        return r;
      }
      if (priv_exfat_slot_free(e[0]) == 0U) {
        run = 0U;
        continue;
      }
      if (run == 0U) {
        *out_idx = i;
      }
      run++;
      if (run >= need) {
        *out_clus = cluster;
        return k_ra8_ok;
      }
    }
    uint32_t  next = 0U;
    ra8_err_t fe   = priv_fat_get(m, cluster, &next);
    if (fe != k_ra8_ok) {
      return fe;
    }
    if (priv_is_eoc(m, next) != 0U) {
      return k_ra8_err_no_mem;
    }
    cluster = next;
    guard++;
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Build the File + Stream + Name entry set into @p set.
 *
 * @details Fills the typed entries, the name hash, and the trailing SetChecksum.
 *
 * @param[out] set        Buffer (>= set_len * 32 bytes).
 * @param[in]  path       File name (ASCII).
 * @param[in]  nlen       Name length in characters.
 * @param[in]  first_clus First data cluster of the file.
 * @param[in]  len        File length in bytes.
 * @return The total byte length of the built set.
 * @retval >0 Number of bytes written into @p set.
 * @pre @p set and @p path are non-NULL; @p set is large enough.
 * @pre ``priv_strlen(path) == nlen``.
 * @post @p set holds a complete entry set with a valid SetChecksum.
 * @post No volume state modified.
 * @note NoFatChain (contiguous) is recorded in the stream flags.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_exfat_build_set(uint8_t*    set,
                                     const char* path,
                                     uint32_t    nlen,
                                     uint32_t    first_clus,
                                     uint32_t    len)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t sec_count = 1U + name_entries;
  const uint32_t total     = (1U + sec_count) * (uint32_t)k_exfat_entry_bytes;
  for (uint32_t i = 0U; i < total; i++) {
    set[i] = 0U;
  }
  set[0]                      = (uint8_t)k_exfat_entry_file;
  set[k_exfat_off_file_secnt] = (uint8_t)sec_count;
  priv_wr16(&set[k_exfat_off_file_attr], (uint16_t)k_exfat_attr_archive);
  uint8_t* strm                = &set[k_exfat_entry_bytes];
  strm[0]                      = (uint8_t)k_exfat_entry_stream;
  strm[k_exfat_strm_off_flags] = (uint8_t)k_exfat_secflag_alloc;
  strm[k_exfat_strm_off_nlen]  = (uint8_t)nlen;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(path, nlen));
  priv_wr32(&strm[k_exfat_off_strm_valid], len);
  priv_wr32(&strm[k_exfat_strm_off_clus], first_clus);
  priv_wr32(&strm[k_exfat_strm_off_dlen], len);
  for (uint32_t n = 0U; n < name_entries; n++) {
    uint8_t* ne = &set[(size_t)(2U + n) * (size_t)k_exfat_entry_bytes];
    ne[0]       = (uint8_t)k_exfat_entry_name;
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t pos = (n * (uint32_t)k_exfat_name_per_entry) + c;
      if (pos < nlen) {
        ne[k_exfat_name_off + (c * 2U)] = (uint8_t)path[pos];
      }
    }
  }
  priv_wr16(&set[k_exfat_off_file_csum], priv_exfat_set_checksum(set, total));
  return total;
}

/* `priv_exfat_write_dir_set()`: see header for the documented contract. */
ra8_err_t priv_exfat_write_dir_set(const ra8_fs_mount_t* m,
                                   uint32_t              cluster,
                                   uint32_t              idx,
                                   const uint8_t*        set,
                                   uint32_t              bytes)
{
  const uint32_t count = bytes / (uint32_t)k_exfat_entry_bytes;
  for (uint32_t k = 0U; k < count; k++) {
    const uint32_t byte_off = (idx + k) * (uint32_t)k_exfat_entry_bytes;
    const uint32_t lba = priv_cluster_to_lba(m, cluster) + (byte_off / k_ra8_fs_bytes_per_sector);
    uint8_t        sec[k_ra8_fs_bytes_per_sector] = {};
    ra8_err_t      e                              = priv_read_sector(m, lba, sec);
    if (e != k_ra8_ok) {
      return e;
    }
    priv_byte_copy(&sec[byte_off % k_ra8_fs_bytes_per_sector],
                   &set[(size_t)k * (size_t)k_exfat_entry_bytes],
                   (uint32_t)k_exfat_entry_bytes);
    e = priv_write_sector(m, lba, sec);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Allocate a contiguous run, write the data, and mark the bitmap.
 *
 * @details Combines bitmap scan, data write, and bitmap marking into one step.
 *
 * @param[in]  m     Mounted exFAT volume.
 * @param[in]  data  File bytes.
 * @param[in]  len   Byte count (> 0).
 * @param[in]  nclus Clusters required.
 * @param[out] out_start First cluster of the allocated run.
 * @return Error code.
 * @retval k_ra8_ok         Data written; run marked used.
 * @retval k_ra8_err_no_mem No contiguous run of @p nclus clusters.
 * @retval k_ra8_err_*      Bitmap or backend failure.
 * @pre All pointers are non-NULL; @p nclus >= 1.
 * @pre ``m->type`` is exFAT.
 * @post On success the run holds the data and is marked allocated.
 * @post On failure an unlinked run may remain allocated.
 * @note Contiguous (NoFatChain) allocation only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_alloc_write(ra8_fs_mount_t* m,
                                        const uint8_t*  data,
                                        uint32_t        len,
                                        uint32_t        nclus,
                                        uint32_t*       out_start)
{
  uint32_t  bclus = 0U;
  uint32_t  blen  = 0U;
  ra8_err_t e     = priv_exfat_find_bitmap(m, &bclus, &blen);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t bmp_lba = priv_cluster_to_lba(m, bclus);
  e                      = priv_exfat_bitmap_scan(m, bmp_lba, nclus, out_start);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_exfat_write_data(m, *out_start, data, len);
  if (e != k_ra8_ok) {
    return e;
  }
  // NOLINTNEXTLINE(readability-suspicious-call-argument) -- (cluster, count) order is correct
  return priv_exfat_bitmap_mark(m, bmp_lba, *out_start, nclus);
}

/**
 * @brief Append a File/Stream/Name entry set linking a written run.
 *
 * @details Finds directory space, builds the entry set, and writes it.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] path  File name (ASCII).
 * @param[in] nlen  Name length in characters.
 * @param[in] start First data cluster of the file.
 * @param[in] len   File length in bytes.
 * @return Error code.
 * @retval k_ra8_ok         Entry set written.
 * @retval k_ra8_err_no_mem No directory space.
 * @retval k_ra8_err_*      Backend failure.
 * @pre @p m and @p path are non-NULL; ``m->type`` is exFAT.
 * @pre ``priv_strlen(path) == nlen``.
 * @post On success the root directory references the file.
 * @post No data clusters are modified here.
 * @note Keeps the entry set within one directory cluster.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_link(ra8_fs_mount_t* m, const char* path, uint32_t nlen, uint32_t start, uint32_t len)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t need  = 2U + name_entries;
  uint32_t       dclus = 0U;
  uint32_t       didx  = 0U;
  ra8_err_t      e     = priv_exfat_find_dir_space(m, need, &dclus, &didx);
  if (e != k_ra8_ok) {
    return e;
  }
  uint8_t        set[k_exfat_max_set_bytes] = {};
  const uint32_t bytes                      = priv_exfat_build_set(set, path, nlen, start, len);
  return priv_exfat_write_dir_set(m, dclus, didx, set, bytes);
}

/* `priv_exfat_create()`: see header for the documented contract. */
ra8_err_t priv_exfat_create(ra8_fs_mount_t* m, const char* path, const uint8_t* data, uint32_t len)
{
  /* Strip leading slashes so the stored name matches what the matchers
   * search for (#93); otherwise the file is created but cannot be reopened. */
  while (*path == '/') {
    path++;
  }
  const uint32_t nlen = priv_strlen(path);
  if (nlen == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (nlen > (uint32_t)k_exfat_name_cap) {
    return k_ra8_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* Replace, do not duplicate (#603). Nothing in the create path used to look
   * for the name, so a second create wrote a second File/Stream/Name set for
   * it: the directory then held two entries for one name, and the first file's
   * clusters stayed marked used in the allocation bitmap with nothing pointing
   * at them -- unreclaimable without a reformat. Unlinking first frees those
   * clusters AND the old set's slots (which the scan below then reuses), which
   * is the truncate-then-write the FAT side gets from ra8_fs_open(write).
   * priv_exfat_unlink() also carries the directory guard, so write_file() over
   * a directory name reports k_ra8_err_invalid_arg instead of eating it. */
  const ra8_err_t ue = priv_exfat_unlink(m, path);
  if ((ue != k_ra8_ok) && (ue != k_ra8_err_not_found)) {
    return ue;
  }
  const uint32_t cbytes = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  const uint32_t nclus  = (len + cbytes - 1U) / cbytes;
  uint32_t       start  = 0U;
  ra8_err_t      e      = priv_exfat_alloc_write(m, data, len, nclus, &start);
  if (e != k_ra8_ok) {
    return e;
  }
  return priv_exfat_link(m, path, nlen, start, len);
}
