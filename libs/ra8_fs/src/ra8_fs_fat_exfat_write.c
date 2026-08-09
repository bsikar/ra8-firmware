/**
 * @file ra8_fs_fat_exfat_write.c
 * @brief exFAT allocation-bitmap primitives and directory entry-set construction.
 *
 * @details
 * The two things every exFAT write needs before it can put a byte anywhere:
 * somewhere to put it (the allocation bitmap -- locate, probe, scan, mark) and
 * something to name it (the File + Stream + Name directory entry set -- build,
 * checksum, place). The streaming engine that drives them lives in
 * `ra8_fs_fat_exfat_stream.c`.
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
uint16_t priv_exfat_name_hash(const uint16_t* name, uint32_t nlen)
{
  uint16_t h = 0U;
  for (uint32_t i = 0U; i < nlen; i++) {
    /* The spec's definition exactly: fold the UP-CASED name, little-endian, one
     * byte at a time. Up-casing with an ASCII-only rule -- which is what this
     * did -- stored a hash no compliant reader recomputes for any name outside
     * ASCII, so a host could list the file and then not find it (#606). */
    const uint16_t u = priv_exfat_upcase_unit(name[i]);
    h                = priv_exfat_csum_add(h, (uint8_t)((uint32_t)u & (uint32_t)k_utf_byte_mask));
    h                = priv_exfat_csum_add(h, (uint8_t)((uint32_t)u >> (uint32_t)k_utf_byte_shift));
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
  /* The bitmap entry lives in the ROOT directory by definition, so this is the
   * one exFAT scan that is not parameterised by a directory (#605). */
  exfat_dir_t root = {};
  priv_exfat_dir_root(m, &root);
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(&root, &cur);
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
 * @brief Scan a window of the allocation bitmap for a contiguous free run.
 *
 * @details Walks cluster indices `[from, m->count_of_clusters)` looking for
 *          @p need consecutive clear bits, loading each bitmap sector once.
 *          Assumes a contiguous bitmap (true for a freshly formatted volume).
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  bmp_lba  First LBA (volume-relative) of the bitmap.
 * @param[in]  from     Cluster INDEX (0-based) to begin the walk at.
 * @param[in]  need     Number of contiguous free clusters required.
 * @param[out] out_clus First cluster of the found run.
 * @return Error code.
 * @retval k_ra8_ok         A run of @p need free clusters was found.
 * @retval k_ra8_err_no_mem No such run in this window.
 * @retval k_ra8_err_*      Backend read failure.
 * @pre @p m and @p out_clus are non-NULL; @p need >= 1.
 * @pre @p from is a cluster index, not a cluster number.
 * @post On success ``*out_clus`` is the run's first cluster NUMBER.
 * @post No volume state modified.
 * @note O(count_of_clusters - from); bounded by the volume size.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_bitmap_window(const ra8_fs_mount_t* m,
                                          uint32_t              bmp_lba,
                                          uint32_t              from,
                                          uint32_t              need,
                                          uint32_t*             out_clus)
{
  uint32_t run                            = 0U;
  uint32_t start                          = 0U;
  uint32_t loaded                         = UINT32_MAX;
  uint8_t  sec[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t idx = from; idx < m->count_of_clusters; idx++) {
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

/** @brief Implementation of `priv_exfat_bitmap_scan()` -- hinted pass, then a full rescan. */
ra8_err_t
priv_exfat_bitmap_scan(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t need, uint32_t* out_clus)
{
  const uint32_t hint = priv_alloc_hint_get(m);
  uint32_t       from = 0U;
  if (hint > (uint32_t)k_cluster_first_data) {
    from = hint - (uint32_t)k_cluster_first_data;
  }
  if (from >= m->count_of_clusters) {
    from = 0U;
  }
  const ra8_err_t e = priv_exfat_bitmap_window(m, bmp_lba, from, need, out_clus);
  if (e != k_ra8_err_no_mem) {
    return e;
  }
  if (from == 0U) {
    return e;
  }
  return priv_exfat_bitmap_window(m, bmp_lba, 0U, need, out_clus);
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

/** @brief Implementation of `priv_exfat_bitmap_mark()` -- one read-modify-write per sector. */
ra8_err_t
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

/** @brief Implementation of `priv_exfat_bitmap_test()` -- one sector read, one bit. */
ra8_err_t
priv_exfat_bitmap_test(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint8_t* out_free)
{
  const uint32_t idx  = clus - (uint32_t)k_cluster_first_data;
  const uint32_t lba  = bmp_lba + ((idx >> k_exfat_bit_shift) / k_ra8_fs_bytes_per_sector);
  const uint32_t byte = (idx >> k_exfat_bit_shift) % k_ra8_fs_bytes_per_sector;
  const uint32_t bit  = idx & k_exfat_bit_mask;

  uint8_t         sec[k_ra8_fs_bytes_per_sector] = {};
  const ra8_err_t e                              = priv_read_sector(m, lba, sec);
  if (e != k_ra8_ok) {
    return e;
  }
  *out_free = (uint8_t)(((((uint32_t)sec[byte] >> bit) & 1U) != 0U) ? 0U : 1U);
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
 * @brief Scan one directory cluster for @p need consecutive free entries.
 *
 * @details Inner half of ::priv_exfat_find_dir_space, split out so the outer
 *          walk stays inside the nesting and statement budgets.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[in]  cluster Directory cluster to scan.
 * @param[in]  need    Number of consecutive free entries required.
 * @param[out] out_idx Entry index of the run start within @p cluster.
 * @return Error code.
 * @retval k_ra8_ok         A run of @p need free slots starts at ``*out_idx``.
 * @retval k_ra8_err_no_mem No such run in this cluster.
 * @retval k_ra8_err_*      Backend read failure.
 * @pre @p m and @p out_idx are non-NULL; @p need >= 1.
 * @pre @p cluster belongs to the directory being searched.
 * @post On success ``*out_idx`` addresses the first free slot of the run.
 * @post No volume state modified.
 * @note A set is never split across two clusters.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_space_in_cluster(const ra8_fs_mount_t* m,
                                             uint32_t              cluster,
                                             uint32_t              need,
                                             uint32_t*             out_idx)
{
  const uint32_t per_cluster =
    (m->sectors_per_cluster * k_ra8_fs_bytes_per_sector) / (uint32_t)k_exfat_entry_bytes;
  uint32_t run = 0U;
  for (uint32_t i = 0U; i < per_cluster; i++) {
    uint8_t         e[k_exfat_entry_bytes] = {};
    const ra8_err_t r                      = priv_exfat_read_entry(m, cluster, i, e);
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
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Scan a directory's existing clusters for @p need consecutive free slots.
 *
 * @details The inner walk of ::priv_exfat_find_dir_space, split out so the grow
 *          loop that wraps it stays small. Walks @p dir cluster by cluster and
 *          reports ::k_ra8_err_no_mem when its CURRENT run has no room -- it
 *          never grows the directory itself; that is the caller's job.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  dir      Directory to search at its current size.
 * @param[in]  need     Number of consecutive free entries required.
 * @param[out] out_clus Receives the cluster holding the run.
 * @param[out] out_idx  Receives the run's first entry index in that cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok         A run was found at the current size.
 * @retval k_ra8_err_no_mem The directory's current run has no run of @p need.
 * @retval k_ra8_err_*      Backend read failure.
 *
 * @pre Every pointer argument is non-NULL; @p need >= 1.
 * @pre `m->type` is exFAT.
 * @post On success the run location is returned.
 * @post No volume state is modified.
 *
 * @note The walk is bounded by ::k_exfat_scan_limit clusters (P10 Rule 2).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_scan_dir_space(const ra8_fs_mount_t* m,
                                           const exfat_dir_t*    dir,
                                           uint32_t              need,
                                           uint32_t*             out_clus,
                                           uint32_t*             out_idx)
{
  uint32_t cluster = dir->cluster;
  for (uint32_t guard = 0U; guard < (uint32_t)k_exfat_scan_limit; guard++) {
    const ra8_err_t r = priv_exfat_space_in_cluster(m, cluster, need, out_idx);
    if (r == k_ra8_ok) {
      *out_clus = cluster;
      return k_ra8_ok;
    }
    if (r != k_ra8_err_no_mem) {
      return r;
    }
    uint32_t        next = 0U;
    const ra8_err_t fe   = priv_exfat_step_cluster(m, cluster, dir->contig_end, &next);
    if (fe == k_ra8_err_not_found) {
      return k_ra8_err_no_mem; /* the current run is exhausted -- the caller grows */
    }
    if (fe != k_ra8_ok) {
      return fe;
    }
    cluster = next;
  }
  return k_ra8_err_no_mem; /* GCOVR_EXCL_LINE -- 65536 directory clusters required */
}

/* `priv_exfat_find_dir_space()`: see header for the documented contract. */
ra8_err_t priv_exfat_find_dir_space(const ra8_fs_mount_t* m,
                                    const exfat_dir_t*    dir,
                                    uint32_t              need,
                                    uint32_t*             out_clus,
                                    uint32_t*             out_idx)
{
  /* A mutable copy: ::priv_exfat_grow_dir advances `work.contig_end` (and drops
   * it to 0 when the run converts to a FAT chain) so the rescan below reaches
   * the cluster the grow just appended. */
  exfat_dir_t work = *dir;
  for (uint32_t grow = 0U; grow <= (uint32_t)k_exfat_dir_grow_max; grow++) {
    const ra8_err_t e = priv_exfat_scan_dir_space(m, &work, need, out_clus, out_idx);
    if (e != k_ra8_err_no_mem) {
      return e; /* found a run, or a hard read error */
    }
    if (grow == (uint32_t)k_exfat_dir_grow_max) {
      break; /* one grow already yields more than any set needs; stop */
    }
    const ra8_err_t ge = priv_exfat_grow_dir(m, &work);
    if (ge != k_ra8_ok) {
      return ge; /* the volume, not the directory, is out of room */
    }
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Build a zero-length File + Stream + Name entry set into @p set.
 *
 * @details Fills the typed entries, the name hash, the creation stamps and the
 *          trailing SetChecksum for a file that owns NO clusters yet:
 *          `FirstCluster` 0, `DataLength` 0, `ValidDataLength` 0 and
 *          `GeneralSecondaryFlags` = AllocationPossible with `NoFatChain`
 *          CLEAR. That is what exFAT spec sec 7.4.4 requires of an empty file,
 *          and it is the honest starting point for a stream: the first
 *          ::priv_exfat_flush_set after a byte lands rewrites the three fields
 *          and sets `NoFatChain` if the run is still contiguous.
 *
 * @param[out] set  Buffer (>= `k_exfat_max_set_bytes`).
 * @param[in]  name File name as UTF-16 code units.
 * @param[in]  nlen Name length in UTF-16 UNITS, which is what `NameLength`
 *                  counts and what a Name entry holds fifteen of.
 *
 * @return The total byte length of the built set.
 * @retval >0 Number of bytes written into @p set.
 *
 * @pre @p set and @p name are non-NULL; @p set is large enough.
 * @pre @p nlen is the unit count ::priv_utf8_to_utf16() produced, and >= 1.
 * @post @p set holds a complete entry set with a valid SetChecksum.
 * @post No volume state modified.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_exfat_build_set(uint8_t* set, const uint16_t* name, uint32_t nlen)
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
  /* Before the SetChecksum below, which covers these bytes. exFAT's stamps are
   * zero-is-illegal for the same reason FAT's are -- month and day are 1-based
   * -- and it has three fields FAT does not: the 10 ms increments and the
   * UtcOffset bytes (#601). */
  priv_exfat_file_stamp_create(set);
  uint8_t* strm                = &set[k_exfat_entry_bytes];
  strm[0]                      = (uint8_t)k_exfat_entry_stream;
  strm[k_exfat_strm_off_flags] = (uint8_t)k_exfat_secflag_poss;
  strm[k_exfat_strm_off_nlen]  = (uint8_t)nlen;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(name, nlen));
  for (uint32_t n = 0U; n < name_entries; n++) {
    uint8_t* ne = &set[(size_t)(2U + n) * (size_t)k_exfat_entry_bytes];
    ne[0]       = (uint8_t)k_exfat_entry_name;
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t pos = (n * (uint32_t)k_exfat_name_per_entry) + c;
      if (pos < nlen) {
        /* A whole UTF-16 unit, not the low byte of one: writing one unit per
         * INPUT BYTE turned every multi-byte character into that many garbage
         * characters and made NameLength count bytes where the format counts
         * units (#606). */
        priv_wr16(&ne[k_exfat_name_off + (c * 2U)], name[pos]);
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

/** @brief Implementation of `priv_exfat_link()` -- one directory-slot scan, one set write. */
ra8_err_t priv_exfat_link(ra8_fs_mount_t*    m,
                          const exfat_dir_t* dir,
                          const uint16_t*    name,
                          uint32_t           nlen,
                          exfat_setpos_t*    out_head,
                          uint32_t*          out_count)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t need  = 2U + name_entries;
  uint32_t       dclus = 0U;
  uint32_t       didx  = 0U;
  ra8_err_t      e     = priv_exfat_find_dir_space(m, dir, need, &dclus, &didx);
  if (e != k_ra8_ok) {
    return e;
  }
  uint8_t        set[k_exfat_max_set_bytes] = {};
  const uint32_t bytes                      = priv_exfat_build_set(set, name, nlen);
  e                                         = priv_exfat_write_dir_set(m, dclus, didx, set, bytes);
  if (e != k_ra8_ok) {
    return e;
  }
  out_head->cluster = dclus;
  out_head->index   = didx;
  *out_count        = need;
  return k_ra8_ok;
}
