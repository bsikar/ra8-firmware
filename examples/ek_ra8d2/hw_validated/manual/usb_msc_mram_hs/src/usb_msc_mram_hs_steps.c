/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_msc_mram_hs/src/usb_msc_mram_hs_steps.c
 * @brief Synthesized FAT16 volume helpers for the USB-HS MRAM MSC demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements the read-only FAT16 sector-synthesis routines split out of
 * ``main.c`` so every translation unit stays under the source-size cap.
 * The boot sector, FAT, and root directory are generated on the fly,
 * and the data clusters map 1:1 onto the chip's 1 MiB MRAM window at
 * 0x02000000, exposed as a single root file ``MRAM.BIN``. Only
 * ``demo_fat_fill_sector()`` is exported (called from the MSC media-read
 * callback in ``main.c``); the boot/FAT/root fillers and the
 * little-endian packers stay private to this sibling, alongside the
 * file-static FAT identifier strings and the FAT-specific layout enums.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_msc_mram_hs_steps.h"

#include <stdint.h>
#include <string.h>

#ifndef RA8_OFF_TARGET

/**
 * @enum demo_mram_t
 * @brief The MRAM window this volume exposes (RA8D2 HUM memory map).
 */
typedef enum : uint32_t {
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address. */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.             */
} demo_mram_t;

/**
 * @enum demo_fat_boot_t
 * @brief Boot-sector field values (MS FAT spec 1.03 sec 3.1).
 */
typedef enum : uint32_t {
  k_boot_jmp0        = 0xEBU,       /**< Short JMP opcode.           */
  k_boot_jmp1        = 0x3CU,       /**< JMP displacement.           */
  k_boot_jmp2        = 0x90U,       /**< NOP.                        */
  k_boot_media       = 0xF8U,       /**< Fixed-disk media byte.      */
  k_boot_sec_per_trk = 32U,         /**< Geometry filler.            */
  k_boot_num_heads   = 16U,         /**< Geometry filler.            */
  k_boot_drive_num   = 0x80U,       /**< BIOS drive number.          */
  k_boot_ext_sig     = 0x29U,       /**< Extended boot signature.    */
  k_boot_volume_id   = 0x52A8D200U, /**< Arbitrary volume serial.    */
  k_boot_sig_lo      = 0x55U,       /**< Boot signature low byte.    */
  k_boot_sig_hi      = 0xAAU,       /**< Boot signature high byte.   */
  k_boot_sig_lo_off  = 510U,        /**< Signature low-byte offset.  */
  k_boot_sig_hi_off  = 511U,        /**< Signature high-byte offset. */
} demo_fat_boot_t;

/**
 * @enum demo_fat_off_t
 * @brief Byte offsets inside the boot sector and directory entries.
 */
typedef enum : uint8_t {
  k_bpb_off_jmp        = 0U,    /**< Jump instruction.             */
  k_bpb_off_oem        = 3U,    /**< OEM name (8 bytes).           */
  k_bpb_off_bps        = 11U,   /**< Bytes per sector.             */
  k_bpb_off_spc        = 13U,   /**< Sectors per cluster.          */
  k_bpb_off_rsvd       = 14U,   /**< Reserved sector count.        */
  k_bpb_off_nfats      = 16U,   /**< Number of FATs.               */
  k_bpb_off_rootent    = 17U,   /**< Root entry count.             */
  k_bpb_off_totsec16   = 19U,   /**< Total sectors (16-bit).       */
  k_bpb_off_media      = 21U,   /**< Media descriptor.             */
  k_bpb_off_fatsz16    = 22U,   /**< Sectors per FAT.              */
  k_bpb_off_spt        = 24U,   /**< Sectors per track.            */
  k_bpb_off_heads      = 26U,   /**< Head count.                   */
  k_bpb_off_drvnum     = 36U,   /**< Drive number.                 */
  k_bpb_off_bootsig    = 38U,   /**< Extended boot signature.      */
  k_bpb_off_volid      = 39U,   /**< Volume serial (4 bytes).      */
  k_bpb_off_label      = 43U,   /**< Volume label (11 bytes).      */
  k_bpb_off_fstype     = 54U,   /**< Filesystem type (8 bytes).    */
  k_dir_entry_bytes    = 32U,   /**< Directory entry size.         */
  k_dir_off_attr       = 11U,   /**< Attribute byte.               */
  k_dir_off_cluster_lo = 26U,   /**< First cluster (low word).     */
  k_dir_off_size       = 28U,   /**< File size (32-bit LE).        */
  k_dir_attr_volume    = 0x08U, /**< Volume-label attribute.       */
  k_dir_attr_read_only = 0x01U, /**< Read-only attribute.          */
  k_dir_name_bytes     = 11U,   /**< 8.3 name field length.        */
  k_byte_shift         = 8U,    /**< Bits per byte for LE packing. */
  k_byte_mask          = 0xFFU, /**< Low-byte mask.                */
} demo_fat_off_t;

