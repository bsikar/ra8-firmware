/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_sd_format.c
 * @brief Blank-card FAT16/FAT32 in-memory formatters (--sd-new)
 *
 * @details
 * The pure image-buffer formatters board_sd_attach_blank uses to build an
 * empty FAT volume in memory -- moved verbatim out of board_periph_sd.c. No
 * peripheral block is registered here (registration is constructor-based and
 * stays with the card model), so this helper TU rides the board_periph_*.c
 * build glob with no CMake edit.
 *
 *
 * @since 0.1.0
 */

#include <string.h>

#include "board_periph_sd_internal.h"

/* ----------------------------------------------------------------------------
 * Blank-card creation (--sd-new): format an empty FAT volume in memory so a
 * size/format can be set up with no pre-built image. The BPB is complete enough
 * to satisfy a host `fsck_msdos` and the firmware's ra8_fs mount alike.
 * --------------------------------------------------------------------------*/

/** @brief Little-endian byte offsets, shifts, and mask for image serialization. */
typedef enum : uint8_t {
  k_le_byte0     = 0U,    /**< Least-significant byte offset. */
  k_le_byte1     = 1U,    /**< Second byte offset.            */
  k_le_byte2     = 2U,    /**< Third byte offset.             */
  k_le_byte3     = 3U,    /**< Most-significant byte offset.  */
  k_le_shift_b1  = 8U,    /**< Shift to extract byte 1.       */
  k_le_shift_b2  = 16U,   /**< Shift to extract byte 2.       */
  k_le_shift_b3  = 24U,   /**< Shift to extract byte 3.       */
  k_le_byte_mask = 0xFFU, /**< Low-byte mask.                 */
} sd_le_const_t;

/** @brief Little-endian 16-bit store into the image. */
static void sd_put16(uint8_t* p, uint16_t v)
{
  p[k_le_byte0] = (uint8_t)(v & (uint16_t)k_le_byte_mask);
  p[k_le_byte1] = (uint8_t)((v >> (uint16_t)k_le_shift_b1) & (uint16_t)k_le_byte_mask);
}

/** @brief Little-endian 32-bit store into the image. */
static void sd_put32(uint8_t* p, uint32_t v)
{
  p[k_le_byte0] = (uint8_t)(v & (uint32_t)k_le_byte_mask);
  p[k_le_byte1] = (uint8_t)((v >> (uint32_t)k_le_shift_b1) & (uint32_t)k_le_byte_mask);
  p[k_le_byte2] = (uint8_t)((v >> (uint32_t)k_le_shift_b2) & (uint32_t)k_le_byte_mask);
  p[k_le_byte3] = (uint8_t)((v >> (uint32_t)k_le_shift_b3) & (uint32_t)k_le_byte_mask);
}

