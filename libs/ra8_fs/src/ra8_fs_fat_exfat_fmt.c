/**
 * @file ra8_fs_fat_exfat_fmt.c
 * @brief exFAT formatter (mkfs) for the `ra8_fs` adapter.
 *
 * @details
 * VBR + checksum construction, FAT/bitmap/up-case seeding, and the root
 * directory writer for a freshly-formatted exFAT volume.
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
 * exFAT format (mkfs)
 * ===========================================================================
 */

/**
 * @struct exfat_geom_t
 * @brief Resolved exFAT volume geometry produced by `priv_exfat_geometry`.
 */
typedef struct {
  uint64_t part_lba;        /**< Partition start LBA (VBR sits here, not 0).  */
  uint64_t total_sectors;   /**< Partition sector count (VolumeLength).       */
  uint32_t bps;             /**< Device sector size in bytes.                 */
  uint8_t  bps_shift;       /**< BytesPerSectorShift (log2 of `bps`).         */
  uint8_t  spc_shift;       /**< SectorsPerClusterShift (log2).               */
  uint32_t spc;             /**< Sectors per cluster (1 << spc_shift).        */
  uint32_t fat_offset;      /**< FatOffset (sectors from VBR).                */
  uint32_t fat_length;      /**< FatLength (sectors).                         */
  uint32_t heap_offset;     /**< ClusterHeapOffset (sectors from VBR).        */
  uint32_t cluster_count;   /**< ClusterCount.                                */
  uint32_t bitmap_bytes;    /**< Allocation-bitmap logical size (DataLength). */
  uint32_t bitmap_clusters; /**< Clusters the bitmap occupies.                */
  uint32_t upcase_cluster;  /**< First cluster of the up-case table.          */
  uint32_t upcase_clusters; /**< Clusters the up-case table occupies.         */
  uint32_t root_cluster;    /**< First cluster of the root directory.         */
  uint32_t used_clusters;   /**< bitmap + up-case + root (pre-allocated).     */
} exfat_geom_t;

/* `priv_exfat_csum32()`: see header for the documented contract. */
uint32_t priv_exfat_csum32(uint32_t cs, const uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    cs = (((cs & 1U) != 0U) ? (uint32_t)k_exfat_fmt_csum_hibit : 0U) + (cs >> 1) + (uint32_t)buf[i];
  }
  return cs;
}

/**
 * @brief Return the default SectorsPerClusterShift for an exFAT volume.
 *
 * @details Implements the size-tiered lookup from Microsoft exFAT spec
 *          section 12.1, expressed in cluster BYTES so every sector size lands
 *          on the same cluster size a 512-byte device of equal capacity gets:
 *          up to 256 MB uses 4 kB clusters, up to 32 GB uses 32 kB, up to
 *          256 GB uses 128 kB, and larger devices use 256 kB clusters. The
 *          thresholds are counts of 512-byte-equivalent sectors, so the
 *          device's count is scaled by `bps / 512` first; the returned shift
 *          is `log2(cluster bytes) - bps_shift`, the sectors-per-cluster
 *          shift the VBR records.
 *
 * @param[in] total_sectors Whole-device sector count (device sectors).
 * @param[in] bps_shift     log2 of the device sector size (9..12).
 *
 * @return SectorsPerClusterShift value for the exFAT VBR.
 * @retval 0 Cluster equals one sector (4 kB clusters on a 4Kn device).
 * @retval 9 256 kB clusters on a 512-byte device.
 *
 * @pre @p total_sectors is the actual backend capacity in device sectors.
 * @pre @p total_sectors is non-zero; @p bps_shift is 9..12.
 * @post Return value plus @p bps_shift is one of the four cluster tiers.
 * @post Return value is a valid SectorsPerClusterShift for the exFAT spec.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_exfat_spc_shift(uint64_t total_sectors, uint8_t bps_shift)
{
  /* The subtraction is guarded even though every caller passes 9..12: the
   * analyzer cannot see the cross-function bound, and an under-9 shift here
   * would be a 2^32-sized left shift rather than a wrong answer. */
  const uint32_t scale      = ((uint32_t)bps_shift > (uint32_t)k_exfat_bps_shift_min)
                                ? ((uint32_t)bps_shift - (uint32_t)k_exfat_bps_shift_min)
                                : 0U;
  const uint64_t total_512  = total_sectors << scale;
  uint32_t       clus_shift = (uint32_t)k_exfat_fmt_clus_256k;
  if (total_512 <= (uint64_t)k_exfat_fmt_thr_256m) {
    clus_shift = (uint32_t)k_exfat_fmt_clus_4k;
  } else if (total_512 <= (uint64_t)k_exfat_fmt_thr_32g) {
    clus_shift = (uint32_t)k_exfat_fmt_clus_32k;
  } else if (total_512 <= (uint64_t)k_exfat_fmt_thr_256g) {
    clus_shift = (uint32_t)k_exfat_fmt_clus_128k;
  } else {
    clus_shift = (uint32_t)k_exfat_fmt_clus_256k;
  }
  if (clus_shift <= (uint32_t)bps_shift) {
    return 0U;
  }
  return (uint8_t)(clus_shift - (uint32_t)bps_shift);
}

