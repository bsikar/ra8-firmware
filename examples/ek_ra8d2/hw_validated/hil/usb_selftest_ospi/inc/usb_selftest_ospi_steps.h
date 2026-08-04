/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_ospi/inc/usb_selftest_ospi_steps.h
 * @brief Shared constants + step prototypes for the OSPI USB self-loop app
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Self-contained contract surface shared between the app's translation
 * units. ``main.c`` owns boot, the ThreadX workers, and ``tx_application_
 * define``; the FAT16-synthesis / console / Mass-Storage-media callbacks
 * live in ``usb_selftest_ospi_format.c``; the polled host-side pass ladder
 * (enumerate -> mount -> verify -> write-protect) lives in
 * ``usb_selftest_ospi_host.c``. This header carries every app-local enum
 * (volume geometry, OSPI pattern coefficients, SCSI sense triples, console
 * sizing, J-Link phase markers) plus the cross-TU function prototypes so no
 * symbol is defined in one TU and referenced from another without going
 * through this header.
 *
 * The USBX-typed entry points (``UCHAR*`` / ``UINT`` / ``VOID*`` / ``ULONG``)
 * are only declared when ThreadX/USBX is in the build, i.e. outside
 * ``RA8_OFF_TARGET``; the plain console helpers are likewise gated since
 * their definitions are.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

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
  k_selftest_phase_boot      = 0U, /**< Host thread not yet started.   */
  k_selftest_phase_host_init = 1U, /**< ra8_usb_hmsc_init issued.      */
  k_selftest_phase_enum      = 2U, /**< Enumerating the FS device.     */
  k_selftest_phase_mount     = 3U, /**< Mounting the FAT16 volume.     */
  k_selftest_phase_verify    = 4U, /**< Streaming + checking OSPI.BIN. */
  k_selftest_phase_wp        = 5U, /**< Write-protect rejection test.  */
  k_selftest_phase_pass      = 6U, /**< Full OSPI self-loop pass.      */
} selftest_phase_t;

/**
 * @enum selftest_ospi_t
 * @brief OSPI flash geometry the device-side volume is backed by.
 *
 * @details The 1 MiB window the device programs + exposes lives at
 * offset 0x100000 in the IS25LX512M (clear of flash_journal's offset-0
 * record). ra8_xspi addresses the chip 0-based. Erase granularity is the
 * IS25LX512M 4 KiB sector.
 */
typedef enum : uint32_t {
  k_ospi_instance     = 0U,          /**< xSPI controller instance.      */
  k_ospi_test_offset  = 0x00100000U, /**< 1 MiB into the chip (scratch). */
  k_ospi_bytes        = 0x00100000U, /**< 1 MiB exposed window size.     */
  k_ospi_erase_sector = 0x00001000U, /**< IS25LX512M 4 KiB erase sector. */
  k_ospi_erase_count  = 256U,        /**< 1 MiB / 4 KiB = 256 erases.    */
} selftest_ospi_t;

/**
 * @enum selftest_pattern_t
 * @brief Deterministic sector-pattern coefficients (device + host agree).
 *
 * @details Sector @p s, byte @p i holds
 * ``(s * smul + i * imul + bias) & 0xFF``. Both the boot programmer and
 * the host verifier compute this identically, so the host never has to
 * read the OSPI (single-controller contention-free).
 */
typedef enum : uint32_t {
  k_ospi_pat_smul = 31U,   /**< Per-sector multiplier. */
  k_ospi_pat_imul = 131U,  /**< Per-byte multiplier.   */
  k_ospi_pat_bias = 0xA5U, /**< Constant bias.         */
  k_ospi_pat_mask = 0xFFU, /**< Byte mask.             */
} selftest_pattern_t;

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
  k_fat_data_clusters    = 2048U,   /**< Clusters backed by MRAM (1 MiB).   */
  k_fat_last_data_clus   = 2049U,   /**< Last cluster of MRAM.BIN.          */
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

#include "ra8_fs.h"
#include "ra8_usb_hmsc.h"
#include "tx_api.h"

/* -------------------------------------------------------------------------- */
/* FAT16 synthesis + storage media callbacks (usb_selftest_ospi_format.c) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Compute the deterministic pattern for one window data sector.
 *
 * @details Fills @p out with ``(win_sector * smul + i * imul + bias)``
 * per byte (see ::selftest_pattern_t). This is the single source of
 * truth: the boot programmer writes it into OSPI and the host verifier
 * recomputes it -- so the host never reads the flash and there is no
 * single-controller contention.
 *
 * @param[in]  win_sector 0-based sector index within the OSPI window.
 * @param[out] out        512-byte destination buffer.
 *
 * @pre @p out has 512 writable bytes.
 * @post @p out holds the sector's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
void selftest_pattern_fill(uint32_t win_sector, UCHAR* out);

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
 *
 * @details Bound checks the request against the volume, then fills each
 * block via the FAT16 sector synthesizer. LED1 toggles per call so the
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
 * @post The read-call probe advanced.
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

/* -------------------------------------------------------------------------- */
/* Console helpers (usb_selftest_ospi_format.c) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by the bounded string-length scan.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_selftest_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print(const char* text);

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @details Digit-reversal into a bounded scratch buffer.
 *
 * @param[in] value Value to print.
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_dec(uint32_t value);

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @details Width is clamped to 8 hex digits.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_selftest_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; print errors inside are not recoverable
 * anyway, so the first failing chunk's code is returned.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The diagnostic line is queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_fail(const char* what, ra8_err_t err);

/* -------------------------------------------------------------------------- */
/* Host-side pass ladder (usb_selftest_ospi_host.c) */
/* -------------------------------------------------------------------------- */

/**
 * @brief One full host-side pass: enumerate, mount, verify, WP.
 *
 * @details Phases mirror ::selftest_phase_t and are mirrored into the
 * J-Link phase probe for readout. On any failure the host controller is
 * closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed OSPI PASS.
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
