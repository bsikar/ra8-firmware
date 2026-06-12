/**
 * @file examples/ek_ra8d2/usb_msc_mram_hs/main.c
 * @brief ThreadX + USBX Mass-Storage view of the onboard MRAM (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual to the on-board USB-FS receptacle, hands
 * control to ThreadX, and brings the Mass-Storage device class up via
 * Eclipse USBX (``_ux_device_class_storage_initialize``). The class
 * sits on top of the project's ``port/usbx/ux_dcd_ra_usb`` bridge to
 * the hand-written ``ra_usb`` register-level driver (HUM Ch. 36
 * USBFS, sec. 36.2.x for SYSCFG / DCPCFG / DCPMAXP / PIPECFG /
 * CFIFO). The host actually enumerates the device because USBX's
 * chapter-9 + SCSI/BBB state machines answer SETUP and BOT packets
 * through the DCD bridge.
 *
 * The single LUN is a READ-ONLY synthesized FAT16 volume: the boot
 * sector, FAT, and root directory are generated on the fly, and the
 * data clusters map 1:1 onto the chip's 1 MiB MRAM window at
 * 0x02000000, exposed as a single root file ``MRAM.BIN``. Plug the
 * board into a computer and the onboard flash shows up as a file you
 * can open/copy; comparing it against a debugger dump of the same
 * window proves the bytes travel USB end to end. Writes are rejected
 * with DATA PROTECT sense and the LUN reports write-protected via
 * MODE SENSE, so hosts mount it read-only.
 *
 * ## Pinout (USB-HS, mirrors tz_secure_only_usb_hs)
 *
 * P4_08 = USBHS_VBUS sense (PSEL ``k_ra_psel_usb_hs``; the only
 * PFS-muxed HS pin -- D+/D- are dedicated PHY balls). PD07 = J7 role
 * select, driven LOW for Device mode (EK-RA8D2 v1 UM Sec 6.2 p 34).
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates
 * as a USB Mass-Storage device. ``system_profiler SPUSBDataType``
 * lists it under "USB Bus" with class Mass Storage; Disk Utility
 * shows an unformatted 4 KiB volume.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra_time millisecond
 * counter (for ra_delay_ms) AND ThreadX's timer (for tx_thread_sleep and
 * semaphore timeouts). The default weak ra_time SysTick handler only advances
 * the ms counter; without _tx_timer_interrupt ThreadX time never advances and
 * tx_thread_sleep / USBX class-thread scheduling stall. The project's
 * tx_initialize_low_level.S configures SysTick but relies on the application
 * to publish the handler. */
extern void ra_time_on_tick(void);
extern void _tx_timer_interrupt(void);
void        SysTick_Handler(void);
void        SysTick_Handler(void)
{
  ra_time_on_tick();
  _tx_timer_interrupt();
  /* Re-enable the USB IRQ at the NVIC level: the bridge's storm guard
   * masks it to break the USBFS event-less interrupt storm, and this
   * 1 ms pulse is its recovery clock -- a masked line is re-enabled
   * within one period so real USB events are never lost. */
  ux_dcd_ra_usb_irq_reenable();
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB-FS pin identifiers, packed ``ra_port_pin_t`` (port << 8 | pin).
 * @details Built as a runtime cast so clang-tidy's enum-range check
 * is happy with the otherwise out-of-enum value.
 * @since 0.1.0
 */
static const ra_port_pin_t k_demo_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 role-select strap (PD07): LOW = Device (UM Sec 6.2 p 34). */
static const ra_port_pin_t k_demo_pin_hs_role =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the worker thread + USBX pool +
 *        RAM-disk geometry.
 */
/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range. */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none. */
} scsi_sense_code_t;

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
  k_mram_base_addr = 0x02000000U, /**< MRAM code window base address.   */
  k_mram_bytes     = 0x00100000U, /**< 1 MiB window size.               */
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
} demo_fat_geom_t;

/**
 * @enum demo_fat_boot_t
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
  k_boot_volume_id   = 0x52A8D200U, /**< Arbitrary volume serial.     */
  k_boot_sig_lo      = 0x55U,       /**< Boot signature low byte.     */
  k_boot_sig_hi      = 0xAAU,       /**< Boot signature high byte.    */
  k_boot_sig_lo_off  = 510U,        /**< Signature low-byte offset.   */
  k_boot_sig_hi_off  = 511U,        /**< Signature high-byte offset.  */
} demo_fat_boot_t;