/**
 * @brief Resolve the exFAT region geometry for a device of @p total_sectors.
 *
 * @details Picks the cluster size, fixed-points the FAT length against the
 *          cluster count (the FAT shrinks the heap which shrinks the count
 *          which shrinks the FAT), then lays the allocation bitmap, up-case
 *          table, and root directory as the first contiguous clusters of the
 *          heap.
 *
 * @param[in]  total_sectors Partition sector count (device sectors).
 * @param[out] g             Receives the resolved geometry (`bps` / `bps_shift`
 *                           already populated by the caller).
 *
 * @return Error code.
 * @retval k_ra8_ok               Geometry resolved.
 * @retval k_ra8_err_invalid_size Device too small to hold a volume.
 *
 * @pre @p g is non-NULL with `bps` and `bps_shift` set.
 * @pre @p total_sectors is the partition capacity in device sectors (non-zero).
 * @post On k_ra8_ok, every field of @p g is consistent with the exFAT spec.
 * @post On k_ra8_err_invalid_size, @p g may be partially written and must be
 *       discarded.
 *
 * @note Bounded loop (NASA Rule 2): `k_exfat_fmt_geom_iters` passes.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_geometry(uint64_t total_sectors, exfat_geom_t* g)
{
  g->total_sectors = total_sectors;
  g->spc_shift     = internal_exfat_spc_shift(total_sectors, g->bps_shift);
  g->spc           = 1U << g->spc_shift;
  g->fat_offset    = (uint32_t)k_exfat_fmt_boot_secs;
  if (total_sectors <= g->fat_offset) {
    return k_ra8_err_invalid_size;
  }
  uint32_t clusters = (uint32_t)((total_sectors - g->fat_offset) / g->spc);
  g->fat_length     = 0U;
  g->heap_offset    = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_fmt_geom_iters; i++) {
    const uint64_t fat_bytes = ((uint64_t)clusters + 2U) * 4U;
    const uint32_t fat_len   = (uint32_t)((fat_bytes + g->bps - 1U) / g->bps);
    const uint32_t heap      = g->fat_offset + fat_len;
    if (total_sectors <= heap) {
      return k_ra8_err_invalid_size;
    }
    const uint32_t next = (uint32_t)((total_sectors - heap) / g->spc);
    g->fat_length       = fat_len;
    g->heap_offset      = heap;
    if (next == clusters) {
      break;
    }
    clusters = next;
  }
  g->cluster_count             = (uint32_t)((total_sectors - g->heap_offset) / g->spc);
  const uint32_t cluster_bytes = g->spc * g->bps;
  g->bitmap_bytes =
    (g->cluster_count + (uint32_t)k_exfat_fmt_byte_bits - 1U) / (uint32_t)k_exfat_fmt_byte_bits;
  g->bitmap_clusters = (g->bitmap_bytes + cluster_bytes - 1U) / cluster_bytes;
  g->upcase_clusters = ((uint32_t)k_exfat_fmt_upc_std_bytes + cluster_bytes - 1U) / cluster_bytes;
  g->upcase_cluster  = (uint32_t)k_exfat_fmt_first_clus + g->bitmap_clusters;
  g->root_cluster    = g->upcase_cluster + g->upcase_clusters;
  g->used_clusters   = g->bitmap_clusters + g->upcase_clusters + 1U; /* bitmap + up-case + root */
  if (g->cluster_count < g->used_clusters) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Build the 512-byte exFAT Main Boot Sector (VBR) into @p sec.
 *
 * @details Zeroes @p sec, then writes all mandatory exFAT VBR fields: the
 *          3-byte JMP instruction, the "EXFAT   " file-system name, PartitionOffset,
 *          VolumeLength, FatOffset, FatLength, ClusterHeapOffset, ClusterCount, RootDirectory
 *          first cluster, VolumeSerialNumber (XOR of constant and total_sectors),
 *          FileSystemRevision (1.00), BytesPerSectorShift, SectorsPerClusterShift,
 *          NumberOfFats, PercentInUse, and the 0xAA55 boot signature. All values
 *          are written as little-endian fields per the Microsoft exFAT spec.
 *
 * @param[out] sec Caller-provided sector buffer to receive the VBR.
 * @param[in]  g   Resolved exFAT geometry from priv_exfat_geometry().
 *
 * @return Nothing.
 *
 * @pre @p sec is non-NULL and points to at least `g->bps` writable bytes.
 * @pre @p g is non-NULL and was filled by a successful priv_exfat_geometry() call.
 * @post @p sec holds a spec-compliant exFAT Main Boot Sector.
 * @post All `g->bps` bytes of @p sec have been written (zeroed, then fields set).
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_exfat_build_vbr(uint8_t* sec, const exfat_geom_t* g)
{
  for (uint32_t i = 0U; i < g->bps; i++) {
    sec[i] = 0U;
  }
  sec[(uint32_t)k_exfat_foff_jump]         = (uint8_t)k_exfat_fmt_jump0;
  sec[(uint32_t)k_exfat_foff_jump + 1U]    = (uint8_t)k_exfat_fmt_jump1;
  sec[(uint32_t)k_exfat_foff_jump + 2U]    = (uint8_t)k_exfat_fmt_jump2;
  static const char nm[k_exfat_fsname_len] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
  for (uint32_t i = 0U; i < (uint32_t)k_exfat_fsname_len; i++) {
    sec[(uint32_t)k_exfat_off_fsname + i] = (uint8_t)nm[i];
  }
  priv_wr64(&sec[k_exfat_foff_part_off], g->part_lba);
  priv_wr64(&sec[k_exfat_foff_vol_len], g->total_sectors);
  priv_wr32(&sec[k_exfat_off_fat_lba], g->fat_offset);
  priv_wr32(&sec[k_exfat_off_fat_len], g->fat_length);
  priv_wr32(&sec[k_exfat_off_heap_lba], g->heap_offset);
  priv_wr32(&sec[k_exfat_off_clus_count], g->cluster_count);
  priv_wr32(&sec[k_exfat_off_root_clus], g->root_cluster);
  priv_wr32(&sec[k_exfat_foff_serial], (uint32_t)k_exfat_fmt_serial ^ (uint32_t)g->total_sectors);
  priv_wr16(&sec[k_exfat_foff_fs_rev], (uint16_t)k_exfat_fmt_fs_rev);
  sec[k_exfat_off_bps_shift]          = g->bps_shift;
  sec[k_exfat_off_spc_shift]          = g->spc_shift;
  sec[k_exfat_off_num_fats]           = (uint8_t)k_exfat_fmt_num_fats;
  sec[(uint32_t)k_exfat_foff_drive]   = (uint8_t)k_exfat_fmt_drive;
  sec[(uint32_t)k_exfat_foff_percent] = (uint8_t)k_exfat_fmt_percent;
  priv_wr16(&sec[k_exfat_foff_boot_sig], (uint16_t)k_exfat_fmt_boot_sig);
}

