/**
 * @file ra8_fs_fat_fmt.c
 * @brief FAT12/16/32 formatter (mkfs) for the `ra8_fs` adapter.
 *
 * @details
 * Geometry selection, BPB/FSInfo construction, FAT seeding, and the
 * region-clear helpers shared with the exFAT formatter.
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
 * Public API: format (mkfs)
 * =============================================================================
 */

/* `priv_fmt_reserved_for()`: see header for the documented contract. */
uint32_t priv_fmt_reserved_for(ra8_fs_type_t type)
{
  return (type == k_ra8_fs_type_fat32) ? (uint32_t)k_fmt_resv_f32 : (uint32_t)k_fmt_resv_f16;
}

/**
 * @brief Compute the cluster count for a trial cluster size.
 *
 * @details Mirrors the mount-side geometry: subtracts the reserved, FAT, and
 *          fixed-root regions from the device, then divides the remaining data
 *          sectors by @p spc. The FAT size is sized to index every data
 *          cluster (`entries_per_sector * spc + 2` accounts for the two
 *          reserved entries) and is reported back so the caller can store it.
 *
 * @param[in]  g   Geometry holding total/reserved/root spans and the type.
 * @param[in]  spc Trial sectors-per-cluster (power of two).
 * @param[out] out_fatsz Sectors per FAT copy for this @p spc.
 *
 * @return Resulting data-cluster count, or 0 if the device is too small.
 * @retval 0            Reserved + FAT + root already exceed the device.
 * @retval 1..UINT32_MAX Cluster count for @p spc.
 *
 * @pre @p g and @p out_fatsz are non-NULL.
 * @pre @p spc is non-zero.
 * @post `*out_fatsz` holds the FAT size for @p spc.
 * @post No other state modified.
 *
 * @note Pure aside from `*out_fatsz`; thread-safe vs other readers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_fmt_clusters_for(const ra8_fs_fmt_geom_t* g, uint32_t spc, uint32_t* out_fatsz)
{
  const uint32_t entry_cap = (g->type == k_ra8_fs_type_fat32) ? (uint32_t)k_fmt_fat32_entry_cap
                                                              : (uint32_t)k_fmt_fat16_entry_cap;
  const uint32_t overhead  = g->reserved_sectors + g->root_sectors;
  if (g->total_sectors <= overhead) {
    *out_fatsz = 0U;
    return 0U;
  }
  const uint32_t avail     = g->total_sectors - overhead;
  const uint32_t denom     = (entry_cap * spc) + (uint32_t)k_fmt_num_fats;
  const uint32_t fatsz     = (avail + denom - 1U) / denom;
  *out_fatsz               = fatsz;
  const uint32_t fat_total = (uint32_t)k_fmt_num_fats * fatsz;
  if (avail <= fat_total) {
    return 0U;
  }
  return (avail - fat_total) / spc;
}

/**
 * @brief Test whether a cluster count is valid for a given FAT type.
 *
 * @details Applies the MS FAT spec sec 3.5 thresholds: FAT12 needs
 *          `count < 4085`, FAT16 needs `4085 <= count < 65525`, FAT32 needs
 *          `65525 <= count <= k_fmt_fat32_clus_cap`.
 *
 * @param[in] type  Requested FAT variant.
 * @param[in] count Trial cluster count.
 *
 * @return Whether @p count lands in @p type's valid band.
 * @retval true  @p count is valid for @p type.
 * @retval false @p count is outside @p type's band.
 *
 * @pre @p type is FAT12/FAT16/FAT32.
 * @pre @p count was produced by `priv_fmt_clusters_for()`.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_fmt_count_in_band(ra8_fs_type_t type, uint32_t count)
{
  if (type == k_ra8_fs_type_fat12) {
    return count < (uint32_t)k_cluster_count_fat12_max;
  }
  if (type == k_ra8_fs_type_fat16) {
    return (count >= (uint32_t)k_cluster_count_fat12_max) &&
           (count < (uint32_t)k_cluster_count_fat16_max);
  }
  return (count >= (uint32_t)k_cluster_count_fat16_max) &&
         (count <= (uint32_t)k_fmt_fat32_clus_cap);
}

/**
 * @brief Default FAT32 cluster size for a device, per the Microsoft table.
 *
 * @details Implements the `DskSzToSecPerClus` tiers from the MS FAT spec
 *          (fatgen103 sec 3.3). Used as the *starting* cluster size for the
 *          FAT32 auto-sweep so a large card does not land on `spc=1` (which the
 *          cluster cap technically allows but which produces a multi-hundred-MB
 *          FAT that takes minutes to zero over SPI). The sweep can still grow
 *          `spc` further for cards above the table's range.
 *
 * @param[in] total_sectors Whole-device 512-byte sector count.
 *
 * @return Sectors-per-cluster (power of two in 1..`k_fmt_spc_max`).
 * @retval 1  Device is <= 260 MB.
 * @retval 64 Device is > 32 GB.
 *
 * @pre @p total_sectors is the card capacity in 512-byte blocks.
 * @pre @p total_sectors represents the actual device capacity, not a user hint.
 * @post Return value is a power of two in the range [1, k_fmt_spc_max].
 * @post No state is modified (pure function, no side effects).
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_fmt_fat32_default_spc(uint32_t total_sectors)
{
  if (total_sectors <= (uint32_t)k_fmt_f32_thr_260m) {
    return (uint32_t)k_fmt_f32_spc_512b;
  }
  if (total_sectors <= (uint32_t)k_fmt_f32_thr_8g) {
    return (uint32_t)k_fmt_f32_spc_4k;
  }
  if (total_sectors <= (uint32_t)k_fmt_f32_thr_16g) {
    return (uint32_t)k_fmt_f32_spc_8k;
  }
  if (total_sectors <= (uint32_t)k_fmt_f32_thr_32g) {
    return (uint32_t)k_fmt_f32_spc_16k;
  }
  return (uint32_t)k_fmt_f32_spc_32k;
}

/* `priv_fmt_choose_geometry()`: see header for the documented contract. */
ra8_err_t priv_fmt_choose_geometry(ra8_fs_fmt_geom_t* g, uint32_t spc_hint)
{
  bool     auto_mode = (spc_hint == 0U);
  uint32_t spc       = spc_hint;
  if (auto_mode) {
    /* FAT32 starts the sweep at the size-appropriate cluster (MS table) so a
     * big card gets a small FAT; FAT12/16 start at the minimum and grow. */
    spc = (g->type == k_ra8_fs_type_fat32) ? priv_fmt_fat32_default_spc(g->total_sectors) : 1U;
  }
  for (uint32_t guard = 0U; guard <= (uint32_t)k_fmt_spc_max; guard++) {
    uint32_t       fatsz = 0U;
    const uint32_t count = priv_fmt_clusters_for(g, spc, &fatsz);
    if (priv_fmt_count_in_band(g->type, count)) {
      g->sectors_per_cluster = spc;
      g->fat_size_sectors    = fatsz;
      g->count_of_clusters   = count;
      return k_ra8_ok;
    }
    if (!auto_mode || spc >= (uint32_t)k_fmt_spc_max) {
      return k_ra8_err_invalid_size;
    }
    spc *= 2U;
  }
  return k_ra8_err_invalid_size;
}

