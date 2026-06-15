/**
 * @file examples/ek_ra8d2/hw_pending/usb_selftest_soak/main.c
 * @brief USB self-loop endurance soak + throughput benchmark (board-only)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The robustness/benchmark member of the self-loop matrix: same dual-stack
 * image as config A (HS host + FS device on the J7<->J11 cable), but the host
 * worker REPEATS the 1 MiB MRAM integrity sweep ::k_selftest_soak_iters times
 * back to back instead of once. Every 4 KiB READ(10) burst is still memcmp'd
 * against the real MRAM window, so a single corrupted or dropped transfer
 * anywhere in the soak fails the run -- this is the endurance proof that the
 * USB transport stays byte-perfect under sustained load. The aggregate volume
 * and elapsed time are summed across all iterations for a stable throughput
 * benchmark (more representative than a single short sweep), then the RO
 * write-rejection is confirmed once.
 *
 *  - USBFS (J11) = DEVICE: the USBX MSC class exposing the 1 MiB MRAM window
 *    at 0x02000000 as a read-only synthesized FAT16 volume (``MRAM.BIN``).
 *  - USBHS (J7) = HOST: the polled first-party host stack (`ra_usb_hmsc` +
 *    `ra_fs`) on a low-priority ThreadX thread.
 *
 * Entirely on-chip: no Mac/PC host -- the board both hosts and devices itself,
 * so the soak runs anywhere the loop cable is fitted. Verdicts stream over SCI8
 * (J-Link OB CDC, 115200); ::s_dbg_pass_count mirrors the completed-iteration
 * count for a J-Link readout of soak progress.
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN as GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_hmsc.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra_time millisecond
 * counter (for ra_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer (for tx_thread_sleep and USBX class-thread scheduling).
 * The 1 ms pulse also recovers the DCD's storm-guard NVIC mask. */
extern void ra_time_on_tick(void);
extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick (ra_time_init) BEFORE tx_kernel_enter,
 *          and this app's setup window is long (the U15 expander I2C
 *          transaction blocks for milliseconds), so the tick WILL fire
 *          pre-kernel. Feeding _tx_timer_interrupt into ThreadX's
 *          still-zeroed timer state walks a bogus expiration list and
 *          bus-faults (observed: IMPRECISERR HardFault from SysTick).
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra_port_pin_t k_selftest_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra_port_pin_t k_selftest_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USBFS D+ (P8_14). */
static const ra_port_pin_t k_selftest_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USBFS D- (P8_15). */
static const ra_port_pin_t k_selftest_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_selftest_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra_port_pin_t k_selftest_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_selftest_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_selftest_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
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
  k_selftest_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.    */
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
  k_selftest_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".         */
  k_selftest_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_selftest_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count.  */
  k_selftest_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_selftest_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
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
  k_selftest_burst_blocks  = 8U,          /**< Blocks per READ(10) burst.     */
  k_selftest_burst_bytes   = 4096U,       /**< 8 x 512 B burst buffer size.   */
  k_selftest_target_lun    = 0U,          /**< Single-LUN device.             */
  k_selftest_wp_probe_lba  = 50U,         /**< Data-region LBA for WP test.  */
  k_selftest_no_mismatch   = 0xFFFFFFFFU, /**< Probe: no mismatch found.     */
  k_selftest_ms_per_sec    = 1000U,       /**< Milliseconds per second.      */
  k_selftest_bytes_per_kib = 1024U,       /**< Bytes per KiB (rate math).    */
  k_selftest_soak_iters    = 16U,         /**< Soak: 1 MiB verify repeats.   */
  k_selftest_bytes_per_mib = 1048576U,    /**< Bytes per MiB (aggregate).    */
} selftest_verify_t;

/**
 * @enum selftest_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_selftest_phase_boot      = 0U, /**< Host thread not yet started.    */
  k_selftest_phase_host_init = 1U, /**< ra_usb_hmsc_init issued.        */
  k_selftest_phase_enum      = 2U, /**< Enumerating the FS device.      */
  k_selftest_phase_mount     = 3U, /**< Mounting the FAT16 volume.      */
  k_selftest_phase_verify    = 4U, /**< Streaming + comparing MRAM.BIN. */
  k_selftest_phase_wp        = 5U, /**< Write-protect rejection test.   */
  k_selftest_phase_pass      = 6U, /**< Full config A pass.             */
} selftest_phase_t;

/**
 * @enum selftest_mram_t
 * @brief The MRAM window the device-side volume exposes.
 */
typedef enum : uint32_t {
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address.   */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.               */
} selftest_mram_t;

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range. */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none. */
} scsi_sense_code_t;

