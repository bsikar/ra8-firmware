/**
 * @file
 * examples/ek_ra8d2/hw_validated/hil/usb_selftest_fs_host/inc/usb_selftest_fs_host_steps.h
 * @brief Shared contract for the USB self-loop config B (FS host) example.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Splits the oversized `main.c` into three translation units while keeping
 * the firmware behaviour byte-for-byte identical:
 *
 *  - `main.c` -- boot, pins, SysTick, ThreadX `tx_application_define`, and
 *    the synthesized-disk class-registration glue.
 *  - `usb_selftest_fs_host_fatdisk.c` -- the DEVICE role: the synthesized
 *    read-only FAT16 volume over the 1 MiB MRAM window, the USBX storage
 *    media callbacks, the device-stack bring-up, and the device worker.
 *  - `usb_selftest_fs_host_host.c` -- the HOST role: the SCI8 console
 *    formatters, the ra8_io USB-MSC block device the `ra8_fs` mount runs
 *    on, the enumerate / mount / verify / write-protect ladder, and the
 *    host worker.
 *
 * This header owns every compile-time constant the three units share, plus
 * the prototypes of the five functions that cross a translation-unit
 * boundary (the two ThreadX workers and the three USBX storage callbacks).
 * All other helpers and file-static state stay private to their unit.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

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
 * @enum selftest_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_selftest_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_selftest_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_selftest_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_selftest_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_selftest_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} selftest_hex_t;

/**
 * @enum selftest_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_selftest_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_selftest_dec_radix   = 10U,  /**< Base for decimal conversion. */
} selftest_mask_t;

/**
 * @enum selftest_verify_t
 * @brief Content-verification geometry over the device's FAT16 volume.
 */
typedef enum : uint32_t {
  k_selftest_burst_blocks  = 8U,          /**< Blocks per READ(10) burst.   */
  k_selftest_burst_bytes   = 4096U,       /**< 8 x 512 B burst buffer size. */
  k_selftest_target_lun    = 0U,          /**< Single-LUN device.           */
  k_selftest_wp_probe_lba  = 50U,         /**< Data-region LBA for WP test. */
  k_selftest_no_mismatch   = 0xFFFFFFFFU, /**< Probe: no mismatch found.    */
  k_selftest_ms_per_sec    = 1000U,       /**< Milliseconds per second.     */
  k_selftest_bytes_per_kib = 1024U,       /**< Bytes per KiB (rate math).   */
} selftest_verify_t;

/**
 * @enum selftest_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_selftest_phase_boot      = 0U, /**< Host thread not yet started.    */
  k_selftest_phase_host_init = 1U, /**< ra8_usb_hmsc_init issued.       */
  k_selftest_phase_enum      = 2U, /**< Enumerating the FS device.      */
  k_selftest_phase_mount     = 3U, /**< Mounting the FAT16 volume.      */
  k_selftest_phase_verify    = 4U, /**< Streaming + comparing MRAM.BIN. */
  k_selftest_phase_wp        = 5U, /**< Write-protect rejection test.   */
  k_selftest_phase_pass      = 6U, /**< Full config B pass.             */
} selftest_phase_t;

/**
 * @enum selftest_mram_t
 * @brief The MRAM window the device-side volume exposes.
 */
typedef enum : uint32_t {
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address. */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.             */
} selftest_mram_t;

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.      */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                 */
} scsi_sense_code_t;

/** @brief SCSI sense triple for a write to the protected medium. */
typedef enum : uint8_t {
  k_scsi_sense_data_protect  = 0x07U, /**< Sense key: DATA PROTECT. */
  k_scsi_asc_write_protected = 0x27U, /**< ASC: WRITE PROTECTED.    */
} scsi_wp_sense_t;

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
 * @enum selftest_fat_boot_t
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
  k_boot_volume_id   = 0x52A8D20AU, /**< Arbitrary volume serial.    */
  k_boot_sig_lo      = 0x55U,       /**< Boot signature low byte.    */
  k_boot_sig_hi      = 0xAAU,       /**< Boot signature high byte.   */
  k_boot_sig_lo_off  = 510U,        /**< Signature low-byte offset.  */
  k_boot_sig_hi_off  = 511U,        /**< Signature high-byte offset. */
} selftest_fat_boot_t;

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

/**
 * @enum selftest_word_pack_t
 * @brief 32-bit little-endian split constants.
 */
typedef enum : uint32_t {
  k_word_shift = 16U,     /**< Bits per half-word. */
  k_word_mask  = 0xFFFFU, /**< Low half-word mask. */
} selftest_word_pack_t;

#ifndef RA8_OFF_TARGET

#include "tx_api.h"
#include "ux_api.h"

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
 *
 * @details Bound checks the request against the volume, then fills each
 * block from the synthesized FAT16 layout. LED1 toggles per call so the
 * self-loop traffic is visible on the board. Defined in
 * `usb_selftest_fs_host_fatdisk.c`; referenced by `main.c` when it
 * registers the USBX storage class.
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
 * @post The device-side read counter advanced.
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
 * working afterwards). Defined in `usb_selftest_fs_host_fatdisk.c`.
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
 * Defined in `usb_selftest_fs_host_fatdisk.c`.
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
 * @brief Device-side worker: bring the HS device stack up, then park.
 *
 * @details USBX system + device stack + MSC class + DCD bridge on the
 * USBHS controller, then DPRPU attach. The FS host downstream forces a
 * full-speed link, so the stack serves the FS-fallback framework. USBX
 * runs the SCSI/BBB state machine on its own class threads after this.
 * Defined in `usb_selftest_fs_host_fatdisk.c`; spawned by
 * `tx_application_define` in `main.c`.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-HS pins + UTMI PLL are up (main did both).
 * @post The HS device is attached and serviceable.
 * @post On any bring-up failure the thread exits (probes show where).
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
VOID selftest_device_worker(ULONG arg);

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full
 * config B ladder with a retry pause until it passes; afterwards parks so
 * the verdict stays on the wire. Defined in
 * `usb_selftest_fs_host_host.c`; spawned by `tx_application_define`.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread (lower priority than
 *      the USBX device-side threads).
 * @pre The FS host pins, VBUSEN, and 48 MHz clock are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Polled host stack: blocking calls, ms timeouts via ra8_time.
 * @since 0.1.0
 */
VOID selftest_host_worker(ULONG arg);

#endif /* !RA8_OFF_TARGET */