/**
 * @enum demo_word_pack_t
 * @brief 32-bit little-endian split constants.
 */
typedef enum : uint32_t {
  k_word_shift = 16U,     /**< Bits per half-word. */
  k_word_mask  = 0xFFFFU, /**< Low half-word mask. */
} demo_word_pack_t;

/** @brief Boot-sector OEM name (8 bytes, space padded). */
static const UCHAR s_fat_oem_name[8] = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};

/** @brief Volume label, 11 bytes space padded (also the root entry). */
static const UCHAR s_fat_volume_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};

/** @brief Filesystem-type tag, 8 bytes space padded. */
static const UCHAR s_fat_fs_type[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};

/** @brief 8.3 directory name of the exposed file: "MRAM.BIN". */
static const UCHAR s_fat_file_name[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/**
 * @brief Write a 16-bit value little-endian into a byte buffer.
 *
 * @param[out] dst   Destination (2 bytes).
 * @param[in]  value Value to store.
 *
 * @pre @p dst has 2 writable bytes.
 * @pre None beyond the buffer contract.
 * @post ``dst[0]`` holds the low byte, ``dst[1]`` the high byte.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void demo_put16(UCHAR* dst, uint16_t value)
{
  dst[0] = (UCHAR)(value & (uint16_t)k_byte_mask);
  dst[1] = (UCHAR)((value >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_mask);
}

/**
 * @brief Write a 32-bit value little-endian into a byte buffer.
 *
 * @param[out] dst   Destination (4 bytes).
 * @param[in]  value Value to store.
 *
 * @pre @p dst has 4 writable bytes.
 * @pre None beyond the buffer contract.
 * @post @p dst holds the four little-endian bytes of @p value.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void demo_put32(UCHAR* dst, uint32_t value)
{
  demo_put16(dst, (uint16_t)(value & (uint32_t)k_word_mask));
  demo_put16(dst + 2U, (uint16_t)(value >> (uint32_t)k_word_shift));
}

/**
 * @brief Synthesize the FAT16 boot sector (MS FAT spec 1.03 sec 3.1).
 *
 * @param[out] out Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre Geometry constants describe a valid FAT16 volume.
 * @post @p out holds the BPB + 0x55AA signature.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void demo_fat_fill_boot(UCHAR* out)
{
  out[k_bpb_off_jmp]      = (UCHAR)k_boot_jmp0;
  out[k_bpb_off_jmp + 1U] = (UCHAR)k_boot_jmp1;
  out[k_bpb_off_jmp + 2U] = (UCHAR)k_boot_jmp2;
  (void)memcpy(&out[k_bpb_off_oem], s_fat_oem_name, sizeof(s_fat_oem_name));
  demo_put16(&out[k_bpb_off_bps], (uint16_t)k_demo_block_size);
  out[k_bpb_off_spc] = 1U;
  demo_put16(&out[k_bpb_off_rsvd], (uint16_t)k_fat_reserved_sectors);
  out[k_bpb_off_nfats] = (UCHAR)k_fat_num_fats;
  demo_put16(&out[k_bpb_off_rootent], (uint16_t)k_fat_root_entries);
  demo_put16(&out[k_bpb_off_totsec16], (uint16_t)k_fat_total_sectors);
  out[k_bpb_off_media] = (UCHAR)k_boot_media;
  demo_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_fat_fat_sectors);
  demo_put16(&out[k_bpb_off_spt], (uint16_t)k_boot_sec_per_trk);
  demo_put16(&out[k_bpb_off_heads], (uint16_t)k_boot_num_heads);
  out[k_bpb_off_drvnum]  = (UCHAR)k_boot_drive_num;
  out[k_bpb_off_bootsig] = (UCHAR)k_boot_ext_sig;
  demo_put32(&out[k_bpb_off_volid], (uint32_t)k_boot_volume_id);
  (void)memcpy(&out[k_bpb_off_label], s_fat_volume_label, sizeof(s_fat_volume_label));
  (void)memcpy(&out[k_bpb_off_fstype], s_fat_fs_type, sizeof(s_fat_fs_type));
  out[k_boot_sig_lo_off] = (UCHAR)k_boot_sig_lo;
  out[k_boot_sig_hi_off] = (UCHAR)k_boot_sig_hi;
}

/**
 * @brief Synthesize one FAT16 sector of the cluster chain.
 *
 * @details MRAM.BIN occupies clusters 2..2049 as one sequential chain
 * (entry c -> c + 1, last entry -> end-of-chain). Entries 0/1 carry
 * the media descriptor per the FAT spec; everything past the chain
 * reads as free (0x0000).
 *
 * @param[in]  fat_sector Index of the FAT sector (0-based).
 * @param[out] out        Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre @p fat_sector is below ::k_fat_fat_sectors.
 * @post @p out holds 256 little-endian FAT16 entries.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void demo_fat_fill_fat(uint32_t fat_sector, UCHAR* out)
{
  const uint32_t first_entry = fat_sector * (uint32_t)k_fat_entries_per_sec;
  for (uint32_t j = 0U; j < (uint32_t)k_fat_entries_per_sec; j++) {
    const uint32_t entry = first_entry + j;
    uint16_t       value = 0U;
    if (entry == 0U) {
      value = (uint16_t)k_fat_entry0;
    } else if (entry == 1U) {
      value = (uint16_t)k_fat_eoc;
    } else if (entry < (uint32_t)k_fat_last_mram_clus) {
      value = (uint16_t)(entry + 1U);
    } else if (entry == (uint32_t)k_fat_last_mram_clus) {
      value = (uint16_t)k_fat_eoc;
    } else {
      value = 0U;
    }
    demo_put16(&out[j * 2U], value);
  }
}

/**
 * @brief Synthesize one root-directory sector.
 *
 * @details Sector 0 of the root carries two entries: the volume label
 * and the read-only ``MRAM.BIN`` file (start cluster 2, size 1 MiB).
 * Every other root sector is empty.
 *
 * @param[in]  root_sector Index of the root sector (0-based).
 * @param[out] out         Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre @p root_sector is below ::k_fat_root_sectors.
 * @post @p out holds the directory entries for that sector.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void demo_fat_fill_root(uint32_t root_sector, UCHAR* out)
{
  if (root_sector != 0U) {
    return;
  }
  /* Entry 0: volume label. */
  (void)memcpy(&out[0], s_fat_volume_label, (size_t)k_dir_name_bytes);
  out[k_dir_off_attr] = (UCHAR)k_dir_attr_volume;
  /* Entry 1: MRAM.BIN, read-only, cluster 2, 1 MiB. */
  UCHAR* entry = &out[k_dir_entry_bytes];
  (void)memcpy(entry, s_fat_file_name, (size_t)k_dir_name_bytes);
  entry[k_dir_off_attr] = (UCHAR)k_dir_attr_read_only;
  demo_put16(&entry[k_dir_off_cluster_lo], (uint16_t)k_fat_first_cluster);
  demo_put32(&entry[k_dir_off_size], (uint32_t)k_mram_bytes);
}

void demo_fat_fill_sector(uint32_t lba, UCHAR* out)
{
  (void)memset(out, 0, (size_t)k_demo_block_size);
  if (lba == 0U) {
    demo_fat_fill_boot(out);
    return;
  }
  if (lba < (uint32_t)k_fat_root_lba) {
    demo_fat_fill_fat(lba - (uint32_t)k_fat_fat_lba, out);
    return;
  }
  if (lba < (uint32_t)k_fat_data_lba) {
    demo_fat_fill_root(lba - (uint32_t)k_fat_root_lba, out);
    return;
  }
  const uint32_t cluster = (lba - (uint32_t)k_fat_data_lba) + (uint32_t)k_fat_first_cluster;
  if (cluster <= (uint32_t)k_fat_last_mram_clus) {
    const uint32_t offset = (cluster - (uint32_t)k_fat_first_cluster) * (uint32_t)k_demo_block_size;
    const UCHAR*   mram   = (const UCHAR*)(uintptr_t)((uint32_t)k_mram_base_addr + offset);
    (void)memcpy(out, mram, (size_t)k_demo_block_size);
  }
}

#endif /* !RA8_OFF_TARGET */