/** @brief FAT BPB byte offsets within a sector (FAT16 + FAT32 layouts). */
typedef enum : uint16_t {
  k_bpb_jmp0          = 0U,   /**< Jump instruction byte 0.        */
  k_bpb_jmp1          = 1U,   /**< Jump instruction byte 1.        */
  k_bpb_jmp2          = 2U,   /**< Jump instruction byte 2.        */
  k_bpb_oem           = 3U,   /**< OEM name (8 bytes).             */
  k_bpb_bytes_per_sec = 11U,  /**< BPB_BytsPerSec.                 */
  k_bpb_sec_per_clus  = 13U,  /**< BPB_SecPerClus.                 */
  k_bpb_resv_sec      = 14U,  /**< BPB_RsvdSecCnt.                 */
  k_bpb_num_fats      = 16U,  /**< BPB_NumFATs.                    */
  k_bpb_root_ents     = 17U,  /**< BPB_RootEntCnt.                 */
  k_bpb_totsec16      = 19U,  /**< BPB_TotSec16.                   */
  k_bpb_media         = 21U,  /**< BPB_Media.                      */
  k_bpb_fatsz16       = 22U,  /**< BPB_FATSz16.                    */
  k_bpb_sec_per_trk   = 24U,  /**< BPB_SecPerTrk.                  */
  k_bpb_num_heads     = 26U,  /**< BPB_NumHeads.                   */
  k_bpb_totsec32      = 32U,  /**< BPB_TotSec32.                   */
  k_bpb_fatsz32       = 36U,  /**< BPB_FATSz32 (FAT32 only).       */
  k_bpb16_drive_num   = 36U,  /**< BS_DrvNum (FAT16).              */
  k_bpb16_boot_sig    = 38U,  /**< BS_BootSig (FAT16).             */
  k_bpb16_volid       = 39U,  /**< BS_VolID (FAT16).               */
  k_bpb16_vol_label   = 43U,  /**< BS_VolLab (FAT16, 11 bytes).    */
  k_bpb32_root_clus   = 44U,  /**< BPB_RootClus (FAT32).           */
  k_bpb32_fsinfo_sec  = 48U,  /**< BPB_FSInfo (FAT32).             */
  k_bpb32_bkboot_sec  = 50U,  /**< BPB_BkBootSec (FAT32).          */
  k_bpb16_fstype      = 54U,  /**< BS_FilSysType (FAT16, 8 bytes). */
  k_bpb32_drive_num   = 64U,  /**< BS_DrvNum (FAT32).              */
  k_bpb32_boot_sig    = 66U,  /**< BS_BootSig (FAT32).             */
  k_bpb32_volid       = 67U,  /**< BS_VolID (FAT32).               */
  k_bpb32_vol_label   = 71U,  /**< BS_VolLab (FAT32, 11 bytes).    */
  k_bpb32_fstype      = 82U,  /**< BS_FilSysType (FAT32, 8 bytes). */
  k_bpb_sig_lo        = 510U, /**< 0x55 boot-signature byte.       */
  k_bpb_sig_hi        = 511U, /**< 0xAA boot-signature byte.       */
} sd_bpb_off_t;

/** @brief FAT BPB literal field values + the FSInfo signatures. */
typedef enum : uint32_t {
  k_bpb_jmp_b0          = 0xEBU,       /**< Boot jump byte 0 (JMP short).       */
  k_bpb_jmp_b1          = 0x58U,       /**< Boot jump byte 1 (offset).          */
  k_bpb_jmp_b2          = 0x90U,       /**< Boot jump byte 2 (NOP).             */
  k_bpb_oem_len         = 8U,          /**< OEM-name field length.              */
  k_bpb_fstype_len      = 8U,          /**< FilSysType field length.            */
  k_bpb_sig_byte_lo     = 0x55U,       /**< Boot signature low byte.            */
  k_bpb_sig_byte_hi     = 0xAAU,       /**< Boot signature high byte.           */
  k_bpb_num_fats_val    = 2U,          /**< Number of FAT copies.               */
  k_bpb_media_fixed     = 0xF8U,       /**< Media descriptor: fixed disk.       */
  k_bpb_sec_per_trk_val = 63U,         /**< Sectors per track.                  */
  k_bpb_num_heads_val   = 255U,        /**< Number of heads.                    */
  k_bpb_drive_num_val   = 0x80U,       /**< Drive number (hard disk).           */
  k_bpb_ext_boot_sig    = 0x29U,       /**< Extended boot signature present.    */
  k_bpb_dir_ent_size    = 32U,         /**< Bytes per directory entry.          */
  k_bpb_fat16_div       = 256U,        /**< 512-byte sector / 2-byte FAT16 ent. */
  k_bpb_fat32_div       = 128U,        /**< FAT32 FAT-size divisor multiplier.  */
  k_fat16_eoc_clus0     = 0xFFF8U,     /**< FAT16 entry 0: media + EOC bits.    */
  k_fat16_eoc_clus1     = 0xFFFFU,     /**< FAT16 entry 1: end-of-chain.        */
  k_fat32_eoc_media     = 0x0FFFFFF8U, /**< FAT32 entry 0: media + EOC.         */
  k_fat32_eoc           = 0x0FFFFFFFU, /**< FAT32 end-of-chain marker.          */
  k_fat32_root_clus     = 2U,          /**< FAT32 root directory cluster.       */
  k_fat32_fsinfo_sec    = 1U,          /**< FSInfo sector index.                */
  k_fat32_bkboot_sec    = 6U,          /**< Backup boot-sector index.           */
  k_fsi_lead_sig        = 0x41615252U, /**< FSInfo lead signature "RRaA".       */
  k_fsi_struc_sig       = 0x61417272U, /**< FSInfo struct signature "rrAa".     */
  k_fsi_trail_sig       = 0xAA550000U, /**< FSInfo trailing signature.          */
  k_fsi_off_lead        = 0U,          /**< FSInfo lead-sig offset.             */
  k_fsi_off_struc       = 484U,        /**< FSInfo struct-sig offset.           */
  k_fsi_off_free        = 488U,        /**< FSInfo free-cluster-count off.      */
  k_fsi_off_nxtfree     = 492U,        /**< FSInfo next-free-cluster off.       */
  k_fsi_off_trail       = 508U,        /**< FSInfo trailing-sig offset.         */
  k_fsi_free_unknown    = 0xFFFFFFFFU, /**< Free-cluster count unknown.         */
  k_fsi_nxt_free        = 3U,          /**< Next-free cluster hint.             */
  k_fat_ent1_off        = 2U,          /**< FAT16 entry-1 byte offset.          */
  k_fat32_ent1_off      = 4U,          /**< FAT32 entry-1 byte offset.          */
  k_fat32_ent2_off      = 8U,          /**< FAT32 entry-2 byte offset.          */
} sd_bpb_val_t;

