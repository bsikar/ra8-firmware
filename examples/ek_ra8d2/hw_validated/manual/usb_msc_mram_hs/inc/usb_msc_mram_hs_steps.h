/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_msc_mram_hs/inc/usb_msc_mram_hs_steps.h
 * @brief Synthesized FAT16 volume helpers for the USB-HS MRAM MSC demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the read-only FAT16 sector-synthesis API shared between the
 * application entry (``main.c``) and its implementation sibling
 * (``usb_msc_mram_hs_steps.c``). The volume's boot sector, FAT, and
 * root directory are generated on the fly; data clusters map 1:1 onto
 * the chip's 1 MiB MRAM window at 0x02000000, exposed as a single root
 * file ``MRAM.BIN``. Only ``demo_fat_fill_sector()`` is called from the
 * MSC media-read callback in ``main.c``; the lower-level fillers stay
 * private to the sibling. The shared geometry/config enums live here so
 * both translation units agree on the volume layout and block size.
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

#ifndef RA8_OFF_TARGET

#include "tx_api.h"
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
