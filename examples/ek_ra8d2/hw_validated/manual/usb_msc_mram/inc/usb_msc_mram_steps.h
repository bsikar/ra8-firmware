/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_msc_mram/inc/usb_msc_mram_steps.h
 * @brief Shared geometry constants + FAT16 sector-synthesis prototype.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the compile-time FAT16 volume-geometry / boot-sector /
 * offset enums plus the read-only sector-synthesizer that
 * ``main.c`` and ``usb_msc_mram_steps.c`` share. The enums live here
 * so the on-the-fly volume image stays a single source of truth: the
 * worker thread in ``main.c`` reads geometry constants such as
 * ::k_fat_total_sectors and ::k_demo_block_size to size the LUN, while
 * the synthesis helpers in ``usb_msc_mram_steps.c`` consume the same
 * byte-offset and field-value constants to build each 512-byte sector.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the worker thread + USBX pool +
 *        RAM-disk geometry.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 4096U,  /**< Worker thread stack (bytes).        */
  k_demo_usbx_pool_bytes = 32768U, /**< USBX memory pool (bytes).           */
  k_demo_block_size      = 512U,   /**< SCSI logical block size (bytes).    */
  k_demo_idle_ticks      = 50U,    /**< Heartbeat back-off (ThreadX ticks). */
} demo_config_t;

/**
 * @enum demo_mram_t
 * @brief The MRAM window this volume exposes (RA8D2 HUM memory map).
 */
typedef enum : uint32_t {
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address. */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.             */
} demo_mram_t;

/**
 * @enum demo_fat_geom_t
 * @brief Synthesized FAT16 volume geometry (MS FAT spec 1.03).
 *
 * @details One 512-byte sector per cluster. The data region is padded
 * to 4096 clusters so the cluster count crosses the 4085 FAT16
 * threshold; only the first 2048 clusters (1 MiB) are backed by MRAM,
 * and nothing references the rest. FAT16 needs 2 bytes per entry for
 * 4098 entries = 8196 bytes = 17 sectors. The root directory holds
 * 512 entries = 32 sectors.
 */
typedef enum : uint32_t {
  k_fat_reserved_sectors = 1U,      /**< Boot sector only.                  */
  k_fat_num_fats         = 1U,      /**< Single FAT copy.                   */
  k_fat_fat_sectors      = 17U,     /**< FAT16 size for 4098 entries.       */
  k_fat_root_entries     = 512U,    /**< Root directory entries.            */
  k_fat_root_sectors     = 32U,     /**< 512 entries x 32 B / 512 B.        */
  k_fat_data_sectors     = 4096U,   /**< Padded data region (>= 4085).      */
  k_fat_fat_lba          = 1U,      /**< First FAT sector.                  */
  k_fat_root_lba         = 18U,     /**< First root-directory sector.       */
  k_fat_data_lba         = 50U,     /**< First data sector (cluster 2).     */
  k_fat_total_sectors    = 4146U,   /**< 1 + 17 + 32 + 4096.                */
  k_fat_first_cluster    = 2U,      /**< FAT data area starts at cluster 2. */
  k_fat_mram_clusters    = 2048U,   /**< Clusters backed by MRAM (1 MiB).   */
  k_fat_last_mram_clus   = 2049U,   /**< Last cluster of MRAM.BIN.          */
  k_fat_entries_per_sec  = 256U,    /**< FAT16 entries per 512-byte sector. */
  k_fat_eoc              = 0xFFFFU, /**< End-of-chain marker.               */
  k_fat_entry0           = 0xFFF8U, /**< FAT[0]: media F8 + filler.         */
} demo_fat_geom_t;

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

#ifndef RA8_OFF_TARGET

#include "ux_api.h"

/**
 * @brief Synthesize one 512-byte sector of the read-only volume.
 *
 * @details Dispatches on the LBA: boot sector, FAT, root directory, or
 * data region. Data sectors inside the MRAM.BIN chain are copied
 * straight out of the 1 MiB MRAM window; padding clusters past the
 * chain read as zeros.
 *
 * @param[in]  lba Logical block address inside the volume.
 * @param[out] out 512-byte destination buffer.
 *
 * @pre @p lba is below ::k_fat_total_sectors (caller-checked).
 * @pre @p out has 512 writable bytes.
 * @post @p out holds the synthesized sector content.
 * @post No other state changes.
 *
 * @note Reads chip MRAM directly; no caching.
 * @since 0.1.0
 */
void demo_fat_fill_sector(uint32_t lba, UCHAR* out);

#endif /* !RA8_OFF_TARGET */