/** @brief Write the shared boot prologue (jump + OEM) and the 0x55AA signature. */
static void sd_boot_frame(uint8_t* img, const char* oem)
{
  img[k_bpb_jmp0] = (uint8_t)k_bpb_jmp_b0;
  img[k_bpb_jmp1] = (uint8_t)k_bpb_jmp_b1;
  img[k_bpb_jmp2] = (uint8_t)k_bpb_jmp_b2;
  (void)memcpy(&img[k_bpb_oem], oem, (size_t)k_bpb_oem_len);
  img[k_bpb_sig_lo] = (uint8_t)k_bpb_sig_byte_lo;
  img[k_bpb_sig_hi] = (uint8_t)k_bpb_sig_byte_hi;
}

/** @brief Pad an ASCII volume label to the 11-byte 8.3 field (space-filled). */
void sd_label_field(uint8_t* dst, const char* label)
{
  bool past_end = (label == nullptr);
  for (uint32_t i = 0U; i < (uint32_t)k_fmt_label_len; i++) {
    if (!past_end && (label[i] == '\0')) {
      past_end = true; /* stop reading at the NUL; pad the rest with spaces. */
    }
    dst[i] = past_end ? (uint8_t)' ' : (uint8_t)label[i];
  }
}

/* @brief Format an empty FAT16 volume; returns the chosen sectors-per-cluster. */
/**
 * @brief Solve the FAT16 cluster size and FAT length for @p total_sectors.
 *
 * @details
 * Doubles sectors-per-cluster until the cluster count fits under the FAT16
 * ceiling, recomputing the FAT length each round because the two depend on
 * each other. Stops at @ref k_fmt_spc_max, the largest cluster the format
 * allows.
 *
 * @param[in]  total_sectors Card size in 512-byte sectors.
 * @param[in]  root_sectors  Sectors the fixed root directory occupies.
 * @param[out] fatsz         Receives the per-copy FAT length in sectors.
 *
 * @return Sectors per cluster.
 *
 * @pre @p fatsz is non-NULL.
 * @pre @p total_sectors leaves room for the reserved and root regions.
 * @post The returned cluster size is a power of two, at most k_fmt_spc_max.
 * @post `*fatsz` covers every cluster the returned size yields.
 *
 * @note Thread-safe: pure arithmetic over its arguments.
 */