/**
 * @brief Write a sector to both its primary and backup locations in the exFAT boot region.
 *
 * @details Calls @p b->write_block twice: once for the primary copy at @p lba, and
 *          once for the backup copy at @p lba + k_exfat_fmt_backup_lba (12 sectors
 *          later, per the Microsoft exFAT spec). The first write failure aborts;
 *          the backup write is skipped to avoid partial state.
 *
 * @param[in] b        Block-device backend with a non-NULL `write_block` function.
 * @param[in] part_lba Partition start LBA added to every write (0 = whole-disk).
 * @param[in] lba      Partition-relative primary LBA (0 <= lba < k_exfat_fmt_boot_secs).
 * @param[in] buf      One whole-sector image to write.
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    Both primary and backup copies written successfully.
 * @retval k_ra8_err_* The primary or backup write_block call failed.
 *
 * @pre @p b and @p b->write_block are non-NULL.
 * @pre @p buf is non-NULL and holds one whole device sector.
 * @post On k_ra8_ok, sectors @p lba and @p lba+k_exfat_fmt_backup_lba are identical.
 * @post On failure, the primary may be written but the backup state is undefined.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_wr_dual(const ra8_fs_backend_t* b,
                                        uint64_t                part_lba,
                                        uint32_t                lba,
                                        const uint8_t*          buf)
{
  const ra8_err_t err = b->write_block(b->ctx, part_lba + lba, 1U, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  return b->write_block(b->ctx, part_lba + lba + (uint32_t)k_exfat_fmt_backup_lba, 1U, buf);
}

/**
 * @brief Write a 32-bit little-endian word at slot @p idx in a sector buffer.
 *
 * @details Computes the byte offset as `idx * 4` and calls priv_wr32() to
 *          store @p val in little-endian order. Used to populate FAT entries
 *          and checksum-sector words without repeating the stride arithmetic.
 *
 * @param[in,out] buf Sector buffer to write into.
 * @param[in]     idx Zero-based 32-bit word index within @p buf.
 * @param[in]     val 32-bit value to store.
 *
 * @return Nothing.
 *
 * @pre @p buf is non-NULL and has at least (idx+1)*4 writable bytes.
 * @pre @p idx does not overflow the sector (caller verifies the range).
 * @post Bytes buf[idx*4 .. idx*4+3] hold @p val in little-endian order.
 * @post No bytes outside the four-byte target word are modified.
 *
 * @note Not thread-safe; the caller serialises access to @p buf.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_exfat_put32(uint8_t* buf, uint32_t idx, uint32_t val)
{
  priv_wr32(&buf[(size_t)idx * 4U], val);
}

/**
 * @brief Write exFAT boot sectors 1-11 (ext-boot, OEM, reserved, checksum) to main and backup.
 *
 * @details Takes the running boot checksum @p cs (which already covers sector 0)
 *          and completes the boot region. Sectors 1-8 each receive an extended
 *          boot sector with only the ExtendedBootSignature (0xAA550000) set;
 *          sector 9 (OEM) and sector 10 (reserved) are all-zero. Each sector is
 *          folded into @p cs via priv_exfat_csum32() before being written via
 *          priv_exfat_wr_dual() to both the main and backup regions. Sector 11
 *          (checksum) is then filled with k_exfat_fmt_csum_copies copies of the
 *          final checksum word and written to both regions. The global priv_scratch
 *          buffer is used as a scratch pad.
 *
 * @param[in] backend  Block-device backend with a non-NULL write_block hook.
 * @param[in] g        Resolved geometry (partition base + sector size).
 * @param[in] cs       Running boot checksum accumulated over sector 0.
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    All sectors written to both main and backup regions.
 * @retval k_ra8_err_* Backend write_block failure; format is aborted.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p cs reflects the checksum folded over sector 0 of the VBR.
 * @post On k_ra8_ok, sectors 1-11 and their backups (13-23) are written.
 * @post The checksum sector (11 and 23) contains the finalised boot checksum.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_exfat_write_boot_tail(const ra8_fs_backend_t* backend, const exfat_geom_t* g, uint32_t cs)
{
  /* Sectors 1-8: extended boot (ext-boot signature only). The extended boot
   * signature sits in the sector's LAST four bytes (exFAT spec sec 3.2). */
  for (uint32_t i = 0U; i < g->bps; i++) {
    priv_scratch[i] = 0U;
  }
  priv_wr32(
    &priv_scratch[g->bps - ((uint32_t)k_ra8_fs_sector_min - (uint32_t)k_exfat_foff_ext_sig)],
    (uint32_t)k_exfat_fmt_ext_sig);
  for (uint32_t s = (uint32_t)k_exfat_fmt_ext_first;
       s < (uint32_t)k_exfat_fmt_ext_first + (uint32_t)k_exfat_fmt_ext_count;
       s++) {
    cs                = priv_exfat_csum32(cs, priv_scratch, g->bps);
    const ra8_err_t e = internal_exfat_wr_dual(backend, g->part_lba, s, priv_scratch);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  /* Sectors 9 (OEM) + 10 (reserved): all zero. */
  for (uint32_t i = 0U; i < g->bps; i++) {
    priv_scratch[i] = 0U;
  }
  cs = priv_exfat_csum32(cs, priv_scratch, g->bps);
  ra8_err_t err =
    internal_exfat_wr_dual(backend, g->part_lba, (uint32_t)k_exfat_fmt_oem_lba, priv_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  cs  = priv_exfat_csum32(cs, priv_scratch, g->bps);
  err = internal_exfat_wr_dual(backend, g->part_lba, (uint32_t)k_exfat_fmt_resv_lba, priv_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Checksum sector: every 32-bit word = the boot checksum. */
  for (uint32_t i = 0U; i < (g->bps / 4U); i++) {
    internal_exfat_put32(priv_scratch, i, cs);
  }
  return internal_exfat_wr_dual(backend, g->part_lba, (uint32_t)k_exfat_fmt_csum_lba, priv_scratch);
}

/**
 * @brief Write the complete exFAT main and backup boot regions (sectors 0-23).
 *
 * @details Builds the VBR in priv_scratch via priv_exfat_build_vbr(), then
 *          accumulates the boot checksum over sector 0, skipping the three bytes
 *          that the exFAT spec excludes from the checksum: VolumeFlags bytes
 *          (offsets 106-107) and PercentInUse (offset 112). The three ranges
 *          [0, 106), [108, 112), and [113, 512) are folded via priv_exfat_csum32().
 *          Sector 0 is then written to both main (LBA 0) and backup (LBA 12) by
 *          priv_exfat_wr_dual(). The remaining boot sectors 1-11 and their backups
 *          are written by priv_exfat_write_boot_tail() using the running checksum.
 *
 * @param[in] backend Block-device backend with a non-NULL write_block hook.
 * @param[in] g       Resolved exFAT geometry from priv_exfat_geometry().
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    Both main and backup boot regions (sectors 0-23) written.
 * @retval k_ra8_err_* Backend write failure; format is aborted.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p g was filled by a successful priv_exfat_geometry() call.
 * @post On k_ra8_ok, sectors 0-23 on the device hold spec-compliant boot regions.
 * @post The boot checksum stored in sectors 11 and 23 covers sectors 0-10.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_write_boot(const ra8_fs_backend_t* backend, const exfat_geom_t* g)
{
  internal_exfat_build_vbr(priv_scratch, g);
  /* Boot checksum over sector 0 skips bytes 106,107 (VolumeFlags) and 112
   * (PercentInUse): fold [0,106), [108,112), [113,bps). */
  uint32_t cs = priv_exfat_csum32(0U, &priv_scratch[0], (uint32_t)k_exfat_fmt_csum_skip0);
  cs = priv_exfat_csum32(cs,
                         &priv_scratch[k_exfat_off_bps_shift],
                         (uint32_t)k_exfat_fmt_csum_skip2 - (uint32_t)k_exfat_off_bps_shift);
  cs = priv_exfat_csum32(cs,
                         &priv_scratch[(uint32_t)k_exfat_fmt_csum_skip2 + 1U],
                         g->bps - ((uint32_t)k_exfat_fmt_csum_skip2 + 1U));
  const ra8_err_t err = internal_exfat_wr_dual(backend, g->part_lba, 0U, priv_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_exfat_write_boot_tail(backend, g, cs);
}

/**
 * @brief Zero the exFAT FAT region, then seed the media, EOC, and chain entries.
 *
 * @details Clears all FAT sectors via priv_fmt_clear_region(), then builds the
 *          first FAT sector in priv_scratch. FAT[0] is set to the media byte
 *          (k_exfat_fmt_fat_media) and FAT[1] to the end-of-chain sentinel
 *          (k_exfat_fmt_fat_eoc). The bitmap and up-case cluster chains are each
 *          written as a linked run terminating with EOC (the up-case table may
 *          span more than one cluster), and the root directory cluster receives
 *          a single EOC entry. Finally, the populated first sector is written to
 *          the FAT start LBA (partition-adjusted).
 *
 * @param[in] backend Block-device backend with non-NULL write_block.
 * @param[in] g       Resolved exFAT geometry from priv_exfat_geometry().
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    FAT region zeroed and initial entries written.
 * @retval k_ra8_err_* Backend clear or write failure.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p g was filled by a successful priv_exfat_geometry() call.
 * @post On k_ra8_ok, FAT[0..1] and the pre-allocated cluster chains are seeded.
 * @post The remainder of the FAT region beyond the first sector reads as zero.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_write_fat(const ra8_fs_backend_t* backend, const exfat_geom_t* g)
{
  ra8_err_t err =
    priv_fmt_clear_region(backend, g->part_lba + g->fat_offset, g->fat_length, g->bps);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < g->bps; i++) {
    priv_scratch[i] = 0U;
  }
  internal_exfat_put32(priv_scratch, 0U, (uint32_t)k_exfat_fmt_fat_media);
  internal_exfat_put32(priv_scratch, 1U, (uint32_t)k_exfat_fmt_fat_eoc);
  const uint32_t bmp_last = (uint32_t)k_exfat_fmt_first_clus + g->bitmap_clusters - 1U;
  for (uint32_t c = (uint32_t)k_exfat_fmt_first_clus; c <= bmp_last; c++) {
    const uint32_t next = (c < bmp_last) ? (c + 1U) : (uint32_t)k_exfat_fmt_fat_eoc;
    internal_exfat_put32(priv_scratch, c, next);
  }
  const uint32_t upc_last = g->upcase_cluster + g->upcase_clusters - 1U;
  for (uint32_t c = g->upcase_cluster; c <= upc_last; c++) {
    const uint32_t next = (c < upc_last) ? (c + 1U) : (uint32_t)k_exfat_fmt_fat_eoc;
    internal_exfat_put32(priv_scratch, c, next);
  }
  internal_exfat_put32(priv_scratch, g->root_cluster, (uint32_t)k_exfat_fmt_fat_eoc);
  return backend->write_block(backend->ctx, g->part_lba + g->fat_offset, 1U, priv_scratch);
}

/**
 * @brief Zero the allocation bitmap region, then mark the pre-allocated clusters.
 *
 * @details Clears all bitmap sectors via priv_fmt_clear_region(), then builds
 *          the first bitmap sector in priv_scratch. The g->used_clusters count
 *          (bitmap + up-case + root) is marked allocated: full bytes are set to
 *          0xFF and the trailing partial byte gets a mask of the remaining bits.
 *          The populated sector is then written to the bitmap's heap LBA.
 *
 * @param[in] backend Block-device backend with non-NULL write_block.
 * @param[in] g       Resolved exFAT geometry from priv_exfat_geometry().
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    Bitmap region zeroed and pre-allocated clusters marked.
 * @retval k_ra8_err_* Backend clear or write failure.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p g was filled by a successful priv_exfat_geometry() call.
 * @post On k_ra8_ok, the first g->used_clusters bits of the bitmap are set to 1.
 * @post Remaining bitmap bits beyond g->used_clusters read as zero.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_write_bitmap(const ra8_fs_backend_t* backend, const exfat_geom_t* g)
{
  const uint64_t bmp_lba     = g->part_lba + g->heap_offset;
  const uint32_t bmp_sectors = g->bitmap_clusters * g->spc;
  ra8_err_t      err         = priv_fmt_clear_region(backend, bmp_lba, bmp_sectors, g->bps);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < g->bps; i++) {
    priv_scratch[i] = 0U;
  }
  const uint32_t full = g->used_clusters / (uint32_t)k_exfat_fmt_byte_bits;
  for (uint32_t i = 0U; i < full; i++) {
    priv_scratch[i] = (uint8_t)k_exfat_fmt_byte_full;
  }
  const uint32_t rem = g->used_clusters % (uint32_t)k_exfat_fmt_byte_bits;
  if (rem != 0U) {
    priv_scratch[full] = (uint8_t)((1U << rem) - 1U);
  }
  return backend->write_block(backend->ctx, bmp_lba, 1U, priv_scratch);
}

/**
 * @brief Pack an ASCII volume label into a UTF-16LE buffer for the exFAT VolumeLabel entry.
 *
 * @details Converts up to k_exfat_fmt_label_max ASCII characters of @p label
 *          into UTF-16LE by zero-extending each byte: the low byte of each
 *          UTF-16 code-unit receives the ASCII character and the high byte is
 *          set to zero. Stops at the NUL terminator or the 11-character limit,
 *          whichever comes first. A NULL @p label writes nothing and returns 0.
 *
 * @param[out] dst   Destination buffer for the UTF-16LE character array (at
 *                   least k_exfat_fmt_label_max * 2 bytes).
 * @param[in]  label NUL-terminated ASCII label, or NULL for no label.
 *
 * @return Number of UTF-16 code units written to @p dst.
 * @retval 0  @p label is NULL or is an empty string.
 * @retval 11 @p label has 11 or more non-NUL characters.
 *
 * @pre @p dst is non-NULL and has at least k_exfat_fmt_label_max * 2 writable bytes.
 * @pre @p label, if non-NULL, is NUL-terminated and contains only ASCII characters.
 * @post @p dst[0 .. (return*2)-1] holds the UTF-16LE label characters.
 * @post No byte beyond the written range of @p dst is modified.
 *
 * @note Pure function on the output buffer; trivially thread-safe on distinct buffers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_exfat_label_utf16(uint8_t* dst, const char* label)
{
  uint32_t n = 0U;
  if (label != nullptr) {
    for (; (n < (uint32_t)k_exfat_fmt_label_max) && (label[n] != '\0'); n++) {
      dst[(size_t)n * 2U]        = (uint8_t)label[n];
      dst[((size_t)n * 2U) + 1U] = 0U;
    }
  }
  return n;
}

/**
 * @brief Write the exFAT root directory entry set (bitmap, up-case, label).
 *
 * @details Builds the root directory sector in priv_scratch and writes it to the
 *          root cluster in the heap. The sector contains three directory entries:
 *          an Allocation Bitmap entry (0x81) pointing to the first cluster of
 *          the bitmap and recording bitmap_bytes as the DataLength; an Up-case
 *          Table entry (0x82) with @p upcase_csum, the up-case cluster, and
 *          k_exfat_fmt_upc_std_bytes as the DataLength; and a Volume Label entry
 *          (0x83) with the label packed into UTF-16LE via priv_exfat_label_utf16().
 *
 * @param[in] backend     Block-device backend with non-NULL write_block.
 * @param[in] g           Resolved exFAT geometry from priv_exfat_geometry().
 * @param[in] upcase_csum Rotate-add checksum of the up-case table data.
 * @param[in] label       Optional NUL-terminated ASCII volume label, or NULL.
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    Root directory sector written to the root cluster.
 * @retval k_ra8_err_* Backend write_block failure.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p g was filled by a successful priv_exfat_geometry() call.
 * @post On k_ra8_ok, the root cluster contains the three mandatory directory entries.
 * @post Every sector of the root cluster past the first reads as zero.
 * @post The label entry holds the UTF-16LE encoding of @p label (or zero length if NULL).
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_write_root(const ra8_fs_backend_t* backend,
                                           const exfat_geom_t*     g,
                                           uint32_t                upcase_csum,
                                           const char*             label)
{
  const uint64_t lba = (uint64_t)g->heap_offset +
                       ((uint64_t)(g->root_cluster - (uint32_t)k_exfat_fmt_first_clus) * g->spc);
  /* Zero the WHOLE root cluster, not just the sector the three system entries
   * land in. The rest must read as end-of-directory (0x00), or a directory that
   * fills past the first sector -- or a rename that relocates an entry set into
   * it -- runs the scan straight into whatever the device held before the format
   * (#603). Directory growth already zeroes every cluster it appends; the initial
   * root cluster was the one spot that skipped it. */
  const ra8_err_t ze = priv_fmt_clear_region(backend, g->part_lba + lba, g->spc, g->bps);
  if (ze != k_ra8_ok) {
    return ze;
  }
  for (uint32_t i = 0U; i < g->bps; i++) {
    priv_scratch[i] = 0U;
  }
  /* Allocation Bitmap entry (0x81). */
  priv_scratch[0] = (uint8_t)k_exfat_entry_bitmap;
  priv_wr32(&priv_scratch[k_exfat_de_first_clus], (uint32_t)k_exfat_fmt_first_clus);
  priv_wr32(&priv_scratch[k_exfat_de_data_len], g->bitmap_bytes);
  /* Up-case Table entry (0x82). */
  uint8_t* upc = &priv_scratch[k_exfat_de_second];
  upc[0]       = (uint8_t)k_exfat_entry_upcase;
  priv_wr32(&upc[k_exfat_de_upc_csum], upcase_csum);
  priv_wr32(&upc[k_exfat_de_first_clus], g->upcase_cluster);
  priv_wr32(&upc[k_exfat_de_data_len], (uint32_t)k_exfat_fmt_upc_std_bytes);
  /* Volume Label entry (0x83). */
  uint8_t*       lbl      = &priv_scratch[k_exfat_de_third];
  const uint32_t n        = internal_exfat_label_utf16(&lbl[k_exfat_de_lbl_name], label);
  lbl[0]                  = (uint8_t)k_exfat_entry_label;
  lbl[k_exfat_de_lbl_cnt] = (uint8_t)n;
  return backend->write_block(backend->ctx, g->part_lba + lba, 1U, priv_scratch);
}

/**
 * @brief Encode an LBA as legacy CHS bytes using the 255-head / 63-sector geometry.
 *
 * @details Fills the three-byte CHS field an MBR partition entry carries.
 *          Above ::k_mbr_fmt_chs_max (the first LBA the 1024-cylinder CHS space
 *          cannot address) the universal 0xFE/0xFF/0xFF "beyond CHS, use LBA"
 *          sentinel is written, matching what `fdisk`/`sfdisk` emit for large
 *          disks; below it a real cylinder/head/sector triple is packed
 *          (sector in bits[5:0], cylinder bits[9:8] in bits[7:6] of the second
 *          byte, cylinder low byte in the third).
 *
 * @param[in]  lba  Logical block address to encode.
 * @param[out] out3 Three-byte CHS field to fill.
 *
 * @return Nothing.
 *
 * @pre @p out3 points to at least 3 writable bytes.
 * @pre @p lba is the sector this CHS field should describe.
 * @post @p out3 holds the packed CHS (or the overflow sentinel).
 * @post No bytes outside @p out3[0..2] are modified.
 *
 * @note Pure aside from @p out3; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_exfat_lba_to_chs(uint32_t lba, uint8_t* out3)
{
  if (lba >= (uint32_t)k_mbr_fmt_chs_max) {
    out3[0] = (uint8_t)k_mbr_fmt_chs_ovf_h;
    out3[1] = (uint8_t)k_mbr_fmt_chs_ovf_m;
    out3[2] = (uint8_t)k_mbr_fmt_chs_ovf_l;
    return;
  }
  const uint32_t spt  = (uint32_t)k_mbr_fmt_chs_spt;
  const uint32_t hpc  = (uint32_t)k_mbr_fmt_chs_heads;
  const uint32_t cyl  = lba / (hpc * spt);
  const uint32_t head = (lba / spt) % hpc;
  const uint32_t sec  = (lba % spt) + 1U;
  out3[0]             = (uint8_t)head;
  out3[1]             = (uint8_t)((sec & (uint32_t)k_mbr_fmt_chs_sec_mask) |
                                  ((cyl & (uint32_t)k_mbr_fmt_chs_cyl_hi_mask) >>
                                   (uint32_t)k_mbr_fmt_chs_cyl_hi_shift));
  out3[2]             = (uint8_t)(cyl & (uint32_t)k_byte_mask);
}

/**
 * @brief Write the MBR (LBA 0) with one type-0x07 exFAT partition at @p part_lba.
 *
 * @details Zeroes the sector, stamps a disk signature at offset 440, lays a
 *          single primary partition entry at offset 446 (non-bootable, type
 *          0x07 exFAT/NTFS, packed start/end CHS via priv_exfat_lba_to_chs(),
 *          32-bit start LBA and sector count), and writes the 0x55AA boot
 *          signature. A PC then sees standard partitioned removable media
 *          rather than a "superfloppy" volume at sector 0.
 *
 * @param[in] backend      Block-device backend with a non-NULL write_block hook.
 * @param[in] bps          Device sector size in bytes.
 * @param[in] part_lba     First LBA of the exFAT partition (1 MiB aligned).
 * @param[in] part_sectors Partition length in sectors.
 *
 * @return Error code from the backend.
 * @retval k_ra8_ok    MBR written at LBA 0.
 * @retval k_ra8_err_* Backend write_block failure.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p part_lba + @p part_sectors does not exceed the device capacity.
 * @post On k_ra8_ok, LBA 0 holds an MBR describing the exFAT partition.
 * @post Byte 510/511 hold the 0x55/0xAA signature.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_write_mbr(const ra8_fs_backend_t* backend,
                                          uint32_t                bps,
                                          uint32_t                part_lba,
                                          uint32_t                part_sectors)
{
  for (uint32_t i = 0U; i < bps; i++) {
    priv_scratch[i] = 0U;
  }
  priv_wr32(&priv_scratch[k_mbr_fmt_disk_sig_off],
            (uint32_t)k_mbr_fmt_disk_sig_base ^ part_sectors);
  uint8_t* pe           = &priv_scratch[k_mbr_fmt_part0_off];
  pe[k_mbr_fmt_pe_boot] = (uint8_t)k_mbr_fmt_boot_none;
  internal_exfat_lba_to_chs(part_lba, &pe[k_mbr_fmt_pe_chs_start]);
  pe[k_mbr_fmt_pe_type] = (uint8_t)k_mbr_fmt_type_exfat;
  internal_exfat_lba_to_chs((part_lba + part_sectors) - 1U, &pe[k_mbr_fmt_pe_chs_end]);
  priv_wr32(&pe[k_mbr_fmt_pe_lba], part_lba);
  priv_wr32(&pe[k_mbr_fmt_pe_nsect], part_sectors);
  priv_wr16(&priv_scratch[k_exfat_foff_boot_sig], (uint16_t)k_exfat_fmt_boot_sig);
  return backend->write_block(backend->ctx, 0U, 1U, priv_scratch);
}

/**
 * @brief Size and place the exFAT partition on the device, or refuse.
 *
 * @details The preamble of ::priv_exfat_format, split out so the emit sequence
 *          stays inside the function-size gate: derives `log2(bps)`, aligns
 *          the partition at 1 MiB, and applies the two size refusals -- a
 *          device too small for the minimum volume plus the alignment gap,
 *          and one whose partition would not fit the MBR's 32-bit fields
 *          (past-2-TiB media arrive GPT-partitioned; mounting them is the
 *          supported half, #683).
 *
 * @param[in]  total_sectors    Whole-device sector count.
 * @param[in]  bps              Device sector size (a power of two, 512..4096).
 * @param[out] out_bps_shift    Receives `log2(bps)`.
 * @param[out] out_part_lba     Receives the partition's first LBA.
 * @param[out] out_part_sectors Receives the partition's sector count.
 *
 * @return Error code.
 * @retval k_ra8_ok                The partition is sized and placed.
 * @retval k_ra8_err_not_supported Device too small, or past the MBR's fields.
 *
 * @pre @p bps was validated by the format entry point.
 * @pre Every output pointer is non-NULL.
 * @post On success the three outputs are populated.
 * @post No device state is touched on any path.
 *
 * @note Pure computation; trivially thread-safe.
 *
 * @note The two size refusals are single-condition decisions, driven by the
 *       formatter tests' too-small and (new) beyond-2-TiB vectors.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exfat_fmt_place(uint64_t  total_sectors,
                                          uint32_t  bps,
                                          uint8_t*  out_bps_shift,
                                          uint32_t* out_part_lba,
                                          uint32_t* out_part_sectors)
{
  uint32_t bps_shift = 0U;
  for (uint32_t w = bps; w > 1U; w >>= 1U) {
    bps_shift++;
  }
  const uint32_t part_lba = (uint32_t)k_exfat_fmt_part_align / bps;
  const uint64_t min_total =
    (uint64_t)part_lba +
    (((uint64_t)k_exfat_fmt_min_sectors * (uint32_t)k_ra8_fs_sector_min) / bps);
  if (total_sectors < min_total) {
    return k_ra8_err_not_supported;
  }
  /* The MBR this formatter writes records the partition in 32-bit fields, so
   * a device whose partition would not fit them (past 2 TiB at 512-byte
   * sectors) is refused with a clean error rather than a truncated table.
   * Beyond-2-TiB media formatted elsewhere (GPT) mount and address fine. */
  if ((total_sectors - part_lba) > (uint64_t)UINT32_MAX) {
    return k_ra8_err_not_supported;
  }
  *out_bps_shift    = (uint8_t)bps_shift;
  *out_part_lba     = part_lba;
  *out_part_sectors = (uint32_t)(total_sectors - part_lba);
  return k_ra8_ok;
}

