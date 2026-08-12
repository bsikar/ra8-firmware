/**
 * @file ra8_fs_fat.c
 * @brief FAT12/FAT16/FAT32 low-level primitives for the `ra8_fs` adapter.
 *
 * @details
 * Sector-by-sector primitives shared by the rest of the FAT/exFAT adapter:
 * little-endian field access, the backend sector read/write wrappers, and the
 * FAT12/16/32 entry get/set + cluster-allocation layer. The adapter is split
 * across several translation units (see `ra8_fs_fat_internal.h`); this file
 * holds the bottom layer every other unit builds on. No dynamic memory: one
 * static scratch sector serves every access.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000. Section numbers track that PDF.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: every loop is bounded by sector count, cluster count, or
 *     a small enum-defined max.
 *   - Rule 3: zero malloc; all state lives in static arrays.
 *   - Rule 5: every public entry checks pre/post-conditions.
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

/* =============================================================================
 * Little-endian helpers
 * =============================================================================
 */

/* `priv_rd16()`: see header for the documented contract. */
uint16_t priv_rd16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << k_shift_byte));
}

/* `priv_rd32()`: see header for the documented contract. */
uint32_t priv_rd32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_shift_byte) | ((uint32_t)p[2] << k_shift_two_bytes) |
         ((uint32_t)p[3] << k_shift_three_bytes);
}

/* `priv_rd64()`: see header for the documented contract. */
uint64_t priv_rd64(const uint8_t* p)
{
  return (uint64_t)priv_rd32(p) | ((uint64_t)priv_rd32(&p[sizeof(uint32_t)]) << k_shift_word32);
}

/* `priv_wr16()`: see header for the documented contract. */
void priv_wr16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & k_byte_mask);
  p[1] = (uint8_t)((v >> k_shift_byte) & k_byte_mask);
}

/* `priv_wr32()`: see header for the documented contract. */
void priv_wr32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & k_byte_mask);
  p[1] = (uint8_t)((v >> k_shift_byte) & k_byte_mask);
  p[2] = (uint8_t)((v >> k_shift_two_bytes) & k_byte_mask);
  p[3] = (uint8_t)((v >> k_shift_three_bytes) & k_byte_mask);
}

/* `priv_wr64()`: see header for the documented contract. */
void priv_wr64(uint8_t* p, uint64_t v)
{
  priv_wr32(p, (uint32_t)v);
  priv_wr32(&p[sizeof(uint32_t)], (uint32_t)(v >> k_shift_word32));
}

/* `priv_bps()`: see header for the documented contract. */
uint32_t priv_bps(const ra8_fs_mount_t* m)
{
  return m->bytes_per_sector;
}

/* `priv_cluster_bytes()`: see header for the documented contract. */
uint32_t priv_cluster_bytes(const ra8_fs_mount_t* m)
{
  return m->sectors_per_cluster * m->bytes_per_sector;
}

/* `priv_dir_eps()`: see header for the documented contract. */
uint32_t priv_dir_eps(const ra8_fs_mount_t* m)
{
  return m->bytes_per_sector / (uint32_t)k_ra8_fs_dir_entry_bytes;
}

/* `priv_byte_fill()`: see header for the documented contract. */
void priv_byte_fill(uint8_t* dst, uint8_t value, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    dst[i] = value;
  }
}

/* `priv_byte_copy()`: see header for the documented contract. */
void priv_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/* `priv_byte_equal()`: see header for the documented contract. */
uint8_t priv_byte_equal(const uint8_t* a, const uint8_t* b, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    if (a[i] != b[i]) {
      return 0U;
    }
  }
  return 1U;
}

/* =============================================================================
 * Backend wrappers
 * =============================================================================
 */

/* `priv_read_sector()`: see header for the documented contract. */
ra8_err_t priv_read_sector(const ra8_fs_mount_t* m, uint64_t lba, uint8_t* buf)
{
  return m->backend.read_block(m->backend.ctx, lba + m->partition_base_lba, 1, buf);
}