/**
 * @brief Write the shared boot prologue (jump + OEM) into the scratch sector.
 *
 * @details Lays the 3-byte short-jump opcode and the 8-byte OEM name at the
 *          head of the boot sector. The scratch buffer must be zeroed first.
 *
 * @param[out] sec Zeroed 512-byte boot sector under construction.
 *
 * @pre @p sec is non-NULL and at least 512 bytes, pre-zeroed.
 * @pre The caller owns @p sec for the duration.
 * @post Bytes 0..2 and 3..10 hold the jump + OEM fields.
 * @post No bytes past offset 10 are touched.
 *
 * @note Not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_fmt_boot_prologue(uint8_t* sec)
{
  static const uint8_t k_oem[k_filename_base_len] = {'M', 'S', 'D', 'O', 'S', '5', '.', '0'};
  sec[k_fmt_off_jmp0]                             = (uint8_t)k_fmt_jmp_byte0;
  sec[k_fmt_off_jmp1]                             = (uint8_t)k_fmt_jmp_byte1;
  sec[k_fmt_off_jmp2]                             = (uint8_t)k_fmt_jmp_byte2;
  priv_byte_copy(&sec[k_fmt_off_oem], k_oem, (uint32_t)k_filename_base_len);
}

/* `priv_fmt_label_field()`: see header for the documented contract. */
void priv_fmt_label_field(uint8_t* dst, const char* label)
{
  /* An unlabelled FAT volume stores the spec sentinel "NO NAME    ", never
   * zeros and never a bare run of spaces: fsck.fat reads a blank BS_VolLab as a
   * corrupt label and strips it on sight, which trains people to ignore fsck
   * output on every card this firmware writes (#634). A NULL or empty label
   * therefore resolves to that sentinel before padding. */
  static const char k_no_name[] = "NO NAME";
  const char*       eff         = ((label != nullptr) && (label[0] != '\0')) ? label : k_no_name;
  bool              past_end    = false;
  for (uint32_t i = 0U; i < (uint32_t)k_fmt_label_len; i++) {
    if (!past_end && (eff[i] == '\0')) {
      past_end = true;
    }
    dst[i] = past_end ? (uint8_t)' ' : (uint8_t)eff[i];
  }
}