static uint32_t fat16_solve_geometry(uint32_t total_sectors, uint32_t root_sectors, uint32_t* fatsz)
{
  uint32_t spc = 1U;
  for (;;) {
    const uint32_t tmp1 = total_sectors - ((uint32_t)k_fmt_resv_f16 + root_sectors);
    const uint32_t tmp2 = ((uint32_t)k_bpb_fat16_div * spc) +
                          (uint32_t)k_bpb_num_fats_val; /* 256 = 512 / 2-byte ent. */
    *fatsz              = (tmp1 + tmp2 - 1U) / tmp2;
    const uint32_t data = total_sectors - (uint32_t)k_fmt_resv_f16 - (2U * *fatsz) - root_sectors;
    const uint32_t clus = data / spc;
    if (clus <= (uint32_t)k_fmt_fat16_max) {
      break;
    }
    spc *= 2U;
    if (spc >= (uint32_t)k_fmt_spc_max) {
      spc = (uint32_t)k_fmt_spc_max;
      break;
    }
  }
  return spc;
}

/**
 * @brief Write the FAT16 BIOS Parameter Block into sector 0 of @p img.
 *
 * @param[out] img           Card image.
 * @param[in]  total_sectors Card size in sectors.
 * @param[in]  spc           Sectors per cluster.
 * @param[in]  fatsz         Per-copy FAT length in sectors.
 * @param[in]  label         Volume label for the boot-sector field.
 *
 * @pre @p img covers at least one sector.
 * @pre @p spc and @p fatsz came from fat16_solve_geometry().
 * @post The sector count lands in the 16- or 32-bit field as its size requires.
 * @post The label and "FAT16   " type string are space-padded to width.
 *
 * @note Not thread-safe with respect to @p img.
 */
static void fat16_write_bpb(uint8_t*    img,
                            uint32_t    total_sectors,
                            uint32_t    spc,
                            uint32_t    fatsz,
                            const char* label)
{
  sd_boot_frame(img, "MSDOS5.0");
  sd_put16(&img[k_bpb_bytes_per_sec], (uint16_t)k_fmt_sec_bytes);
  img[k_bpb_sec_per_clus] = (uint8_t)spc;
  sd_put16(&img[k_bpb_resv_sec], (uint16_t)k_fmt_resv_f16);
  img[k_bpb_num_fats] = (uint8_t)k_bpb_num_fats_val;
  sd_put16(&img[k_bpb_root_ents], (uint16_t)k_fmt_root_ents);
  if (total_sectors < (uint32_t)k_fmt_totsec16_max) {
    sd_put16(&img[k_bpb_totsec16], (uint16_t)total_sectors);
  } else {
    sd_put32(&img[k_bpb_totsec32], total_sectors);
  }
  img[k_bpb_media] = (uint8_t)k_bpb_media_fixed;
  sd_put16(&img[k_bpb_fatsz16], (uint16_t)fatsz);
  sd_put16(&img[k_bpb_sec_per_trk], (uint16_t)k_bpb_sec_per_trk_val);
  sd_put16(&img[k_bpb_num_heads], (uint16_t)k_bpb_num_heads_val);
  img[k_bpb16_drive_num] = (uint8_t)k_bpb_drive_num_val;
  img[k_bpb16_boot_sig]  = (uint8_t)k_bpb_ext_boot_sig;
  sd_put32(&img[k_bpb16_volid], (uint32_t)k_fmt_volid | total_sectors);
  sd_label_field(&img[k_bpb16_vol_label], label);
  (void)memcpy(&img[k_bpb16_fstype], "FAT16   ", (size_t)k_bpb_fstype_len);
}