/** @brief SCSI sense triple for a write to the protected medium. */
typedef enum : uint8_t {
  k_scsi_sense_data_protect  = 0x07U, /**< Sense key: DATA PROTECT.     */
  k_scsi_asc_write_protected = 0x27U, /**< ASC: WRITE PROTECTED.       */
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
  k_fat_reserved_sectors = 1U,      /**< Boot sector only.                   */
  k_fat_num_fats         = 1U,      /**< Single FAT copy.                    */
  k_fat_fat_sectors      = 17U,     /**< FAT16 size for 4098 entries.        */
  k_fat_root_entries     = 512U,    /**< Root directory entries.             */
  k_fat_root_sectors     = 32U,     /**< 512 entries x 32 B / 512 B.         */
  k_fat_data_sectors     = 4096U,   /**< Padded data region (>= 4085).       */
  k_fat_fat_lba          = 1U,      /**< First FAT sector.                   */
  k_fat_root_lba         = 18U,     /**< First root-directory sector.        */
  k_fat_data_lba         = 50U,     /**< First data sector (cluster 2).      */
  k_fat_total_sectors    = 4146U,   /**< 1 + 17 + 32 + 4096.                 */
  k_fat_first_cluster    = 2U,      /**< FAT data area starts at cluster 2.  */
  k_fat_mram_clusters    = 2048U,   /**< Clusters backed by MRAM (1 MiB).    */
  k_fat_last_mram_clus   = 2049U,   /**< Last cluster of MRAM.BIN.           */
  k_fat_entries_per_sec  = 256U,    /**< FAT16 entries per 512-byte sector.  */
  k_fat_eoc              = 0xFFFFU, /**< End-of-chain marker.              */
  k_fat_entry0           = 0xFFF8U, /**< FAT[0]: media F8 + filler.        */
} selftest_fat_geom_t;

/**
 * @enum selftest_fat_boot_t
 * @brief Boot-sector field values (MS FAT spec 1.03 sec 3.1).
 */
typedef enum : uint32_t {
  k_boot_jmp0        = 0xEBU,       /**< Short JMP opcode.            */
  k_boot_jmp1        = 0x3CU,       /**< JMP displacement.            */
  k_boot_jmp2        = 0x90U,       /**< NOP.                         */
  k_boot_media       = 0xF8U,       /**< Fixed-disk media byte.       */
  k_boot_sec_per_trk = 32U,         /**< Geometry filler.             */
  k_boot_num_heads   = 16U,         /**< Geometry filler.             */
  k_boot_drive_num   = 0x80U,       /**< BIOS drive number.           */
  k_boot_ext_sig     = 0x29U,       /**< Extended boot signature.     */
  k_boot_volume_id   = 0x52A8D20AU, /**< Arbitrary volume serial.     */
  k_boot_sig_lo      = 0x55U,       /**< Boot signature low byte.     */
  k_boot_sig_hi      = 0xAAU,       /**< Boot signature high byte.    */
  k_boot_sig_lo_off  = 510U,        /**< Signature low-byte offset.   */
  k_boot_sig_hi_off  = 511U,        /**< Signature high-byte offset.  */
} selftest_fat_boot_t;

/**
 * @enum selftest_fat_off_t
 * @brief Byte offsets inside the boot sector and directory entries.
 */
typedef enum : uint8_t {
  k_bpb_off_jmp        = 0U,    /**< Jump instruction.                 */
  k_bpb_off_oem        = 3U,    /**< OEM name (8 bytes).               */
  k_bpb_off_bps        = 11U,   /**< Bytes per sector.                 */
  k_bpb_off_spc        = 13U,   /**< Sectors per cluster.              */
  k_bpb_off_rsvd       = 14U,   /**< Reserved sector count.            */
  k_bpb_off_nfats      = 16U,   /**< Number of FATs.                   */
  k_bpb_off_rootent    = 17U,   /**< Root entry count.                 */
  k_bpb_off_totsec16   = 19U,   /**< Total sectors (16-bit).           */
  k_bpb_off_media      = 21U,   /**< Media descriptor.                 */
  k_bpb_off_fatsz16    = 22U,   /**< Sectors per FAT.                  */
  k_bpb_off_spt        = 24U,   /**< Sectors per track.                */
  k_bpb_off_heads      = 26U,   /**< Head count.                       */
  k_bpb_off_drvnum     = 36U,   /**< Drive number.                     */
  k_bpb_off_bootsig    = 38U,   /**< Extended boot signature.          */
  k_bpb_off_volid      = 39U,   /**< Volume serial (4 bytes).          */
  k_bpb_off_label      = 43U,   /**< Volume label (11 bytes).          */
  k_bpb_off_fstype     = 54U,   /**< Filesystem type (8 bytes).        */
  k_dir_entry_bytes    = 32U,   /**< Directory entry size.             */
  k_dir_off_attr       = 11U,   /**< Attribute byte.                   */
  k_dir_off_cluster_lo = 26U,   /**< First cluster (low word).         */
  k_dir_off_size       = 28U,   /**< File size (32-bit LE).            */
  k_dir_attr_volume    = 0x08U, /**< Volume-label attribute.           */
  k_dir_attr_read_only = 0x01U, /**< Read-only attribute.              */
  k_dir_name_bytes     = 11U,   /**< 8.3 name field length.            */
  k_byte_shift         = 8U,    /**< Bits per byte for LE packing.     */
  k_byte_mask          = 0xFFU, /**< Low-byte mask.                    */
} selftest_fat_off_t;

/**
 * @enum selftest_word_pack_t
 * @brief 32-bit little-endian split constants.
 */
typedef enum : uint32_t {
  k_word_shift = 16U,     /**< Bits per half-word.   */
  k_word_mask  = 0xFFFFU, /**< Low half-word mask.   */
} selftest_word_pack_t;

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage                                        */
/* -------------------------------------------------------------------------- */