/**
 * @brief Write a totals/FAT-size pair into the BPB, choosing 16- vs 32-bit.
 *
 * @details `BPB_TotSec16`/`BPB_FATSz16` hold values that fit 16 bits; larger
 *          values spill to `BPB_TotSec32`/`BPB_FATSz32` with the 16-bit field
 *          left zero (FAT12/16). FAT32 always uses the 32-bit fields. The
 *          caller passes the already-resolved type to drive that choice.
 *
 * @param[out] sec     Boot sector under construction.
 * @param[in]  g       Geometry with totals + FAT size.
 *
 * @pre @p sec and @p g are non-NULL.
 * @pre BPB common fields were already written.
 * @post The total-sectors and FAT-size BPB fields reflect @p g.
 * @post Exactly one width (16- or 32-bit) is non-zero per field.
 *
 * @note Not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_fmt_write_totals(uint8_t* sec, const ra8_fs_fmt_geom_t* g)
{
  if (g->type == k_ra8_fs_type_fat32) {
    priv_wr32(&sec[k_bpb_off_tot_sec_32], g->total_sectors);
    priv_wr32(&sec[k_bpb_off_fat_sz_32], g->fat_size_sectors);
    return;
  }
  if (g->total_sectors < (uint32_t)(k_word_mask + 1U)) {
    priv_wr16(&sec[k_bpb_off_tot_sec_16], (uint16_t)g->total_sectors);
  } else {
    priv_wr32(&sec[k_bpb_off_tot_sec_32], g->total_sectors);
  }
  priv_wr16(&sec[k_bpb_off_fat_sz_16], (uint16_t)g->fat_size_sectors);
}

/**
 * @brief Build the FAT12/FAT16 boot sector into the scratch buffer.
 *
 * @details Writes the full BPB: bytes-per-sector, the chosen cluster size,
 *          one reserved sector, two FATs, the 512-entry root directory, media
 *          descriptor, geometry, an extended boot signature with volume serial
 *          + label, the "FAT12   "/"FAT16   " type string, and the 0x55AA boot
 *          signature.
 *
 * @param[out] sec   Zeroed boot sector under construction.
 * @param[in]  g     Resolved geometry.
 * @param[in]  label Optional volume label.
 *
 * @pre @p sec, @p g are non-NULL; @p sec pre-zeroed.
 * @pre @p g->type is FAT12 or FAT16.
 * @post @p sec holds a complete FAT12/16 boot sector.
 * @post Byte 510/511 hold the 0x55/0xAA signature.
 *
 * @note Not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_fmt_build_bpb_f16(uint8_t* sec, const ra8_fs_fmt_geom_t* g, const char* label)
{
  static const uint8_t k_t12[k_filename_base_len] = {'F', 'A', 'T', '1', '2', ' ', ' ', ' '};
  static const uint8_t k_t16[k_filename_base_len] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};
  priv_fmt_boot_prologue(sec);
  priv_wr16(&sec[k_bpb_off_bytes_per_sec], (uint16_t)k_ra8_fs_bytes_per_sector);
  sec[k_bpb_off_sec_per_clus] = (uint8_t)g->sectors_per_cluster;
  priv_wr16(&sec[k_bpb_off_rsvd_sec_cnt], (uint16_t)g->reserved_sectors);
  sec[k_bpb_off_num_fats] = (uint8_t)k_fmt_num_fats;
  priv_wr16(&sec[k_bpb_off_root_ent_cnt], (uint16_t)g->root_entries);
  sec[k_fmt_off_media] = (uint8_t)k_fmt_media_fixed;
  priv_fmt_write_totals(sec, g);
  priv_wr16(&sec[k_fmt_off_sec_per_trk], (uint16_t)k_fmt_sec_per_trk);
  priv_wr16(&sec[k_fmt_off_num_heads], (uint16_t)k_fmt_num_heads);
  sec[k_fmt_off_f16_drvnum]  = (uint8_t)k_fmt_drvnum_hd;
  sec[k_fmt_off_f16_bootsig] = (uint8_t)k_fmt_ext_bootsig;
  priv_wr32(&sec[k_fmt_off_f16_volid], (uint32_t)k_fmt_volid_base | g->total_sectors);
  priv_fmt_label_field(&sec[k_fmt_off_f16_label], label);
  priv_byte_copy(&sec[k_fmt_off_f16_fstype],
                 (g->type == k_ra8_fs_type_fat12) ? k_t12 : k_t16,
                 (uint32_t)k_filename_base_len);
  sec[k_bpb_off_signature_lo] = (uint8_t)k_bpb_sig_lo;
  sec[k_bpb_off_signature_hi] = (uint8_t)k_bpb_sig_hi;
}

/**
 * @brief Build the FAT32 boot sector into the scratch buffer.
 *
 * @details Writes the FAT32 BPB: zero root entries, 32-bit totals + FAT size,
 *          root cluster 2, the FSInfo sector number, the backup-boot location,
 *          an extended boot signature with serial + label, the "FAT32   " type
 *          string, and the 0x55AA signature.
 *
 * @param[out] sec   Zeroed boot sector under construction.
 * @param[in]  g     Resolved geometry (FAT32).
 * @param[in]  label Optional volume label.
 *
 * @pre @p sec, @p g are non-NULL; @p sec pre-zeroed.
 * @pre @p g->type is FAT32.
 * @post @p sec holds a complete FAT32 boot sector.
 * @post Byte 510/511 hold the 0x55/0xAA signature.
 *
 * @note Not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_fmt_build_bpb_f32(uint8_t* sec, const ra8_fs_fmt_geom_t* g, const char* label)
{
  static const uint8_t k_t32[k_filename_base_len] = {'F', 'A', 'T', '3', '2', ' ', ' ', ' '};
  priv_fmt_boot_prologue(sec);
  priv_wr16(&sec[k_bpb_off_bytes_per_sec], (uint16_t)k_ra8_fs_bytes_per_sector);
  sec[k_bpb_off_sec_per_clus] = (uint8_t)g->sectors_per_cluster;
  priv_wr16(&sec[k_bpb_off_rsvd_sec_cnt], (uint16_t)g->reserved_sectors);
  sec[k_bpb_off_num_fats] = (uint8_t)k_fmt_num_fats;
  sec[k_fmt_off_media]    = (uint8_t)k_fmt_media_fixed;
  priv_fmt_write_totals(sec, g);
  priv_wr16(&sec[k_fmt_off_sec_per_trk], (uint16_t)k_fmt_sec_per_trk);
  priv_wr16(&sec[k_fmt_off_num_heads], (uint16_t)k_fmt_num_heads);
  priv_wr32(&sec[k_bpb_off_root_clus], (uint32_t)k_fmt_root_clus_f32);
  priv_wr16(&sec[k_fmt_off_f32_fsinfo], (uint16_t)k_fmt_fsinfo_sector);
  priv_wr16(&sec[k_fmt_off_f32_bkboot], (uint16_t)k_fmt_bkboot_sector);
  sec[k_fmt_off_f32_drvnum]  = (uint8_t)k_fmt_drvnum_hd;
  sec[k_fmt_off_f32_bootsig] = (uint8_t)k_fmt_ext_bootsig;
  priv_wr32(&sec[k_fmt_off_f32_volid], (uint32_t)k_fmt_volid_base | g->total_sectors);
  priv_fmt_label_field(&sec[k_fmt_off_f32_label], label);
  priv_byte_copy(&sec[k_fmt_off_f32_fstype], k_t32, (uint32_t)k_filename_base_len);
  sec[k_bpb_off_signature_lo] = (uint8_t)k_bpb_sig_lo;
  sec[k_bpb_off_signature_hi] = (uint8_t)k_bpb_sig_hi;
}

/** @brief All-zero source for the chunked FAT/root wipe (read-only, in flash). */
static const uint8_t
  k_fmt_zero_chunk[(uint32_t)k_fmt_zero_chunk_secs * (uint32_t)k_ra8_fs_bytes_per_sector] = {};