/**
 * @enum demo_fat_off_t
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
} demo_fat_off_t;

/**
 * @enum demo_word_pack_t
 * @brief 32-bit little-endian split constants.
 */
typedef enum : uint32_t {
  k_word_shift = 16U,     /**< Bits per half-word.   */
  k_word_mask  = 0xFFFFU, /**< Low half-word mask.   */
} demo_word_pack_t;

/** @brief SCSI sense triple for a write to the protected medium. */
typedef enum : uint8_t {
  k_scsi_sense_data_protect  = 0x07U, /**< Sense key: DATA PROTECT.     */
  k_scsi_asc_write_protected = 0x27U, /**< ASC: WRITE PROTECTED.       */
} scsi_wp_sense_t;

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage                                         */
/* -------------------------------------------------------------------------- */

/**
 * @var s_demo_thread
 * @brief ThreadX TCB for the USBX worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_demo_thread;

/**
 * @var s_demo_stack
 * @brief Stack backing storage for ``s_demo_thread``.
 * @since 0.1.0
 */
static UCHAR s_demo_stack[k_demo_thread_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @details Sized larger than CDC/HID because the storage class
 *          allocates 2 x bulk endpoint buffers (~64 KiB-shaped reads).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_demo_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "MRAM 1MiB (RO)  ";
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
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints)              */
/* -------------------------------------------------------------------------- */

/* Single-interface MSC config: bulk-only transport, SCSI command set.
 * Class = 0x08 (Mass Storage), SubClass = 0x06 (SCSI transparent),
 * Protocol = 0x50 (BBB / Bulk-Only). EP1 IN + EP2 OUT, 64-byte MPS.
 *
 * Layout per USB Mass Storage Class Bulk-Only Transport (BBB) rev 1.0
 * sec 4 + USB 2.0 sec 9.6.
 *
 * Total config-blob length:
 *   9 (config) + 9 (interface) + 7 (EP IN) + 7 (EP OUT) = 32 bytes.
 */
/* High-speed framework: identical layout plus the mandatory Device
 * Qualifier descriptor (USB 2.0 sec 9.6.2) and 512-byte bulk
 * wMaxPacketSize values, which the USB 2.0 spec mandates for HS bulk
 * endpoints. */