uint32_t sd_format_fat16(uint8_t* img, uint32_t total_sectors, const char* label)
{
  const uint32_t root_sectors = (((uint32_t)k_fmt_root_ents * (uint32_t)k_bpb_dir_ent_size) +
                                 ((uint32_t)k_fmt_sec_bytes - 1U)) /
                                (uint32_t)k_fmt_sec_bytes;
  uint32_t       fatsz        = 1U;
  const uint32_t spc          = fat16_solve_geometry(total_sectors, root_sectors, &fatsz);

  fat16_write_bpb(img, total_sectors, spc, fatsz, label);

  /* FAT[0] media + FAT[1] EOC, in both FAT copies. */
  for (uint32_t f = 0U; f < (uint32_t)k_bpb_num_fats_val; f++) {
    uint8_t* fat =
      &img[(size_t)((uint32_t)k_fmt_resv_f16 + (f * fatsz)) * (uint32_t)k_fmt_sec_bytes];
    sd_put16(&fat[k_le_byte0], (uint16_t)k_fat16_eoc_clus0);
    sd_put16(&fat[k_fat_ent1_off], (uint16_t)k_fat16_eoc_clus1);
  }
  return spc;
}

/**
 * @brief Compute FAT32 volume geometry for a card of `total_sectors`.
 *
 * @details
 * Iteratively grows the sectors-per-cluster (spc) so the FAT stays bounded and
 * the cluster size is realistic (8 KiB at ~2 GiB, 32 KiB at ~8 GiB+), capped at
 * the FAT32 cluster ceiling `k_fmt_spc_max`; `k_fmt_fat32_pref` keeps small
 * cards at 1 spc. For each candidate spc it derives the FAT size (in sectors)
 * using the Microsoft FAT32 divisor `((256 * spc) + NumFATs) / 2 = 128 * spc + 1`
 * and the resulting cluster count. Extracted verbatim from `sd_format_fat32()`.
 *
 * @param[in]  total_sectors Card size in 512-byte sectors.
 * @param[out] out_fatsz     Receives the chosen FAT size in sectors.
 * @param[out] out_clusters  Receives the resulting data-cluster count.
 * @return The chosen sectors-per-cluster value.
 * @retval spc Sectors-per-cluster (power of two, 1 .. k_fmt_spc_max).
 * @pre `out_fatsz` and `out_clusters` are non-null.
 * @pre `total_sectors` exceeds the FAT32 reserved-sector count.
 * @post `*out_fatsz` and `*out_clusters` hold the geometry for the return value.
 * @post The returned spc is a power of two not exceeding k_fmt_spc_max.
 * @note Pure computation; touches no image buffer. Not thread-safe by contract
 *       (operates only on caller-provided locals).
 * @since 0.1.0
 */
static uint32_t
sd_fat32_geometry(uint32_t total_sectors, uint32_t* out_fatsz, uint32_t* out_clusters)
{
  uint32_t spc      = 1U;
  uint32_t fatsz    = 1U;
  uint32_t clusters = 0U;
  for (;;) {
    const uint32_t tmp1 = total_sectors - (uint32_t)k_fmt_resv_f32;
    /* Microsoft FAT32 FAT-size divisor: ((256*spc) + NumFATs) / 2 = 128*spc + 1.
     * (Using +2 can undersize the FAT by a sector on large volumes.) */
    const uint32_t tmp2 = ((uint32_t)k_bpb_fat32_div * spc) + 1U;
    fatsz               = (tmp1 + tmp2 - 1U) / tmp2;
    const uint32_t data = total_sectors - (uint32_t)k_fmt_resv_f32 - (2U * fatsz);
    clusters            = data / spc;
    /* Grow the cluster size for big cards so the FAT stays bounded and the
     * cluster size is realistic (8 KiB at ~2 GiB, 32 KiB at ~8 GiB+), capped at
     * the FAT32 cluster ceiling. k_fmt_fat32_pref keeps small cards at 1 spc. */
    if (clusters <= (uint32_t)k_fmt_fat32_pref) {
      break;
    }
    spc *= 2U;
    if (spc >= (uint32_t)k_fmt_spc_max) {
      spc = (uint32_t)k_fmt_spc_max;
      break;
    }
  }
  *out_fatsz    = fatsz;
  *out_clusters = clusters;
  return spc;
}