/**
 * @var s_device_thread
 * @brief ThreadX TCB for the USBX device-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_device_thread;

/**
 * @var s_device_stack
 * @brief Stack backing storage for ::s_device_thread.
 * @since 0.1.0
 */
static UCHAR s_device_stack[k_selftest_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the polled host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread (ra_fs walks live here).
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_selftest_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_selftest_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "SELFTEST MRAM RO";
static UCHAR s_msc_product_rev[] = "0001";

/** @brief Boot-sector OEM name (8 bytes, space padded). */
static const UCHAR s_fat_oem_name[8] = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};

/** @brief Volume label, 11 bytes space padded (also the root entry). */
static const UCHAR s_fat_volume_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};

/** @brief Filesystem-type tag, 8 bytes space padded. */
static const UCHAR s_fat_fs_type[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};

/** @brief 8.3 directory name of the exposed file: "MRAM.BIN". */
static const UCHAR s_fat_file_name[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/* -------------------------------------------------------------------------- */
/* J-Link probes                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::selftest_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Bytes content-verified so far this pass. */
static volatile uint32_t s_dbg_verified_bytes;
/** @brief First mismatching MRAM offset (::k_selftest_no_mismatch = none). */
static volatile uint32_t s_dbg_mismatch_off = (uint32_t)k_selftest_no_mismatch;
/** @brief Milliseconds the 1 MiB verify streamed for. */
static volatile uint32_t s_dbg_verify_ms;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Device-side media_read invocations. */
static volatile uint32_t s_dbg_read_calls;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints)              */
/* -------------------------------------------------------------------------- */

/* Single-interface MSC config: bulk-only transport, SCSI command set.
 * EP1 IN + EP2 OUT, 64-byte MPS. PID 0x000E marks the self-test
 * identity apart from the Mac-facing usb_msc_mram (0x000C). */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U, /* class      = per-interface        */
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x0EU, /* PID = 0x000E (pid.codes test).    */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (32 bytes total). */
  0x09U,
  0x02U,
  0x20U,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface descriptor -- MSC, SCSI, BBB. */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x02U,
  0x08U,
  0x06U,
  0x50U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 64-byte MPS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 64-byte MPS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