static UCHAR s_device_framework_hs[] = {
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
  0x0DU, /* PID = 0x000D (pid.codes test).    */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Device Qualifier descriptor (USB 2.0 sec 9.6.2) -- 10 bytes:
   * the device's capabilities at the OTHER speed (FS). */
  0x0AU,
  0x06U,
  0x00U,
  0x02U,
  0x00U,
  0x00U,
  0x00U,
  0x40U,
  0x01U,
  0x00U,
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
  /* Bulk-IN endpoint (EP1 IN, 512-byte MPS at HS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x00U,
  0x02U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 512-byte MPS at HS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x00U,
  0x02U,
  0x00U,
};

static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes.
   * bcdUSB = 0x0200 (USB 2.0); hosts may reject USB 1.1 for
   * modern composite/class drivers. */
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
  0x0DU, /* PID = 0x000D (pid.codes test).    */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (32 bytes total).
   * bmAttributes = 0x80 (bus-powered); 0xC0 (self-powered)
   * with bMaxPower=100mA is self-contradictory. */
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
  /* idx 2: "EK-RA8D2 MRAM HS". */
  0x09U,
  0x04U,
  0x02U,
  0x10U,
  'E',
  'K',
  '-',
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'M',
  'R',
  'A',
  'M',
  ' ',
  'H',
  'S',
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
  '3',
};

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (read / write / status)                      */
/* -------------------------------------------------------------------------- */

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
static void demo_fat_fill_sector(uint32_t lba, UCHAR* out)
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

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
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
 *
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` and ``media_status`` are non-NULL (USBX
 *      guarantee).
 * @pre ``lun`` is 0 (single-LUN device).
 * @post Either ``number_blocks * 512`` bytes were synthesized or
 *       ``media_status`` is non-zero.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
/** @brief JLink-readable read-path probes (diagnostic only). */
static volatile uint32_t s_dbg_err_level;   /**< Last USBX error level.    */
static volatile uint32_t s_dbg_err_ctx;     /**< Last USBX error context.  */
static volatile uint32_t s_dbg_err_code;    /**< Last USBX error code.     */
static volatile uint32_t s_dbg_err_count;   /**< USBX error callback hits. */
static volatile uint32_t s_dbg_dev_state;   /**< USBX device state mirror. */
static volatile uint32_t s_dbg_ux_speed;    /**< USBX negotiated speed.    */
static volatile uint32_t s_dbg_class_inst;  /**< Class[0] instance ptr.    */
static volatile uint32_t s_dbg_framework;   /**< Active framework pointer. */
static volatile uint32_t s_dbg_fw_len;      /**< Active framework length.  */
static volatile uint32_t s_dbg_thr_state;   /**< Storage thread TX state.  */
static volatile uint32_t s_dbg_thr_runs;    /**< Storage thread run count. */
static volatile uint32_t s_dbg_kicks;       /**< Thread-context re-resumes. */
static volatile uint32_t s_dbg_state3_seen; /**< CONFIGURED sightings (1ms). */
static volatile uint32_t s_dbg_activates;   /**< Class activate calls.     */
static volatile uint32_t s_dbg_deactivates; /**< Class deactivate calls.   */

/**
 * @brief Storage class activate callback: count activations.
 *
 * @param[in] inst Class instance pointer (unused).
 *
 * @pre Registered through the storage class parameter block.
 * @pre Called by USBX on SET_CONFIGURATION.
 * @post ::s_dbg_activates incremented.
 * @post No other state changes.
 *
 * @note Diagnostic only.
 * @since 0.1.0
 */
static VOID demo_msc_activate(VOID* inst)
{
  (void)inst;
  s_dbg_activates++;
}

/**
 * @brief Storage class deactivate callback: count deactivations.
 *
 * @param[in] inst Class instance pointer (unused).
 *
 * @pre Registered through the storage class parameter block.
 * @pre Called by USBX on reset / reconfiguration.
 * @post ::s_dbg_deactivates incremented.
 * @post No other state changes.
 *
 * @note Diagnostic only.
 * @since 0.1.0
 */
static VOID demo_msc_deactivate(VOID* inst)
{
  (void)inst;
  s_dbg_deactivates++;
}
static volatile uint32_t s_dbg_read_calls;    /**< media_read invocations.   */
static volatile uint32_t s_dbg_read_last_lba; /**< Most recent starting LBA. */
static volatile uint32_t s_dbg_read_last_n;   /**< Most recent block count.  */

static UINT demo_msc_read(VOID*  storage,
                          ULONG  lun,
                          UCHAR* data_pointer,
                          ULONG  number_blocks,
                          ULONG  lba,
                          ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  s_dbg_read_last_lba = (uint32_t)lba;
  s_dbg_read_last_n   = (uint32_t)number_blocks;
  if ((lba + number_blocks) > (ULONG)k_fat_total_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  for (ULONG i = 0UL; i < number_blocks; i++) {
    demo_fat_fill_sector((uint32_t)(lba + i), &data_pointer[i * (ULONG)k_demo_block_size]);
  }
  *media_status = 0UL;
  (void)ra_board_led_toggle(k_ra_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: always rejects (write-protected).
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
static UINT demo_msc_write(VOID*  storage,
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
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     media_id     Media id (unused).
 * @param[out]    media_status Filled with 0 (no fault).
 *
 * @return Always ``UX_SUCCESS``.
 * @retval UX_SUCCESS Media is present and ready.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @post ``*media_status`` is 0.
 *
 * @note RAM-disk is always present; never reports media-not-present.
 * @since 0.1.0
 */
static UINT demo_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + run the storage class                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + FS device stack up.
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
/**
 * @brief USBX error callback: mirror the last error into JLink probes.
 *
 * @param[in] system_level   USBX error level.
 * @param[in] system_context USBX error context.
 * @param[in] error_code     USBX error code.
 *
 * @pre Registered via ``_ux_utility_error_callback_register``.
 * @pre Any USBX context (thread or ISR).
 * @post The three probe words + hit counter reflect the last error.
 * @post No other state changes.
 *
 * @note Diagnostic only; never read by production code.
 * @since 0.1.0
 */
static VOID demo_usbx_error_cb(UINT system_level, UINT system_context, UINT error_code)
{
  s_dbg_err_level = (uint32_t)system_level;
  s_dbg_err_ctx   = (uint32_t)system_context;
  s_dbg_err_code  = (uint32_t)error_code;
  s_dbg_err_count++;
}

static UINT demo_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  _ux_utility_error_callback_register(demo_usbx_error_cb);
  return _ux_device_stack_initialize(s_device_framework_hs,
                                     sizeof(s_device_framework_hs),
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
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ``demo_usbx_stack_up`` has succeeded.
 * @pre Media read/write/status callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0.
 * @post LUN0 advertises the read-only synthesized FAT16 volume.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT demo_msc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_instance_activate     = demo_msc_activate;
  msc_params.ux_slave_class_storage_instance_deactivate   = demo_msc_deactivate;
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)k_fat_total_sectors - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_demo_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    demo_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    demo_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    demo_msc_status;

  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_storage",
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

static VOID demo_worker(ULONG arg)
{
  (void)arg;

  if (demo_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (demo_msc_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_hs) != k_ra_ok) {
    return;
  }
  if (ra_usb_device_attach(k_ra_usb_speed_hs, true) != k_ra_ok) {
    return;
  }

  /* Idle. USBX runs the SCSI/BBB state machine on its own threads. */
  while (1) {
    s_dbg_dev_state = (uint32_t)_ux_system_slave->ux_system_slave_device.ux_slave_device_state;
    s_dbg_ux_speed  = (uint32_t)_ux_system_slave->ux_system_slave_speed;
    s_dbg_class_inst =
      (uint32_t)(uintptr_t)_ux_system_slave->ux_system_slave_class_array[0].ux_slave_class_instance;
    s_dbg_framework = (uint32_t)(uintptr_t)_ux_system_slave->ux_system_slave_device_framework;
    s_dbg_fw_len    = (uint32_t)_ux_system_slave->ux_system_slave_device_framework_length;
    s_dbg_thr_state = (uint32_t)_ux_system_slave->ux_system_slave_class_array[0]
                        .ux_slave_class_thread.tx_thread_state;
    s_dbg_thr_runs  = (uint32_t)_ux_system_slave->ux_system_slave_class_array[0]
                        .ux_slave_class_thread.tx_thread_run_count;
    /* Diagnostic kick: if the class thread is parked SUSPENDED while the
     * device is CONFIGURED, the activate-time resume (ISR context) did
     * not take; re-resume from thread context and count it. */
    if (s_dbg_dev_state == (uint32_t)UX_DEVICE_CONFIGURED) {
      s_dbg_state3_seen++;
      if (s_dbg_thr_state == (uint32_t)TX_SUSPENDED) {
        (void)tx_thread_resume(
          &_ux_system_slave->ux_system_slave_class_array[0].ux_slave_class_thread);
        s_dbg_kicks++;
      }
    }
    tx_thread_sleep(1U);
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief ThreadX application-define hook. Spawns the demo worker.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @post One auto-start worker thread is queued.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         "usb_msc_mram_hs",
                         demo_worker,
                         0UL,
                         s_demo_stack,
                         k_demo_thread_stack,
                         8U, /* priority         */
                         8U, /* preempt threshold */
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
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the USB-HS VBUS pin + drive the J7 role strap LOW.
 *
 * @return Error from the first failing route call, or k_ra_ok.
 * @retval k_ra_ok VBUS routed and PD07 driven LOW (device role).
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success P4_08 carries USBHS_VBUS and PD07 is LOW.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_demo_pin_hs_vbus, k_ra_psel_usb_hs, "usb_msc_mram_hs.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  /* PD07 (J7 USB-HS role select, UM 6.2 p 34): drive LOW for Device
   * mode so U18 does not back-feed VBUS into the host's cable. D+/D-
   * are dedicated HS PHY balls -- no PFS routing needed. */
  return ra_gpio_output_init(k_demo_pin_hs_role, k_ra_level_low);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS pins + LED1 + ThreadX.
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
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }
  /* Bring up the USBHS 60 MHz PLL reference before MSTPB12 is
   * released inside ux_dcd_ra_usb_initialize -- without it the UTMI
   * PHY never locks and the host never sees the attach. */
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