/* `priv_exfat_format()`: see header for the documented contract. */
ra8_err_t priv_exfat_format(const ra8_fs_backend_t* backend,
                            uint64_t                total_sectors,
                            uint32_t                bps,
                            const char*             label)
{
  uint8_t   bps_shift    = 0U;
  uint32_t  part_lba     = 0U;
  uint32_t  part_sectors = 0U;
  ra8_err_t err =
    internal_exfat_fmt_place(total_sectors, bps, &bps_shift, &part_lba, &part_sectors);
  if (err != k_ra8_ok) {
    return err;
  }
  exfat_geom_t g = {.bps = bps, .bps_shift = bps_shift};
  err            = internal_exfat_geometry(part_sectors, &g);
  if (err != k_ra8_ok) {
    return err;
  }
  g.part_lba = part_lba;
  /* The bitmap/up-case/root chains are seeded in FAT sector 0 only. */
  if ((g.root_cluster * 4U) >= bps) {
    return k_ra8_err_not_supported;
  }
  err = internal_exfat_write_mbr(backend, bps, part_lba, part_sectors);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_exfat_write_boot(backend, &g);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_exfat_write_fat(backend, &g);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_exfat_write_bitmap(backend, &g);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint64_t upcase_lba =
    g.part_lba + g.heap_offset +
    ((uint64_t)(g.upcase_cluster - (uint32_t)k_exfat_fmt_first_clus) * g.spc);
  uint32_t upcase_csum = 0U;
  err                  = priv_exfat_write_upcase(backend, upcase_lba, bps, &upcase_csum);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_exfat_write_root(backend, &g, upcase_csum, label);
}