/**
 * @var s_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Each entry: 2 bytes lang-id, 1 byte string index, 1 byte
 *          length, then ASCII bytes.
 * @since 0.1.0
 */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie". */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "RA8D2 SELFTEST". */
  0x09U,
  0x04U,
  0x02U,
  0x0EU,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'S',
  'E',
  'L',
  'F',
  'T',
  'E',
  'S',
  'T',
  /* idx 3: serial. */
  0x09U,
  0x04U,
  0x03U,
  0x08U,
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '4',
};

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* FAT16 synthesis (identical layout to usb_msc_mram)                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Write a 16-bit value little-endian into a byte buffer.
 *
 * @details Low byte first, high byte second, per the FAT on-disk layout.
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
static void selftest_put16(UCHAR* dst, uint16_t value)
{
  dst[0] = (UCHAR)(value & (uint16_t)k_byte_mask);
  dst[1] = (UCHAR)((value >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_mask);
}

/**
 * @brief Write a 32-bit value little-endian into a byte buffer.
 *
 * @details Two ::selftest_put16 halves, low half-word first.
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
static void selftest_put32(UCHAR* dst, uint32_t value)
{
  selftest_put16(dst, (uint16_t)(value & (uint32_t)k_word_mask));
  selftest_put16(dst + 2U, (uint16_t)(value >> (uint32_t)k_word_shift));
}

/**
 * @brief Synthesize the FAT16 boot sector (MS FAT spec 1.03 sec 3.1).
 *
 * @details BPB for the padded 4146-sector volume plus the 0x55AA
 * signature; geometry constants in ::selftest_fat_geom_t.
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
static void selftest_fat_fill_boot(UCHAR* out)
{
  out[k_bpb_off_jmp]      = (UCHAR)k_boot_jmp0;
  out[k_bpb_off_jmp + 1U] = (UCHAR)k_boot_jmp1;
  out[k_bpb_off_jmp + 2U] = (UCHAR)k_boot_jmp2;
  (void)memcpy(&out[k_bpb_off_oem], s_fat_oem_name, sizeof(s_fat_oem_name));
  selftest_put16(&out[k_bpb_off_bps], (uint16_t)k_selftest_block_size);
  out[k_bpb_off_spc] = 1U;
  selftest_put16(&out[k_bpb_off_rsvd], (uint16_t)k_fat_reserved_sectors);
  out[k_bpb_off_nfats] = (UCHAR)k_fat_num_fats;
  selftest_put16(&out[k_bpb_off_rootent], (uint16_t)k_fat_root_entries);
  selftest_put16(&out[k_bpb_off_totsec16], (uint16_t)k_fat_total_sectors);
  out[k_bpb_off_media] = (UCHAR)k_boot_media;
  selftest_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_fat_fat_sectors);
  selftest_put16(&out[k_bpb_off_spt], (uint16_t)k_boot_sec_per_trk);
  selftest_put16(&out[k_bpb_off_heads], (uint16_t)k_boot_num_heads);
  out[k_bpb_off_drvnum]  = (UCHAR)k_boot_drive_num;
  out[k_bpb_off_bootsig] = (UCHAR)k_boot_ext_sig;
  selftest_put32(&out[k_bpb_off_volid], (uint32_t)k_boot_volume_id);
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
static void selftest_fat_fill_fat(uint32_t fat_sector, UCHAR* out)
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
    selftest_put16(&out[j * 2U], value);
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
static void selftest_fat_fill_root(uint32_t root_sector, UCHAR* out)
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
  selftest_put16(&entry[k_dir_off_cluster_lo], (uint16_t)k_fat_first_cluster);
  selftest_put32(&entry[k_dir_off_size], (uint32_t)k_mram_bytes);
}

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
static void selftest_fat_fill_sector(uint32_t lba, UCHAR* out)
{
  (void)memset(out, 0, (size_t)k_selftest_block_size);
  if (lba == 0U) {
    selftest_fat_fill_boot(out);
    return;
  }
  if (lba < (uint32_t)k_fat_root_lba) {
    selftest_fat_fill_fat(lba - (uint32_t)k_fat_fat_lba, out);
    return;
  }
  if (lba < (uint32_t)k_fat_data_lba) {
    selftest_fat_fill_root(lba - (uint32_t)k_fat_root_lba, out);
    return;
  }
  const uint32_t cluster = (lba - (uint32_t)k_fat_data_lba) + (uint32_t)k_fat_first_cluster;
  if (cluster <= (uint32_t)k_fat_last_mram_clus) {
    const uint32_t offset =
      (cluster - (uint32_t)k_fat_first_cluster) * (uint32_t)k_selftest_block_size;
    const UCHAR* mram = (const UCHAR*)(uintptr_t)((uint32_t)k_mram_base_addr + offset);
    (void)memcpy(out, mram, (size_t)k_selftest_block_size);
  }
}

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (read / write / status)                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
 *
 * @details Bound checks the request against the volume, then fills each
 * block via ::selftest_fat_fill_sector. LED1 toggles per call so the
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
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT selftest_msc_read(VOID*  storage,
                              ULONG  lun,
                              UCHAR* data_pointer,
                              ULONG  number_blocks,
                              ULONG  lba,
                              ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_fat_total_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  for (ULONG i = 0UL; i < number_blocks; i++) {
    selftest_fat_fill_sector((uint32_t)(lba + i), &data_pointer[i * (ULONG)k_selftest_block_size]);
  }
  *media_status = 0UL;
  (void)ra_board_led_toggle(k_ra_board_led1);
  return UX_SUCCESS;
}

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
/* cppcheck-suppress-begin [constParameterCallback] -- USBX's
 * ux_slave_class_storage_media_write function-pointer signature takes
 * non-const UCHAR*; we cannot const-qualify the parameter. */
static UINT selftest_msc_write(VOID*  storage,
                               ULONG  lun,
                               UCHAR* data_pointer,
                               ULONG  number_blocks,
                               ULONG  lba,
                               ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)data_pointer;
  (void)number_blocks;
  (void)lba;
  *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_data_protect,
                                                       k_scsi_asc_write_protected,
                                                       k_scsi_ascq_none);
  return UX_ERROR;
}
/* cppcheck-suppress-end [constParameterCallback] */

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
static UINT selftest_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC)                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @details Standard '0'-'9' then 'A'-'F' mapping.
 *
 * @param[in] nibble 4-bit value.
 *
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 *
 * @pre Caller has already masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is in the printable hex range.
 * @post No state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t selftest_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_selftest_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_selftest_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_selftest_print_cap).
 *
 * @details Linear scan with a hard upper bound.
 *
 * @param[in] text NUL-terminated string.
 *
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the returned length.
 * @post No state changes.
 * @post Return value never exceeds ::k_selftest_print_cap.
 *
 * @note Bounded scan -- never walks past the cap on a missing NUL.
 * @since 0.1.0
 */