/**
 * @brief Zero a run of sectors on the backend in multi-sector chunks.
 *
 * @details Writes up to ::k_fmt_zero_chunk_secs sectors per `write_block` call
 *          from a read-only all-zero buffer, so an SD backend can clear the
 *          whole region with CMD25 multi-block writes instead of one slow CMD24
 *          single-block write per sector (a 32 MB FAT on a 128 GB card drops
 *          from minutes to seconds). Bounds the loop by the caller's count.
 *
 * @param[in] backend Block-device backend.
 * @param[in] lba     First sector to clear.
 * @param[in] count   Number of sectors to clear.
 *
 * @return Backend error code (first failure aborts).
 * @retval k_ra8_ok    All @p count sectors zeroed.
 * @retval k_ra8_err_* Backend write failure within the run.
 *
 * @pre @p backend and `backend->write_block` are non-NULL.
 * @pre @p lba + @p count does not exceed the device capacity.
 * @post On success, every byte in sectors @p lba .. @p lba+count-1 reads as zero.
 * @post On failure, sectors up to (but not including) the failing one may be zeroed.
 *
 * @note Bounded loop (NASA Rule 2): at most @p count iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fmt_zero_run(const ra8_fs_backend_t* backend, uint32_t lba, uint32_t count)
{
  uint32_t done = 0U;
  while (done < count) {
    uint32_t chunk = count - done;
    if (chunk > (uint32_t)k_fmt_zero_chunk_secs) {
      chunk = (uint32_t)k_fmt_zero_chunk_secs;
    }
    const ra8_err_t err = backend->write_block(backend->ctx, lba + done, chunk, k_fmt_zero_chunk);
    if (err != k_ra8_ok) {
      return err;
    }
    done += chunk;
  }
  return k_ra8_ok;
}

/* `priv_fmt_clear_region()`: see header for the documented contract. */
ra8_err_t priv_fmt_clear_region(const ra8_fs_backend_t* backend, uint32_t lba, uint32_t count)
{
  if ((backend->erase_blocks != nullptr) &&
      (backend->erase_blocks(backend->ctx, lba, count) == k_ra8_ok)) {
    return k_ra8_ok; /* erased and verified zero -- skip the zero-write */
  }
  /* No erase hook, or erase could not guarantee a zero read-back: write zeros. */
  return priv_fmt_zero_run(backend, lba, count);
}

