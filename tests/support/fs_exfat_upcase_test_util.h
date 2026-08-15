/**
 * @file fs_exfat_upcase_test_util.h
 * @brief The mounted volume's OWN up-case table, expanded for the exFAT scanner.
 *
 * @details
 * `fs_exfat_dir_test_util.h`'s structural scan recomputes each entry set's
 * `NameHash` and compares it against the one stored on disk. That fold has to
 * come from somewhere, and where it comes from is the whole value of the check:
 * folding with the driver's own table would turn an independent verification
 * into a restatement of the thing being verified.
 *
 * So it comes off the VOLUME. This header expands the compressed up-case table
 * the FORMATTER wrote, decoded from the exFAT specification rather than by
 * calling into `ra8_fs`, and hands the scanner a plain array to index.
 *
 * The encoding is the specification's: a unit that is not ::k_upc_run_tag is
 * the mapping for the next code point, and ::k_upc_run_tag introduces a run
 * length of code points that map to themselves. The table ends with a
 * ::k_upc_run_tag that has no length after it -- the mapping for U+FFFF, which
 * is its own up-case -- so a walk that runs out while expecting a run length
 * stops with the identity, which is the right answer there.
 *
 * It lives apart from the scanner for the ordinary reason: the scanner was
 * already near the 1000-line cap, and this is one self-contained mechanism with
 * its own on-disk vocabulary rather than another of its checks. It is
 * deliberately self-contained -- it reaches only for the RAM disk and the
 * geometry on the mount -- so including it costs the scanner nothing but the
 * table.
 *
 * Included by `fs_exfat_dir_test_util.h`; not used directly by a test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "support/fs_fat_exfat_mutate_test_util.h"

/**
 * @enum upc_const_t
 * @brief On-disk constants of the exFAT up-case table.
 *
 * @invariant `k_upc_bmp_units` is the number of UTF-16 code units the table
 *            covers, which is the whole Basic Multilingual Plane.
 * @see upc_load()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_upc_type       = 0x82U,   /**< Up-case-table directory entry type.    */
  k_upc_run_tag    = 0xFFFFU, /**< Introduces a run of identity mappings. */
  k_upc_bmp_units  = 65536U,  /**< Code units the table covers.           */
  k_upc_entry_size = 32U,     /**< Directory entry size, in bytes.        */
  k_upc_root_scan  = 64U,     /**< Root entries searched for the table.   */
} upc_const_t;

/**
 * @var s_upc_table
 * @brief The volume's up-case table, one entry per BMP code unit.
 * @details Filled by ::upc_load from the bytes the formatter wrote. 128 KiB of
 *          test-process memory, which never exists on the target -- the driver
 *          walks the compressed form in place instead.
 * @note Not thread-safe; one suite, one process.
 * @since 0.1.0
 */
static uint16_t s_upc_table[k_upc_bmp_units];

/**
 * @brief Byte offset of @p clus's first byte in the fixture's RAM disk.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number (2-based, as the format counts).
 *
 * @return Offset within `s_disk.bytes`.
 * @retval 0..byte_count The cluster's first byte.
 *
 * @pre @p h is mounted and backed by the fixture's RAM disk.
 * @pre @p clus is at least 2 and inside the cluster heap.
 * @post No state is modified.
 * @post The result is partition-adjusted: the formatter lays the volume inside
 *       an MBR partition, so an un-adjusted offset lands in the gap ahead of it.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0 @details Implements the bounded upc clus byte fixture step using caller-owned state.
 */
RA8_INTERNAL static inline uint32_t internal_upc_clus_byte(const ra8_fs_mount_t* h, uint32_t clus)
{
  const uint32_t lba = h->partition_base_lba + h->first_data_lba +
                       ((uint64_t)(clus - (uint32_t)k_mut_cluster_first) * h->sectors_per_cluster);
  return lba * (uint32_t)k_mut_block_size;
}

/**
 * @brief Read a little-endian 16-bit value out of the fixture's RAM disk.
 *
 * @param[in] off Byte offset within `s_disk.bytes`.
 *
 * @return The value.
 * @retval 0..0xFFFF Whatever the two bytes hold.
 *
 * @pre `s_disk.bytes` is allocated and @p off + 1 is inside it.
 * @pre The caller wants on-disk byte order, which is little-endian.
 * @post No state is modified.
 * @post Written here rather than reused from the driver, so the expansion is
 *       independent of the code whose output it checks.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0 @details Implements the bounded upc rd16 fixture step using caller-owned state.
 */
RA8_INTERNAL static inline uint32_t internal_upc_rd16(uint32_t off)
{
  return (uint32_t)s_disk.bytes[off] |
         ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_mut_shift_byte8);
}

/**
 * @brief Expand the mounted volume's compressed up-case table into ::s_upc_table.
 *
 * @details Walks the root directory for the up-case entry (type ::k_upc_type),
 *          then decodes the compressed form at the cluster it names. Every unit
 *          starts as its own mapping, so a volume with no such entry -- which no
 *          formatted one is -- degrades to the identity rather than to garbage.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Nothing.
 *
 * @pre @p h is mounted, exFAT, and backed by the fixture's RAM disk.
 * @pre The caller runs this before hashing anything against the table.
 * @post ::s_upc_table holds one entry per BMP code unit.
 * @post No volume state is modified.
 *
 * @note Re-run per scan, so a re-formatted volume is always re-read.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_upc_load(const ra8_fs_mount_t* h)
{
  for (uint32_t u = 0U; u < (uint32_t)k_upc_bmp_units; u++) {
    s_upc_table[u] = (uint16_t)u;
  }
  const uint32_t root = internal_upc_clus_byte(h, h->root_cluster);
  for (uint32_t i = 0U; i < (uint32_t)k_upc_root_scan; i++) {
    const uint32_t off = root + (i * (uint32_t)k_upc_entry_size);
    if (s_disk.bytes[off] != (uint8_t)k_upc_type) {
      continue;
    }
    const uint32_t base =
      internal_upc_clus_byte(h, internal_disk_get_u32le(off + (uint32_t)k_mut_strm_off_clus));
    const uint32_t words = internal_disk_get_u32le(off + (uint32_t)k_mut_strm_off_dlen) / 2U;
    uint32_t       idx   = 0U;
    uint32_t       w     = 0U;
    while ((w < words) && (idx < (uint32_t)k_upc_bmp_units)) {
      const uint32_t v = internal_upc_rd16(base + (w * 2U));
      w++;
      if (v != (uint32_t)k_upc_run_tag) {
        s_upc_table[idx] = (uint16_t)v;
        idx++;
        continue;
      }
      if (w >= words) {
        break; /* the trailing tag: U+FFFF, whose up-case is itself */
      }
      idx += internal_upc_rd16(base + (w * 2U));
      w++;
    }
    return;
  }
}