static uint32_t selftest_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_selftest_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @details Thin wrapper fixing the console channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_selftest_sci_channel, data, len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::selftest_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_selftest_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print(const char* text)
{
  return selftest_sci_write((const uint8_t*)text, selftest_str_len(text));
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @details Digit-reversal into a bounded scratch buffer.
 *
 * @param[in] value Value to print.
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print_dec(uint32_t value)
{
  uint8_t  scratch[k_selftest_dec_chars_u32] = {};
  uint8_t  out[k_selftest_dec_chars_u32]     = {};
  uint8_t  count                             = 0U;
  uint32_t v                                 = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return selftest_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_selftest_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_selftest_dec_radix));
    v              = v / k_selftest_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return selftest_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @details Width is clamped to 8 hex digits.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_selftest_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_selftest_hex_chars_u32] = {};
  uint8_t width                         = digits;
  if (width > (uint8_t)k_selftest_hex_chars_u32) {
    width = (uint8_t)k_selftest_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_selftest_nibble_bits);
    out[i]              = selftest_nibble_to_hex((value >> shift) & k_selftest_nibble_mask);
  }
  return selftest_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; print errors inside are not recoverable
 * anyway, so the first failing chunk's code is returned.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The diagnostic line is queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print_fail(const char* what, ra_err_t err)
{
  ra_err_t e = selftest_print("ra8d2 selftest: FAIL ");
  if (e != k_ra_ok) {
    return e;
  }
  e = selftest_print(what);
  if (e != k_ra_ok) {
    return e;
  }
  e = selftest_print(" err=0x");
  if (e != k_ra_ok) {
    return e;
  }
  e = selftest_print_hex((uint32_t)err, (uint8_t)k_selftest_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return selftest_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: ra_fs backend over the polled host-MSC class                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief ra_fs backend: read blocks via SCSI READ(10) over the loop.
 *
 * @details Single-LUN device; the 16-bit READ(10) count bound is upheld
 * by the small chunk sizes the suite issues.
 *
 * @param[in]  ctx   Unused backend cookie.
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of 512-byte blocks.
 * @param[out] buf   Destination buffer (count * 512 bytes).
 *
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_ok Blocks transferred.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre @p buf is non-NULL and large enough for the transfer.
 * @post On k_ra_ok the buffer holds the device data.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
selftest_backend_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  return ra_usb_hmsc_read10((uint8_t)k_selftest_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra_fs backend: write blocks via SCSI WRITE(10) over the loop.
 *
 * @details The device LUN is write-protected; this path only exists so
 * the backend struct is total. ra_fs never writes during the read-only
 * suite.
 *
 * @param[in] ctx   Unused backend cookie.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of 512-byte blocks.
 * @param[in] buf   Source buffer (count * 512 bytes).
 *
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_ok Blocks transferred (never for this device).
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre @p buf is non-NULL and holds the full transfer.
 * @post On success the device sectors hold the buffer data.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
selftest_backend_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  return ra_usb_hmsc_write10((uint8_t)k_selftest_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra_fs backend: report capacity via SCSI READ_CAPACITY(10).
 *
 * @details Pass-through to the class layer for LUN 0.
 *
 * @param[in]  ctx         Unused backend cookie.
 * @param[out] block_count Total number of blocks.
 * @param[out] block_size  Block size in bytes (512 for this volume).
 *
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_ok Outputs are valid.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre Both output pointers are non-NULL.
 * @post On k_ra_ok both outputs are filled from the device.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
selftest_backend_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  return ra_usb_hmsc_read_capacity((uint8_t)k_selftest_target_lun, block_count, block_size);
}

/**
 * @brief Map a detected filesystem type to a printable name.
 *
 * @details Total over the enum; unknown maps to "unknown".
 *
 * @param[in] type Mount-time detection result.
 *
 * @return Static NUL-terminated name string.
 * @retval "fat16" For ::k_ra_fs_type_fat16 (the expected verdict).
 *
 * @pre None -- total over the enum.
 * @pre @p type came from a populated mount struct.
 * @post No state changes.
 * @post Returned pointer references static storage.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static const char* selftest_fs_type_name(ra_fs_type_t type)
{
  switch (type) {
    case k_ra_fs_type_fat12:
      return "fat12";
    case k_ra_fs_type_fat16:
      return "fat16";
    case k_ra_fs_type_fat32:
      return "fat32";
    case k_ra_fs_type_exfat:
      return "exfat";
    case k_ra_fs_type_unknown:
    default:
      return "unknown";
  }
}

/**
 * @brief Mount the loop device's volume through the USB-MSC backend.
 *
 * @details Builds the backend vtable over the polled host class and
 * prints the detected filesystem type (must be fat16 for this device).
 *
 * @param[out] out_mount Receives the mount handle on success.
 *
 * @return ra_err_t from ::ra_fs_mount.
 * @retval k_ra_ok Volume mounted; the type line was printed.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre @p out_mount is non-NULL.
 * @post On k_ra_ok the mount handle is live and must be unmounted later.
 * @post The "mounted fs=" line is queued on success.
 *
 * @note Reads the BPB chain over the self-loop cable.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_mount_volume(ra_fs_mount_t** out_mount)
{
  const ra_fs_backend_t backend = {
    .read_block   = selftest_backend_read,
    .write_block  = selftest_backend_write,
    .get_capacity = selftest_backend_capacity,
    .ctx          = nullptr,
  };
  ra_err_t err = ra_fs_mount(&backend, out_mount);
  if (err != k_ra_ok) {
    (void)selftest_print_fail("mount", err);
    return err;
  }
  err = selftest_print("ra8d2 selftest: mounted fs=");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print(selftest_fs_type_name((*out_mount)->type));
  if (err != k_ra_ok) {
    return err;
  }
  return selftest_print("\r\n");
}

/**
 * @brief Raw multi-block READ(10) of the MRAM data region vs MRAM.
 *
 * @details Streams the 1 MiB data region straight through the SCSI
 * READ(10) entry point in 8-block (4 KiB) bursts -- one CBW per burst,
 * not per sector -- and memcmp's each burst against the same offset of
 * the real MRAM window at 0x02000000. This deliberately bypasses
 * ra_fs: the filesystem walk re-reads FAT/metadata sectors per cluster
 * (a ~250x device-read amplification that throttled the loop to under
 * 1 sector/s), whereas the raw path is the direct integrity proof --
 * "the SCSI transport returns the chip's flash byte for byte" -- and
 * runs at the cable's real rate. The mount step (separate phase)
 * already proved the host can PARSE the FAT16 volume over the loop.
 *
 * Data region: FAT16 LBA ::k_fat_data_lba is cluster 2 = MRAM offset 0,
 * so LBA (data_lba + b) holds MRAM[b * 512].
 *
 * @return ra_err_t verdict.
 * @retval k_ra_ok            All 1 MiB matched byte for byte.
 * @retval k_ra_err_invalid_state A byte differed from MRAM.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre The device side exposes the synthesized MRAM volume.
 * @post ::s_dbg_verified_bytes / ::s_dbg_verify_ms / ::s_dbg_mismatch_off
 *       reflect the outcome.
 * @post No filesystem handle is held (raw SCSI path).
 *
 * @note Blocking; 256 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_verify_mram_raw(void)
{
  static uint8_t s_burst[k_selftest_burst_bytes] = {};

  s_dbg_verified_bytes = 0U;
  s_dbg_mismatch_off   = (uint32_t)k_selftest_no_mismatch;
  const uint32_t t0    = ra_time_ms();
  for (uint32_t blk = 0U; blk < (uint32_t)k_fat_mram_clusters;
       blk += (uint32_t)k_selftest_burst_blocks) {
    const uint32_t lba = (uint32_t)k_fat_data_lba + blk;
    ra_err_t       err = ra_usb_hmsc_read10((uint8_t)k_selftest_target_lun,
                                            lba,
                                            (uint16_t)k_selftest_burst_blocks,
                                            s_burst);
    if (err != k_ra_ok) {
      (void)selftest_print_fail("READ(10) burst", err);
      return err;
    }
    const uint32_t offset = blk * (uint32_t)k_selftest_block_size;
    const uint8_t* mram   = (const uint8_t*)(uintptr_t)((uint32_t)k_mram_base_addr + offset);
    if (memcmp(s_burst, mram, (size_t)k_selftest_burst_bytes) != 0) {
      s_dbg_mismatch_off = offset;
      (void)selftest_print_fail("content mismatch at 0x", (ra_err_t)offset);
      return k_ra_err_invalid_state;
    }
    s_dbg_verified_bytes = offset + (uint32_t)k_selftest_burst_bytes;
  }
  s_dbg_verify_ms = ra_time_ms() - t0;
  return k_ra_ok;
}

/**
 * @brief Print the verify verdict line (bytes + duration + rate).
 *
 * @details "verified 1048576 bytes in N ms (M KiB/s)".
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The verdict line is queued.
 *
 * @pre ::selftest_verify_mram_file returned k_ra_ok this pass.
 * @pre SCI8 init already ran.
 * @post One verdict line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Rate math guards the divide against a zero duration.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print_verify_verdict(void)
{
  ra_err_t err = selftest_print("ra8d2 selftest: verified ");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print_dec(s_dbg_verified_bytes);
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print(" bytes vs MRAM in ");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print_dec(s_dbg_verify_ms);
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print(" ms (");
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t ms = s_dbg_verify_ms;
  if (ms == 0U) {
    ms = 1U;
  }
  const uint32_t kib_per_s =
    (uint32_t)(((uint64_t)s_dbg_verified_bytes * (uint64_t)k_selftest_ms_per_sec) /
               ((uint64_t)ms * (uint64_t)k_selftest_bytes_per_kib));
  err = selftest_print_dec(kib_per_s);
  if (err != k_ra_ok) {
    return err;
  }
  return selftest_print(" KiB/s)\r\n");
}

/**
 * @brief WRITE(10) into the read-only LUN must be rejected.
 *
 * @details Issues a 1-block WRITE(10) into the data region. The device
 * LUN is write-protected (MODE SENSE WP bit + media_write returns
 * DATA PROTECT), so the host's write entry point must surface an error
 * rather than k_ra_ok -- the MRAM is never modified. This is the
 * write-protection proof.
 *
 * It deliberately stops there: terminating a data-out phase against a
 * write-protected device STALLs the bulk-OUT endpoint, and recovering
 * the BOT transport afterwards (Bulk-Only Mass Storage Reset + Clear
 * Feature ENDPOINT_HALT on both bulk pipes) is not yet implemented in
 * the host class. That STALL/ClearFeature recovery path is tracked as
 * GitHub issue #92's robustness sweep; this pass parks on success, so
 * the post-STALL desync does not affect the verdict.
 *
 * @return ra_err_t verdict.
 * @retval k_ra_ok            The write was rejected (protection works).
 * @retval k_ra_err_invalid_state The write was unexpectedly accepted.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the loop device.
 * @pre The device LUN reports write-protected.
 * @post The device volume is untouched (the write never lands).
 * @post One verdict line is queued on the console.
 *
 * @note Leaves the bulk-OUT pipe halted (see details); pass parks next.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_write_protect_probe(void)
{
  static uint8_t s_wp_buf[k_selftest_block_size] = {};

  ra_err_t err = selftest_print("ra8d2 selftest: WRITE(10) into RO LUN must be rejected...\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  const ra_err_t wr = ra_usb_hmsc_write10((uint8_t)k_selftest_target_lun,
                                          (uint32_t)k_selftest_wp_probe_lba,
                                          1U,
                                          s_wp_buf);
  if (wr == k_ra_ok) {
    (void)selftest_print_fail("write unexpectedly accepted", k_ra_err_invalid_state);
    return k_ra_err_invalid_state;
  }
  err = selftest_print("ra8d2 selftest: write rejected (code 0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)wr, (uint8_t)k_selftest_hex_chars_u32);
  if (err != k_ra_ok) {
    return err;
  }
  return selftest_print("), MRAM protected\r\n");
}

/**
 * @brief Bring the host controller up and enumerate the loop device.
 *
 * @details Initializes the USBHS host, enumerates the downstream FS
 * device over the cable, and prints its VID/PID. On any failure the
 * host controller is closed so the caller's retry starts clean.
 *
 * @param[out] out_device Receives the enumerated device snapshot.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Host up and device enumerated; identity printed.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre @p out_device is non-NULL.
 * @post On k_ra_ok the host controller is live and @p out_device filled.
 * @post On failure the host controller has been closed again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_host_enumerate(ra_usb_hmsc_device_t* out_device)
{
  s_dbg_phase  = (uint32_t)k_selftest_phase_host_init;
  ra_err_t err = selftest_print("ra8d2 selftest: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_hmsc_init(k_ra_usb_speed_hs);
  if (err != k_ra_ok) {
    (void)selftest_print_fail("host init", err);
    return err;
  }
  s_dbg_phase = (uint32_t)k_selftest_phase_enum;
  err         = ra_usb_hmsc_enumerate(out_device);
  if (err != k_ra_ok) {
    (void)selftest_print_fail("enumerate", err);
    (void)ra_usb_hmsc_close();
    return err;
  }
  err = selftest_print("ra8d2 selftest: enumerated vid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)out_device->vendor_id, (uint8_t)k_selftest_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print(" pid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)out_device->product_id, (uint8_t)k_selftest_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  return selftest_print(" over the loop cable\r\n");
}

/**
 * @brief Print the soak aggregate line (iterations + volume + rate).
 *
 * @details "soak: N iters, M MiB verified in T ms (R KiB/s), 0 errors".
 *
 * @param[in] total_bytes Sum of all bytes verified across the soak.
 * @param[in] total_ms    Sum of all per-iteration verify durations (ms).
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The aggregate line is queued.
 *
 * @pre The soak loop completed every iteration with k_ra_ok.
 * @pre SCI8 init already ran.
 * @post One aggregate line is in the SCI8 TX FIFO.
 *
 * @note Rate math guards the divide against a zero duration.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_print_soak_verdict(uint64_t total_bytes, uint64_t total_ms)
{
  uint64_t ms = total_ms;
  if (ms == 0U) {
    ms = 1U;
  }
  const uint32_t kib_per_s = (uint32_t)((total_bytes * (uint64_t)k_selftest_ms_per_sec) /
                                        (ms * (uint64_t)k_selftest_bytes_per_kib));
  ra_err_t       err       = selftest_print("ra8d2 soak: ");
  if (err == k_ra_ok) {
    err = selftest_print_dec((uint32_t)k_selftest_soak_iters);
  }
  if (err == k_ra_ok) {
    err = selftest_print(" iters, ");
  }
  if (err == k_ra_ok) {
    err = selftest_print_dec((uint32_t)(total_bytes / (uint64_t)k_selftest_bytes_per_mib));
  }
  if (err == k_ra_ok) {
    err = selftest_print(" MiB verified in ");
  }
  if (err == k_ra_ok) {
    err = selftest_print_dec((uint32_t)total_ms);
  }
  if (err == k_ra_ok) {
    err = selftest_print(" ms (");
  }
  if (err == k_ra_ok) {
    err = selftest_print_dec(kib_per_s);
  }
  if (err == k_ra_ok) {
    err = selftest_print(" KiB/s), 0 errors\r\n");
  }
  return err;
}

/**
 * @brief Soak pass: enumerate + mount once, then repeat the 1 MiB verify.
 *
 * @details Enumerates and mounts to prove FAT parse, then runs
 * ::selftest_verify_mram_raw ::k_selftest_soak_iters times back to back --
 * every burst is still memcmp'd vs MRAM, so a single corrupted transfer
 * anywhere in the soak fails the run. Aggregates volume + time for a
 * stable throughput benchmark, then confirms the RO write rejection once.
 * ::s_dbg_pass_count mirrors the completed-iteration count for J-Link.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok All iterations matched; SOAK PASS printed.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success LED2 is on and the aggregate + PASS lines are queued.
 * @post On failure the host controller is closed for a clean retry.
 *
 * @note Blocking; ~k_selftest_soak_iters MiB over the loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t selftest_host_pass(void)
{
  ra_usb_hmsc_device_t device = {};
  ra_err_t             err    = selftest_host_enumerate(&device);
  if (err != k_ra_ok) {
    return err;
  }
  s_dbg_phase          = (uint32_t)k_selftest_phase_mount;
  ra_fs_mount_t* mount = nullptr;
  err                  = selftest_mount_volume(&mount);
  if (err != k_ra_ok) {
    (void)ra_usb_hmsc_close();
    return err;
  }
  (void)ra_fs_unmount(mount);

  s_dbg_phase          = (uint32_t)k_selftest_phase_verify;
  uint64_t total_bytes = 0U;
  uint64_t total_ms    = 0U;
  for (uint32_t it = 0U; it < (uint32_t)k_selftest_soak_iters; it++) {
    err = selftest_verify_mram_raw();
    if (err != k_ra_ok) {
      (void)ra_usb_hmsc_close();
      return err;
    }
    total_bytes += (uint64_t)s_dbg_verified_bytes;
    total_ms += (uint64_t)s_dbg_verify_ms;
    s_dbg_pass_count = it + 1U;
  }

  s_dbg_phase = (uint32_t)k_selftest_phase_wp;
  err         = selftest_write_protect_probe();
  if (err == k_ra_ok) {
    err = selftest_print_soak_verdict(total_bytes, total_ms);
  }
  if (err != k_ra_ok) {
    (void)ra_usb_hmsc_close();
    return err;
  }
  s_dbg_phase = (uint32_t)k_selftest_phase_pass;
  err         = selftest_print("ra8d2 soak: USB SELFTEST SOAK PASS\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led2);
  return k_ra_ok;
}

/* -------------------------------------------------------------------------- */
/* Threads                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + FS device stack up.
 *
 * @details One-shot USBX pool + device-stack initialization for the
 * FS-only framework.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure, USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT selftest_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_selftest_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework_fs,
                                     sizeof(s_device_framework_fs),
                                     s_string_framework,
                                     sizeof(s_string_framework),
                                     s_language_id_framework,
                                     sizeof(s_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Registers the Mass-Storage class with the read-only MRAM LUN.
 *
 * @details Single LUN, write-protected, FAT16 geometry from
 * ::selftest_fat_geom_t, media callbacks above.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::selftest_usbx_stack_up has succeeded.
 * @pre Media read/write/status callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0.
 * @post LUN0 advertises the read-only synthesized FAT16 volume.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT selftest_msc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)k_fat_total_sectors - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_selftest_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    selftest_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    selftest_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    selftest_msc_status;

  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_storage",
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Device-side worker: bring the FS device stack up, then park.
 *
 * @details USBX system + device stack + MSC class + DCD bridge on the
 * USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB state
 * machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable.
 * @post On any bring-up failure the thread exits (probes show where).
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID selftest_device_worker(ULONG arg)
{
  (void)arg;

  if (selftest_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (selftest_msc_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs) != k_ra_ok) {
    return;
  }
  if (ra_usb_device_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    return;
  }

  /* Idle. USBX runs the SCSI/BBB state machine on its own threads. */
  while (1) {
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops
 * ::selftest_host_pass with a retry pause until the whole config A
 * ladder passes; afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread (lower priority than
 *      the USBX device-side threads).
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Polled host stack: blocking calls, ms timeouts via ra_time.
 * @since 0.1.0
 */
static VOID selftest_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_selftest_boot_wait_ticks);
  for (;;) {
    const ra_err_t err = selftest_host_pass();
    if (err == k_ra_ok) {
      break;
    }
    tx_thread_sleep(k_selftest_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Device worker at priority 8 (above USBX class threads'
 * default), host worker at 16 so the polled host loop can never starve
 * the IRQ-driven device side.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @post Two auto-start worker threads are queued.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true; /* ThreadX timer state is initialized past here. */
  (void)tx_thread_create(&s_device_thread,
                         "selftest_device",
                         selftest_device_worker,
                         0UL,
                         s_device_stack,
                         k_selftest_thread_stack,
                         (UINT)k_selftest_dev_priority,
                         (UINT)k_selftest_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "selftest_host",
                         selftest_host_worker,
                         0UL,
                         s_host_stack,
                         k_selftest_host_stack,
                         (UINT)k_selftest_host_priority,
                         (UINT)k_selftest_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal error in boot.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void selftest_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense (PSEL), P5_00 VBUSEN held LOW as
 * GPIO (peripheral routing would force host-style VBUSEN HIGH and block
 * device enumeration), P8_14/P8_15 data. HS host: SW4-8 to Host via the
 * U15 expander, PD07 HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::selftest_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void selftest_route_usb_or_halt(void)
{
  /* FS port: device role. */
  if (ra_pfs_route_peripheral(k_selftest_pin_fs_vbus, k_ra_psel_usb_fs, "selftest.fs_vbus") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_gpio_output_init(k_selftest_pin_fs_vbusen, k_ra_level_low) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_selftest_pin_fs_dp, k_ra_psel_usb_fs, "selftest.fs_dp") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_selftest_pin_fs_dm, k_ra_psel_usb_fs, "selftest.fs_dm") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
  /* HS port: host role. */
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_gpio_output_init(k_selftest_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_selftest_pin_hs_vbus, k_ra_psel_usb_hs, "selftest.hs_vbus") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference before MSTPB11 is
 * released; USBHS needs its 60 MHz UTMI PLL. SCI8 is the J-Link OB CDC
 * console at 115200.
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called exactly once from main.
 * @since 0.1.0
 */
static void selftest_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_selftest_pin_sci_tx, k_ra_psel_sci_async, "selftest.txd8") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_selftest_pin_sci_rx, k_ra_psel_sci_async, "selftest.rxd8") !=
      k_ra_ok) {
    selftest_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_selftest_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_selftest_sci_channel, &sci_cfg) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    selftest_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    selftest_panic_halt();
  }
  selftest_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the two workers only deal with stack bring-up.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  selftest_setup_or_halt();

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  selftest_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