/** @brief Format an empty FAT32 volume; returns the chosen sectors-per-cluster. */
uint32_t sd_format_fat32(uint8_t* img, uint32_t total_sectors, const char* label)
{
  uint32_t       fatsz    = 0U;
  uint32_t       clusters = 0U;
  const uint32_t spc      = sd_fat32_geometry(total_sectors, &fatsz, &clusters);
  sd_boot_frame(img, "MSDOS5.0");
  sd_put16(&img[k_bpb_bytes_per_sec], (uint16_t)k_fmt_sec_bytes);
  img[k_bpb_sec_per_clus] = (uint8_t)spc;
  sd_put16(&img[k_bpb_resv_sec], (uint16_t)k_fmt_resv_f32);
  img[k_bpb_num_fats] = (uint8_t)k_bpb_num_fats_val;
  sd_put16(&img[k_bpb_root_ents], 0U); /* root entries: 0 for FAT32 */
  sd_put16(&img[k_bpb_totsec16], 0U);  /* totsec16: 0, use totsec32 */
  img[k_bpb_media] = (uint8_t)k_bpb_media_fixed;
  sd_put16(&img[k_bpb_fatsz16], 0U); /* fatsz16: 0 for FAT32 */
  sd_put16(&img[k_bpb_sec_per_trk], (uint16_t)k_bpb_sec_per_trk_val);
  sd_put16(&img[k_bpb_num_heads], (uint16_t)k_bpb_num_heads_val);
  sd_put32(&img[k_bpb_totsec32], total_sectors);
  sd_put32(&img[k_bpb_fatsz32], fatsz); /* BPB_FATSz32 */
  sd_put32(&img[k_bpb32_root_clus], (uint32_t)k_fat32_root_clus);
  sd_put16(&img[k_bpb32_fsinfo_sec], (uint16_t)k_fat32_fsinfo_sec);
  sd_put16(&img[k_bpb32_bkboot_sec], (uint16_t)k_fat32_bkboot_sec);
  img[k_bpb32_drive_num] = (uint8_t)k_bpb_drive_num_val;
  img[k_bpb32_boot_sig]  = (uint8_t)k_bpb_ext_boot_sig;
  sd_put32(&img[k_bpb32_volid], (uint32_t)k_fmt_volid | total_sectors);
  sd_label_field(&img[k_bpb32_vol_label], label);
  (void)memcpy(&img[k_bpb32_fstype], "FAT32   ", (size_t)k_bpb_fstype_len);
  /* FSInfo sector (lead + struct + trail signatures). */
  uint8_t* fsi = &img[(size_t)(uint32_t)k_fat32_fsinfo_sec * (uint32_t)k_fmt_sec_bytes];
  sd_put32(&fsi[k_fsi_off_lead], (uint32_t)k_fsi_lead_sig);
  sd_put32(&fsi[k_fsi_off_struc], (uint32_t)k_fsi_struc_sig);
  sd_put32(&fsi[k_fsi_off_free],
           (clusters > 0U) ? (clusters - 1U) : (uint32_t)k_fsi_free_unknown); /* free clusters    */
  sd_put32(&fsi[k_fsi_off_nxtfree], (uint32_t)k_fsi_nxt_free);                /* nxt free cluster */
  sd_put32(&fsi[k_fsi_off_trail], (uint32_t)k_fsi_trail_sig);
  /* Backup boot copy at sector 6. */
  (void)memcpy(&img[(size_t)(uint32_t)k_fat32_bkboot_sec * (uint32_t)k_fmt_sec_bytes],
               img,
               (size_t)k_fmt_sec_bytes);
  /* FAT[0..2]: media + EOC + root-cluster EOC, in both FAT copies. */
  for (uint32_t f = 0U; f < (uint32_t)k_bpb_num_fats_val; f++) {
    uint8_t* fat =
      &img[(size_t)((uint32_t)k_fmt_resv_f32 + (f * fatsz)) * (uint32_t)k_fmt_sec_bytes];
    sd_put32(&fat[k_le_byte0], (uint32_t)k_fat32_eoc_media);
    sd_put32(&fat[k_fat32_ent1_off], (uint32_t)k_fat32_eoc);
    sd_put32(&fat[k_fat32_ent2_off], (uint32_t)k_fat32_eoc);
  }
  return spc;
}