/**
 * @brief Seed the reserved FAT entries (and FAT32 root-cluster EOC) per copy.
 *
 * @details Builds one zeroed FAT sector holding `FAT[0]` (media descriptor in
 *          the low byte, the rest 1-bits) and `FAT[1]` (a clean end-of-chain),
 *          plus -- on FAT32 -- `FAT[2]` marking the root cluster as a
 *          terminated single-cluster chain. The same first sector is written
 *          to the start of every FAT copy.
 *
 * @param[in] backend Block-device backend.
 * @param[in] g       Resolved geometry.
 *
 * @return Backend error code.
 * @retval k_ra8_ok    All FAT copies seeded.
 * @retval k_ra8_err_* Backend write failure.
 *
 * @pre @p backend, @p g are non-NULL.
 * @pre @p g->fat_size_sectors and `reserved_sectors` are set.
 * @post FAT[0]/FAT[1] (and FAT[2] on FAT32) are valid in every copy.
 * @post `s_scratch` holds the last FAT seed sector.
 *
 * @note Bounded loop (NASA Rule 2): `k_fmt_num_fats` iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fmt_seed_fats(const ra8_fs_backend_t* backend, const ra8_fs_fmt_geom_t* g)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_fs_bytes_per_sector; i++) {
    s_scratch[i] = 0U;
  }
  if (g->type == k_ra8_fs_type_fat32) {
    priv_wr32(&s_scratch[0], (uint32_t)k_cluster_eoc_min_fat32 | (uint32_t)k_fmt_media_fixed);
    priv_wr32(&s_scratch[4], (uint32_t)k_cluster_eoc_write_fat32);
    priv_wr32(&s_scratch[8], (uint32_t)k_cluster_eoc_write_fat32);
  } else if (g->type == k_ra8_fs_type_fat16) {
    priv_wr16(&s_scratch[0],
              (uint16_t)((uint32_t)k_cluster_eoc_min_fat16 | (uint32_t)k_fmt_media_fixed));
    priv_wr16(&s_scratch[2], (uint16_t)k_cluster_eoc_write_fat16);
  } else {
    /* FAT12: 1.5-byte entries. FAT[0]=0xF<media>, FAT[1]=0xFFF, packed LE. */
    s_scratch[0] = (uint8_t)k_fmt_media_fixed;
    s_scratch[1] = (uint8_t)k_byte_mask;
    s_scratch[2] = (uint8_t)k_byte_mask;
  }
  for (uint32_t f = 0U; f < (uint32_t)k_fmt_num_fats; f++) {
    const uint32_t  fat_lba = g->reserved_sectors + (f * g->fat_size_sectors);
    const ra8_err_t err     = backend->write_block(backend->ctx, fat_lba, 1U, s_scratch);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Write the FAT32 FSInfo sector (and the backup boot sector copy).
 *
 * @details Lays the FSInfo lead/struct/trail signatures and a best-effort free
 *          count + next-free hint, then copies the boot sector to the backup
 *          location. No-op for FAT12/16 (they have no FSInfo).
 *
 * @param[in] backend  Block-device backend.
 * @param[in] g        Resolved geometry (FAT32).
 * @param[in] boot_sec The freshly built boot sector (512 bytes) to back up.
 *
 * @return Backend error code.
 * @retval k_ra8_ok    FSInfo + backup written (or nothing for FAT12/16).
 * @retval k_ra8_err_* Backend write failure.
 *
 * @pre @p backend, @p g, @p boot_sec are non-NULL.
 * @pre @p g->type is FAT32 for any write to occur.
 * @post On FAT32, the FSInfo + backup-boot sectors are valid.
 * @post `s_scratch` is clobbered.
 *
 * @note Not reentrant against the module scratch buffer.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fmt_write_fsinfo(const ra8_fs_backend_t*  backend,
                                       const ra8_fs_fmt_geom_t* g,
                                       const uint8_t*           boot_sec)
{
  if (g->type != k_ra8_fs_type_fat32) {
    return k_ra8_ok;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_fs_bytes_per_sector; i++) {
    s_scratch[i] = 0U;
  }
  priv_wr32(&s_scratch[k_fmt_fsi_off_lead], (uint32_t)k_fmt_fsi_lead_sig);
  priv_wr32(&s_scratch[k_fmt_fsi_off_struct], (uint32_t)k_fmt_fsi_struct_sig);
  const uint32_t free_count =
    (g->count_of_clusters > 0U) ? (g->count_of_clusters - 1U) : (uint32_t)k_fmt_fsi_unknown;
  priv_wr32(&s_scratch[k_fmt_fsi_off_free], free_count);
  priv_wr32(&s_scratch[k_fmt_fsi_off_nxtfree], (uint32_t)k_fmt_fat32_nxt_free);
  priv_wr32(&s_scratch[k_fmt_fsi_off_trail], (uint32_t)k_fmt_fsi_trail_sig);
  ra8_err_t err = backend->write_block(backend->ctx, (uint32_t)k_fmt_fsinfo_sector, 1U, s_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  return backend->write_block(backend->ctx, (uint32_t)k_fmt_bkboot_sector, 1U, boot_sec);
}

/* `priv_fmt_spc_valid()`: see header for the documented contract. */
bool priv_fmt_spc_valid(uint8_t spc)
{
  if (spc == 0U) {
    return true;
  }
  if (spc > (uint32_t)k_fmt_spc_max) {
    return false;
  }
  return ((uint32_t)spc & ((uint32_t)spc - 1U)) == 0U;
}

/* `priv_fmt_emit_volume()`: see header for the documented contract. */
ra8_err_t
priv_fmt_emit_volume(const ra8_fs_backend_t* backend, const ra8_fs_fmt_geom_t* g, const char* label)
{
  uint8_t boot[k_ra8_fs_bytes_per_sector] = {};
  if (g->type == k_ra8_fs_type_fat32) {
    priv_fmt_build_bpb_f32(boot, g, label);
  } else {
    priv_fmt_build_bpb_f16(boot, g, label);
  }
  /* Clear the FAT region plus the (contiguous) root region first, so no stale
   * non-zero FAT word survives as an orphan cluster. Erase the whole span in
   * one go when the card guarantees zero-after-erase, else stream zeros. Doing
   * this BEFORE the metadata writes means an erase that rounds into the reserved
   * region cannot clobber the boot sector / FSInfo written below. */
  const uint32_t fat_total = (uint32_t)k_fmt_num_fats * g->fat_size_sectors;
  const uint32_t root_span =
    (g->type == k_ra8_fs_type_fat32) ? g->sectors_per_cluster : g->root_sectors;
  ra8_err_t err = priv_fmt_clear_region(backend, g->reserved_sectors, fat_total + root_span);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Now lay the metadata: boot sector, FAT seeds (rewrite FAT sector 0 of each
   * copy with the media + EOC reserved entries), then FAT32 FSInfo + backup. */
  err = backend->write_block(backend->ctx, 0U, 1U, boot);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_fmt_seed_fats(backend, g);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_fmt_write_fsinfo(backend, g, boot);
}
