/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_msc_browse/inc/usb_host_msc_browse_steps.h
 * @brief Shared constants + device-side FAT16/MSC step declarations
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion header for the `usb_host_msc_browse` self-loop app. It carries the
 * volume-geometry and field-offset enums that BOTH `main.c` and the device-side
 * step translation unit (`usb_host_msc_browse_steps.c`) reference, plus the
 * prototypes for the three USBX storage media callbacks that the step TU
 * defines but `main.c`'s class-register helper still wires into the LUN.
 *
 * The header is self-contained: it pulls in `<stdint.h>` for the typed-enum
 * underlying types and, only in the on-target firmware build, `ux_api.h`
 * for the USBX `UINT`/`VOID`/`UCHAR`/`ULONG` types used by the callback
 * signatures.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-25
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifndef RA8_OFF_TARGET
#include "ux_api.h"
#endif

/**
 * @enum selftest_config_t
 * @brief Compile-time settings: threads, pool, console, retry cadence.
 */
typedef enum : uint32_t {
  k_selftest_thread_stack    = 4096U,   /**< Device worker stack (bytes).     */
  k_selftest_host_stack      = 8192U,   /**< Host worker stack (bytes).       */
  k_selftest_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).        */
  k_selftest_block_size      = 512U,    /**< SCSI logical block size (bytes). */
  k_selftest_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).    */
  k_selftest_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).   */
  k_selftest_retry_ticks     = 5000U,   /**< Pause between ladder retries.    */
  k_selftest_baud            = 115200U, /**< J-Link OB CDC log baud.          */
  k_selftest_print_cap       = 160U,    /**< Bound for console-string scans.  */
  k_selftest_dev_priority    = 8U,      /**< Device bring-up worker priority. */
  k_selftest_host_priority   = 24U,     /**< Host worker: BELOW the USBX
                                         *   storage class thread
                                         *   (UX_THREAD_PRIORITY_CLASS = 20).
                                         *   The polled host loop busy-waits
                                         *   for device data; if it outranked
                                         *   the class thread, that thread
                                         *   could never run selftest_msc_read
                                         *   and every bulk read would time
                                         *   out (enumeration still works --
                                         *   device SETUP is ISR-driven).     */
} selftest_config_t;

/**
 * @enum selftest_mram_t
 * @brief The MRAM window the device-side volume exposes.
 */
typedef enum : uint32_t {
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address. */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.             */
} selftest_mram_t;

/**
 * @enum selftest_fat_geom_t
 * @brief Synthesized FAT16 volume geometry (MS FAT spec 1.03).
 *
 * @details Identical to `usb_msc_mram`: one 512-byte sector per
 * cluster, data region padded to 4096 clusters to cross the FAT16
 * threshold, MRAM.BIN occupying clusters 2..2049.
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
} selftest_fat_geom_t;

/**
 * @enum selftest_fat_off_t
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
} selftest_fat_off_t;

#ifndef RA8_OFF_TARGET

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
 *
 * @details Bound checks the request against the volume, then fills each
 * block via the FAT16 synthesizer. LED1 toggles per call so the
 * self-loop traffic is visible on the board.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (must be 0).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the volume; otherwise
 *         ``UX_ERROR`` with media_status set to ILLEGAL REQUEST.
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` and ``media_status`` are non-NULL (USBX
 *      guarantee).
 * @pre ``lun`` is 0 (single-LUN device).
 * @post Either ``number_blocks * 512`` bytes were synthesized or
 *       ``media_status`` is non-zero.
 * @post The device-side read-call probe advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
UINT selftest_msc_read(VOID*  storage,
                       ULONG  lun,
                       UCHAR* data_pointer,
                       ULONG  number_blocks,
                       ULONG  lba,
                       ULONG* media_status);

/**
 * @brief Storage media-write callback: always rejects (write-protected).
 *
 * @details The host side of this very app probes exactly this rejection
 * (WRITE(10) must fail with DATA PROTECT and the transport must keep
 * working afterwards).
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     data_pointer USBX-owned source buffer (unused).
 * @param[in]     number_blocks Number of blocks the host tried (unused).
 * @param[in]     lba          Starting LBA (unused).
 * @param[out]    media_status Filled with DATA PROTECT sense.
 *
 * @return Always ``UX_ERROR``.
 * @retval UX_ERROR The medium is write-protected.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The LUN also reports write-protected via MODE SENSE.
 * @post ``*media_status`` carries the DATA PROTECT sense triple.
 * @post The MRAM window is untouched.
 *
 * @note Hosts honouring the MODE SENSE WP bit never call this.
 * @since 0.1.0
 */
UINT selftest_msc_write(VOID*  storage,
                        ULONG  lun,
                        UCHAR* data_pointer,
                        ULONG  number_blocks,
                        ULONG  lba,
                        ULONG* media_status);

/**
 * @brief Storage media-status callback. Always reports media-present.
 *
 * @details The synthesized volume cannot go away; status is constant 0.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     media_id     Media id (unused).
 * @param[out]    media_status Filled with 0 (no fault).
 *
 * @return Always ``UX_SUCCESS``.
 * @retval UX_SUCCESS Media is present and ready.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The class instance is live.
 * @post ``*media_status`` is 0.
 * @post No other state changes.
 *
 * @note Synthesized volume; never reports media-not-present.
 * @since 0.1.0
 */
UINT selftest_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status);

/**
 * @brief One full host-side pass: enumerate, mount, browse, verify, WP.
 *
 * @details Drives the host-side ladder over the self-loop cable: brings the
 * USBHS host up and enumerates the FS device, mounts and parses the FAT16
 * volume, browses the root directory, raw-verifies the 1 MiB MRAM data region
 * with multi-block READ(10) bursts, then probes the write-protect rejection.
 * Phases are mirrored into the J-Link progress probe for readout. On any
 * failure the host controller is closed so the next retry starts from a clean
 * attach. The host worker thread (in `main.c`) loops this until it returns
 * k_ra8_ok.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed the BROWSE PASS banner.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success the pass counter advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_host_pass(void);

#endif /* !RA8_OFF_TARGET */