/* `priv_write_sector()`: see header for the documented contract. */
ra8_err_t priv_write_sector(const ra8_fs_mount_t* m, uint64_t lba, const uint8_t* buf)
{
  return m->backend.write_block(m->backend.ctx, lba + m->partition_base_lba, 1, buf);
}

/* =============================================================================
 * FAT entry get/set -- dispatches across FAT12/16/32 layouts
 * =============================================================================
 */

/**
 * @brief Compute the byte offset of `cluster`'s FAT entry for this FAT type.
 *
 * @details FAT12 entries are 1.5 bytes, FAT16 are 2 bytes, FAT32 are 4
 *          bytes. Result is the byte offset within the FAT region.
 *
 * @param[in] m       Mount providing the FAT type.
 * @param[in] cluster Cluster number to look up.
 *
 * @return Byte offset within the FAT region.
 * @retval 0..UINT32_MAX  Byte offset.
 *
 * @pre `m` is non-NULL.
 * @pre `cluster` is within the addressable cluster range.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t priv_fat_entry_byte_offset(const ra8_fs_mount_t* m, uint32_t cluster)
{
  if (m->type == k_ra8_fs_type_fat12) {
    return (uint64_t)cluster + ((uint64_t)cluster / 2U);
  }
  if (m->type == k_ra8_fs_type_fat16) {
    return (uint64_t)cluster * 2U;
  }
  /* 8 bytes on neither format: FAT32 and exFAT entries are 4 bytes, and on an
   * exFAT volume near the 2^32-cluster ceiling the product exceeds 32 bits --
   * which is exactly why this returns 64 bits. */
  return (uint64_t)cluster * 4U;
}

/* `priv_fat_get()`: see header for the documented contract. */
ra8_err_t priv_fat_get(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t* out_value)
{
  const uint64_t fat_offset = priv_fat_entry_byte_offset(m, cluster);
  const uint64_t sec_num    = m->first_fat_lba + (fat_offset / priv_bps(m));
  const uint32_t sec_off    = (uint32_t)(fat_offset % priv_bps(m));

  uint8_t* const buf = priv_sec_fat();
  ra8_err_t      err = priv_fat_sector_read(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t v = 0;
  if (m->type == k_ra8_fs_type_fat12) {
    /* MS FAT spec sec 4.1: read 16 bits straddling the byte and shift. */
    uint8_t b0 = buf[sec_off];
    uint8_t b1 = 0;
    if (sec_off + 1U < priv_bps(m)) {
      b1 = buf[sec_off + 1U];
    } else {
      uint8_t* const buf2 = priv_sec_fat2();
      err                 = priv_fat_sector_read(m, sec_num + 1U, buf2);
      if (err != k_ra8_ok) {
        return err;
      }
      b1 = buf2[0];
    }
    uint16_t raw = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << k_shift_byte));
    if ((cluster & 1U) != 0U) {
      v = (uint32_t)(raw >> k_shift_nibble);
    } else {
      v = (uint32_t)(raw & k_fat12_value_mask);
    }
  } else if (m->type == k_ra8_fs_type_fat16) {
    v = priv_rd16(&buf[sec_off]);
  } else if (m->type == k_ra8_fs_type_exfat) {
    /* exFAT spec sec 4.1: a FAT entry is a full 32-bit value. FAT32's top four
     * bits are reserved and masked off above; masking here would fold the
     * end-of-chain marker 0xFFFFFFFF down to 0x0FFFFFFF and, on a volume with
     * more than 2^28 clusters, truncate an ordinary cluster number. */
    v = priv_rd32(&buf[sec_off]);
  } else {
    v = priv_rd32(&buf[sec_off]) & k_cluster_mask_fat32;
  }

  *out_value = v;
  return k_ra8_ok;
}

/**
 * @brief Commit one (or, when straddling, two) FAT12 sectors and refresh the cache.
 *
 * @details Split out of ::priv_fat12_set_one so that function stays inside the
 *          statement budget: the write-back half is self-contained, and both
 *          sectors go through ::priv_fat_sector_wrote so a later read of either
 *          is served from memory rather than the device.
 *
 * @param[in] m        Mount providing backend access.
 * @param[in] buf      The entry's first sector, already patched.
 * @param[in] buf2     The following sector, meaningful only when straddling.
 * @param[in] sec_num  Sector number of @p buf.
 * @param[in] straddle Non-zero when the entry spans into @p buf2.
 *
 * @return Error code.
 * @retval k_ra8_ok    Every affected sector is on disk.
 * @retval k_ra8_err_* Backend write failure.
 *
 * @pre `m`, `buf` and `buf2` are non-NULL.
 * @pre Both buffers hold a whole sector.
 * @post On success the on-disk FAT12 entry reflects the new value.
 * @post On success the sector cache matches what was written.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat12_store(const ra8_fs_mount_t* m,
                                  const uint8_t*        buf,
                                  const uint8_t*        buf2,
                                  uint64_t              sec_num,
                                  uint8_t               straddle)
{
  ra8_err_t err = priv_write_sector(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_sector_wrote(m, buf, sec_num);
  if (straddle == 0U) {
    return k_ra8_ok;
  }
  err = priv_write_sector(m, sec_num + 1U, buf2);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_sector_wrote(m, buf2, sec_num + 1U);
  return k_ra8_ok;
}

/**
 * @brief Write a FAT12 entry, handling sector-straddling 12-bit packing.
 *
 * @details FAT12 entries are 12 bits and may straddle a sector boundary.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry's first byte.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] cluster Cluster number (used to pick low/high nibble).
 * @param[in] value   12-bit value to write (low 12 bits used).
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry updated.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off < m->bytes_per_sector`.
 * @post On success, the FAT12 entry on disk reflects the new value.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fat12_set_one(const ra8_fs_mount_t* m,
                                    uint64_t              sec_num,
                                    uint32_t              sec_off,
                                    uint32_t              cluster,
                                    uint32_t              value)
{
  uint8_t* const buf      = priv_sec_fat();
  uint8_t* const buf2     = priv_sec_fat2();
  uint8_t        straddle = 0U;
  ra8_err_t      err      = priv_fat_sector_read(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t b0 = buf[sec_off];
  uint8_t b1 = 0;
  if (sec_off + 1U < priv_bps(m)) {
    b1 = buf[sec_off + 1U];
  } else {
    err = priv_fat_sector_read(m, sec_num + 1U, buf2);
    if (err != k_ra8_ok) {
      return err;
    }
    b1       = buf2[0];
    straddle = 1U;
  }
  uint16_t raw = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << k_shift_byte));
  if ((cluster & 1U) != 0U) {
    raw = (uint16_t)((raw & k_fat12_low_nibble_mask) |
                     (uint16_t)((value & k_fat12_value_mask) << k_shift_nibble));
  } else {
    raw = (uint16_t)((raw & k_fat12_high_nibble_mask) | (uint16_t)(value & k_fat12_value_mask));
  }
  buf[sec_off] = (uint8_t)(raw & k_byte_mask);
  if (straddle == 0U) {
    buf[sec_off + 1U] = (uint8_t)((raw >> k_shift_byte) & k_byte_mask);
  } else {
    buf2[0] = (uint8_t)((raw >> k_shift_byte) & k_byte_mask);
  }
  return priv_fat12_store(m, buf, buf2, sec_num, straddle);
}

/**
 * @brief Write a FAT16 entry into one sector.
 *
 * @details FAT16 entries never straddle sectors. One read/write cycle
 *          updates the entry.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] value   Value to write (low 16 bits used).
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry updated.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off <= m->bytes_per_sector - 2`.
 * @post On success, the FAT16 entry on disk reflects the new value.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_fat16_set_one(const ra8_fs_mount_t* m, uint64_t sec_num, uint32_t sec_off, uint32_t value)
{
  uint8_t* const buf = priv_sec_fat();
  ra8_err_t      err = priv_fat_sector_read(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_wr16(&buf[sec_off], (uint16_t)(value & k_word_mask));
  err = priv_write_sector(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_sector_wrote(m, buf, sec_num);
  return k_ra8_ok;
}

/**
 * @brief Write a FAT32 entry into one sector (preserves top 4 reserved bits).
 *
 * @details FAT32 entries are 32 bits but only the low 28 bits are
 *          cluster data; the high 4 reserved bits must be preserved.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] value   Value to write (low 28 bits used).
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry updated.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off <= m->bytes_per_sector - 4`.
 * @post Low 28 bits of the entry equal `value`; high 4 bits preserved.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_fat32_set_one(const ra8_fs_mount_t* m, uint64_t sec_num, uint32_t sec_off, uint32_t value)
{
  uint8_t* const buf = priv_sec_fat();
  ra8_err_t      err = priv_fat_sector_read(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t prev = priv_rd32(&buf[sec_off]) & ~k_cluster_mask_fat32;
  priv_wr32(&buf[sec_off], (value & k_cluster_mask_fat32) | prev);
  err = priv_write_sector(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_sector_wrote(m, buf, sec_num);
  return k_ra8_ok;
}

/**
 * @brief Write an exFAT FAT entry -- all 32 bits, nothing reserved.
 *
 * @details The exFAT counterpart of ::priv_fat32_set_one. exFAT has no
 *          reserved high nibble (spec sec 4.1), so there is nothing to
 *          preserve and the value goes down whole. Routing exFAT through the
 *          FAT32 setter would mask the end-of-chain marker to 0x0FFFFFFF,
 *          which a host `fsck` reads as a chain pointing at a cluster the
 *          volume does not have.
 *
 * @param[in] m       Mount providing backend access.
 * @param[in] sec_num Sector number containing the entry.
 * @param[in] sec_off Byte offset within that sector.
 * @param[in] value   The 32-bit entry value to store.
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry updated.
 * @retval k_ra8_err_* Backend read or write failure.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `sec_off <= m->bytes_per_sector - 4`.
 * @post The entry on disk equals @p value exactly.
 * @post On failure, on-disk state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_fat_set_one(const ra8_fs_mount_t* m, uint64_t sec_num, uint32_t sec_off, uint32_t value)
{
  uint8_t* const buf = priv_sec_fat();
  ra8_err_t      err = priv_fat_sector_read(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_wr32(&buf[sec_off], value);
  err = priv_write_sector(m, sec_num, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_fat_sector_wrote(m, buf, sec_num);
  return k_ra8_ok;
}

/* `priv_fat_set()`: see header for the documented contract. */
ra8_err_t priv_fat_set(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t value)
{
  const uint64_t fat_offset = priv_fat_entry_byte_offset(m, cluster);
  for (uint32_t i = 0; i < m->num_fats; i++) {
    const uint64_t fat_base = m->first_fat_lba + ((uint64_t)i * m->fat_size_sectors);
    const uint64_t sec_num  = fat_base + (fat_offset / priv_bps(m));
    const uint32_t sec_off  = (uint32_t)(fat_offset % priv_bps(m));
    ra8_err_t      err      = k_ra8_ok;
    if (m->type == k_ra8_fs_type_fat12) {
      err = priv_fat12_set_one(m, sec_num, sec_off, cluster, value);
    } else if (m->type == k_ra8_fs_type_fat16) {
      err = priv_fat16_set_one(m, sec_num, sec_off, value);
    } else if (m->type == k_ra8_fs_type_exfat) {
      err = priv_exfat_fat_set_one(m, sec_num, sec_off, value);
    } else {
      err = priv_fat32_set_one(m, sec_num, sec_off, value);
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* `priv_is_eoc()`: see header for the documented contract. */
uint8_t priv_is_eoc(const ra8_fs_mount_t* m, uint32_t value)
{
  if (m->type == k_ra8_fs_type_fat12) {
    return (uint8_t)((value >= k_cluster_eoc_min_fat12) ? 1U : 0U);
  }
  if (m->type == k_ra8_fs_type_fat16) {
    return (uint8_t)((value >= k_cluster_eoc_min_fat16) ? 1U : 0U);
  }
  if (m->type == k_ra8_fs_type_exfat) {
    return (uint8_t)((value >= k_cluster_eoc_min_exfat) ? 1U : 0U);
  }
  return (uint8_t)((value >= k_cluster_eoc_min_fat32) ? 1U : 0U);
}

/* `priv_eoc_write()`: see header for the documented contract. */
uint32_t priv_eoc_write(const ra8_fs_mount_t* m)
{
  if (m->type == k_ra8_fs_type_fat12) {
    return k_cluster_eoc_write_fat12;
  }
  if (m->type == k_ra8_fs_type_fat16) {
    return k_cluster_eoc_write_fat16;
  }
  if (m->type == k_ra8_fs_type_exfat) {
    return k_cluster_eoc_write_exfat;
  }
  return k_cluster_eoc_write_fat32;
}

/* `priv_cluster_to_lba()`: see header for the documented contract. */
uint64_t priv_cluster_to_lba(const ra8_fs_mount_t* m, uint32_t cluster)
{
  return m->first_data_lba +
         (((uint64_t)cluster - (uint64_t)k_cluster_first_data) * m->sectors_per_cluster);
}

/**
 * @brief Normalise a next-free hint into a real cluster number.
 *
 * @details A hint is an optimisation, not a promise: it can sit one past the
 *          last cluster after the previous allocation took the final one, and
 *          on a FAT32 volume it can arrive from an `FSI_Nxt_Free` some other
 *          implementation wrote. Anything outside `[2, 2 + count)` restarts
 *          the scan at the first data cluster.
 *
 * @param[in] m    Mount providing the cluster count.
 * @param[in] hint Raw hint from ::priv_alloc_hint_get.
 *
 * @return A cluster number the scan may start at.
 * @retval 2..(2 + count_of_clusters - 1) The clamped start cluster.
 *
 * @pre `m` is non-NULL with `count_of_clusters >= 1`.
 * @pre `hint` came from the allocator state, not from a directory entry.
 * @post No state modified.
 * @post The result addresses a cluster this volume has.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @par MC/DC:
 * Decision: `(hint - k_cluster_first_data) >= count_of_clusters` (1 condition).
 * - hint past the last cluster -> true  -> restart at cluster 2.
 * - an ordinary in-range hint  -> false -> start there.
 * A hint BELOW cluster 2 needs no second condition: the subtraction is
 * unsigned, so it wraps to a value far above any cluster count and the same
 * test catches it. Spelling that out as a second range term would add a
 * condition nothing in the tree can make true -- no caller can produce a hint
 * below 2 -- and an untestable condition is a permanent MC/DC hole.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_alloc_start(const ra8_fs_mount_t* m, uint32_t hint)
{
  if ((hint - (uint32_t)k_cluster_first_data) >= m->count_of_clusters) {
    return (uint32_t)k_cluster_first_data;
  }
  return hint;
}

/* `priv_alloc_cluster()`: see header for the documented contract. */
ra8_err_t priv_alloc_cluster(const ra8_fs_mount_t* m, uint32_t* out_cluster)
{
  /* Start where the last allocation stopped and wrap exactly once, instead of
   * restarting at cluster 2 every time (#607). Combined with the FAT sector
   * cache behind priv_fat_get(), appending to a file costs a bounded number of
   * block reads per cluster rather than one read per cluster EXAMINED -- which
   * is what made writing a K-cluster file O(K * N) real device round trips. */
  const uint32_t past_end = (uint32_t)k_cluster_first_data + m->count_of_clusters;
  uint32_t       c        = priv_alloc_start(m, priv_alloc_hint_get(m));
  for (uint32_t seen = 0U; seen < m->count_of_clusters; seen++) {
    uint32_t  v   = 0;
    ra8_err_t err = priv_fat_get(m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    if (v == k_cluster_free) {
      *out_cluster = c;
      priv_alloc_hint_set(m, c + 1U);
      priv_free_count_took(m, 1U);
      return k_ra8_ok;
    }
    c++;
    if (c >= past_end) {
      c = (uint32_t)k_cluster_first_data;
    }
  }
  return k_ra8_err_no_mem;
}
