/**
 * @file examples/ek_ra8d2/hw_pending/usb_selftest_ospi_rw/main.c
 * @brief USB self-loop: HS host WRITE(10)s + reads back the onboard OSPI flash
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The WRITABLE OSPI MSC self-loop -- it exercises the host->device
 * bulk-OUT data path (SCSI WRITE(10)) landing on real non-volatile
 * storage. The two USB ports are cabled to EACH OTHER and one firmware
 * image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class that
 *    exposes one WRITABLE logical unit (``GET_MAX_LUN`` = 0) backed by a
 *    64-sector (32 KiB) window of the onboard OSPI flash (IS25LX512M at
 *    xSPI CS1, offset 0x00200000). media_write programs host data into the
 *    flash window; media_read serves it back. IRQ-driven through the
 *    `port/usbx/ux_dcd_ra_usb` bridge.
 *  - USBHS (J7) = HOST: the first-party polled host MSC stack
 *    (`ra_usb_hmsc`). It enumerates the device, WRITE(10)s a
 *    deterministic per-LBA pattern across the whole window, then READ(10)s
 *    it back and byte-checks every sector -- proving the device bulk-OUT
 *    WRITE data phase round-trips intact onto flash, end to end on chip.
 *
 * No filesystem is involved (raw SCSI WRITE(10)/READ(10)); this is the
 * non-volatile write-path counterpart to the read-only MSC self-loops,
 * and the on-bench validation for the device bulk-OUT WRITE(10) driver
 * fix against persistent storage. The window (0x00200000) is a scratch
 * region clear of flash_journal (offset 0) and the read-only OSPI image
 * (offset 0x00100000); it is erased then rewritten every run.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
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
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_hmsc.h"
#include "ra_xspi.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra_time millisecond
 * counter (for ra_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */
extern void ra_time_on_tick(void);
extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the setup
 *          window is long (U15 expander I2C blocks for ms), so the tick
 *          fires pre-kernel; feeding _tx_timer_interrupt into ThreadX's
 *          zeroed timer state bus-faults. Gate it until the kernel runs.
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
static const ra_port_pin_t k_ospirw_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra_port_pin_t k_ospirw_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USBFS D+ (P8_14). */
static const ra_port_pin_t k_ospirw_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USBFS D- (P8_15). */
static const ra_port_pin_t k_ospirw_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_ospirw_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra_port_pin_t k_ospirw_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_ospirw_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_ospirw_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum ospirw_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_ospirw_thread_stack    = 4096U,   /**< Device worker stack (bytes).      */
  k_ospirw_host_stack      = 8192U,   /**< Host worker stack (bytes).        */
  k_ospirw_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).         */
  k_ospirw_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).     */
  k_ospirw_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).    */
  k_ospirw_retry_ticks     = 3000U,   /**< Pause between ladder retries.     */
  k_ospirw_baud            = 115200U, /**< J-Link OB CDC log baud.           */
  k_ospirw_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.     */
  k_ospirw_print_cap       = 160U,    /**< Bound for console-string scans.   */
  k_ospirw_dev_priority    = 8U,      /**< Device bring-up worker priority.  */
  k_ospirw_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} ospirw_config_t;

/**
 * @enum ospirw_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_ospirw_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".         */
  k_ospirw_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_ospirw_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count.  */
  k_ospirw_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_ospirw_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
} ospirw_hex_t;

/**
 * @enum ospirw_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_ospirw_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_ospirw_dec_radix   = 10U,  /**< Base for decimal conversion. */
} ospirw_mask_t;

/**
 * @enum ospirw_geom_t
 * @brief LUN geometry + verification constants.
 */
typedef enum : uint32_t {
  k_ospirw_count              = 1U,          /**< Logical units exposed (single writable). */
  k_ospirw_sectors            = 64U,         /**< 512-byte sectors (OSPI window = 32 KiB). */
  k_ospirw_block_size         = 512U,        /**< SCSI logical block size.       */
  k_ospirw_burst_blocks       = 8U,          /**< Blocks per READ(10) burst.     */
  k_ospirw_burst_bytes        = 4096U,       /**< 8 x 512 B burst buffer.        */
  k_ospirw_target_lun0        = 0U,          /**< First LUN index.               */
  k_ospirw_no_mismatch        = 0xFFFFFFFFU, /**< Probe: no mismatch.            */
  k_ospirw_pat_lun_mul        = 97U,         /**< Per-LUN pattern multiplier.    */
  k_ospirw_pat_lba_mul        = 7U,          /**< Per-LBA pattern multiplier.    */
  k_ospirw_pat_bias           = 0x5AU,       /**< Pattern constant bias.         */
  k_ospirw_byte_mask          = 0xFFU,       /**< Byte mask.                     */
  k_ospirw_mismatch_lun_shift = 24U,         /**< s_dbg_mismatch: LUN in bits 31:24. */
  k_ospirw_instance           = 0U,          /**< xSPI controller instance.      */
  k_ospirw_offset             = 0x00200000U, /**< 2 MiB into the chip (scratch). */
  k_ospirw_erase_sector       = 0x00001000U, /**< IS25LX512M 4 KiB erase sector. */
} ospirw_geom_t;

/**
 * @enum ospirw_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_ospirw_phase_boot   = 0U, /**< Host thread not started.   */
  k_ospirw_phase_init   = 1U, /**< ra_usb_hmsc_init issued.   */
  k_ospirw_phase_enum   = 2U, /**< Enumerating.               */
  k_ospirw_phase_verify = 3U, /**< Reading + checking LUNs.   */
  k_ospirw_phase_pass   = 4U, /**< All LUNs verified.         */
} ospirw_phase_t;

/**
 * @enum ospirw_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_ospirw_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_ospirw_dev_step_class  = 2U, /**< MSC class registered.          */
  k_ospirw_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_ospirw_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_ospirw_dev_step_parked = 5U, /**< Bring-up done; worker parked.  */
} ospirw_dev_step_t;

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.      */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                 */
  k_scsi_sense_data_protect    = 0x07U, /**< Sense key: DATA PROTECT.    */
  k_scsi_asc_write_protected   = 0x27U, /**< ASC: WRITE PROTECTED.       */
} scsi_sense_code_t;

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
static UCHAR s_device_stack[k_ospirw_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread.
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_ospirw_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_ospirw_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "WRITABLE OSPI RW";
static UCHAR s_msc_product_rev[] = "0001";

/* -------------------------------------------------------------------------- */
/* J-Link probes                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::ospirw_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Sectors that read back correctly after the write pass. */
static volatile uint32_t s_dbg_luns_ok;
/** @brief Device-reported GET_MAX_LUN value (expect 0, single LUN). */
static volatile uint32_t s_dbg_max_lun;
/** @brief First mismatching sector, or ::k_ospirw_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_ospirw_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Device-side media_read invocations. */
static volatile uint32_t s_dbg_read_calls;
/** @brief Device-side media_write invocations (WRITE(10) data received). */
static volatile uint32_t s_dbg_write_calls;
/** @brief Total 512-B blocks the device media_write callback stored. */
static volatile uint32_t s_dbg_write_blocks;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 parked. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/** @brief OSPI bring-up + window pre-erase result (0 = ready, else error). */
static volatile uint32_t s_dbg_ospi_prov = (uint32_t)k_ospirw_no_mismatch;
/** @brief OSPI JEDEC id read back at boot (sanity probe). */
static volatile uint32_t s_dbg_ospi_id;

/* -------------------------------------------------------------------------- */
/* USB descriptors (single-interface MSC; one writable logical unit)         */
/* -------------------------------------------------------------------------- */

/* MSC config: bulk-only transport, SCSI command set, EP1 IN + EP2 OUT,
 * 64-byte MPS. The number of LUNs is a class-registration parameter, not
 * a descriptor field, so this framework is the standard single-interface
 * MSC blob. PID 0x0016 marks the writable-OSPI self-test identity. */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U,
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x16U, /* PID = 0x0016 (pid.codes test).    */
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
  /* idx 2: "RA8D2 MULTILUN". */
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
  'M',
  'U',
  'L',
  'T',
  'I',
  'L',
  'U',
  'N',
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
  '1',
  '3',
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
/* Shared per-(LUN,LBA) pattern                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill one 512-byte sector with this LUN/LBA's deterministic bytes.
 *
 * @details Byte i = ``(lun*97 + lba*7 + i + 0x5A) & 0xFF``. Distinct per
 * LUN and per LBA so the host can prove it addressed the right logical
 * unit and sector. The device media-read and the host verifier compute
 * it identically.
 *
 * @param[in]  lun The logical unit (0..1).
 * @param[in]  lba The logical block address within the LUN.
 * @param[out] out 512-byte destination buffer.
 *
 * @pre @p out has ::k_ospirw_block_size writable bytes.
 * @pre @p lun and @p lba are within the exposed geometry.
 * @post @p out holds the sector's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void ospirw_pattern_fill(uint32_t lun, uint32_t lba, UCHAR* out)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ospirw_block_size; i++) {
    const uint32_t v = (lun * (uint32_t)k_ospirw_pat_lun_mul) +
                       (lba * (uint32_t)k_ospirw_pat_lba_mul) + i + (uint32_t)k_ospirw_pat_bias;
    out[i]           = (UCHAR)(v & (uint32_t)k_ospirw_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (single writable OSPI-flash LUN)             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: read the OSPI flash window back.
 *
 * @details Bound-checks the request against the LUN geometry, then reads
 * @p number_blocks sectors from the OSPI window at ::k_ospirw_offset with
 * ::ra_xspi_flash_read into the USBX buffer. One callback serves the
 * single LUN. LED1 toggles per call so loop traffic is visible.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (0..1).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the LUN; else ``UX_ERROR``.
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre @p lun is below ::k_ospirw_count.
 * @post Either the blocks were synthesized or media_status is non-zero.
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT ospirw_msc_read(VOID*  storage,
                            ULONG  lun,
                            UCHAR* data_pointer,
                            ULONG  number_blocks,
                            ULONG  lba,
                            ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_ospirw_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  const uint32_t addr = (uint32_t)k_ospirw_offset + ((uint32_t)lba * (uint32_t)k_ospirw_block_size);
  const ra_err_t rerr = ra_xspi_flash_read((uint8_t)k_ospirw_instance,
                                           addr,
                                           data_pointer,
                                           (uint32_t)number_blocks * (uint32_t)k_ospirw_block_size);
  if (rerr != k_ra_ok) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  *media_status = 0UL;
  (void)ra_board_led_toggle(k_ra_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: program host data into OSPI flash.
 *
 * @details The LUN is writable; the host's WRITE(10) data-OUT phase lands
 * here. Programs @p number_blocks sectors from the USBX buffer into the
 * OSPI window at ::k_ospirw_offset with ::ra_xspi_flash_program. The
 * window is ERASED once at boot, so this is a fast program-only path (no
 * per-write 4 KiB erase, which would exceed the host's BOT timeout).
 * Bumps ::s_dbg_write_calls / ::s_dbg_write_blocks.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused; single LUN).
 * @param[in]     data_pointer USBX-owned source buffer (received OUT data).
 * @param[in]     number_blocks Number of 512-byte blocks the host wrote.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with the sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the window; else ``UX_ERROR``.
 * @retval UX_SUCCESS Blocks programmed into the OSPI window.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` / ``media_status`` are non-NULL (USBX guarantee).
 * @pre The OUT data phase delivered ``number_blocks * 512`` bytes.
 * @post The OSPI window holds the written sectors; counters advanced.
 * @post ``*media_status`` is 0 on success.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [constParameterCallback] -- USBX's
 * ux_slave_class_storage_media_write function-pointer signature takes
 * non-const UCHAR*; we cannot const-qualify the parameter. */
static UINT ospirw_msc_write(VOID*  storage,
                             ULONG  lun,
                             UCHAR* data_pointer,
                             ULONG  number_blocks,
                             ULONG  lba,
                             ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_write_calls++;
  if ((lba + number_blocks) > (ULONG)k_ospirw_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  const uint32_t addr = (uint32_t)k_ospirw_offset + ((uint32_t)lba * (uint32_t)k_ospirw_block_size);
  const ra_err_t werr =
    ra_xspi_flash_program((uint8_t)k_ospirw_instance,
                          addr,
                          data_pointer,
                          (uint32_t)number_blocks * (uint32_t)k_ospirw_block_size);
  if (werr != k_ra_ok) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  s_dbg_write_blocks += (uint32_t)number_blocks;
  *media_status = 0UL;
  (void)ra_board_led_toggle(k_ra_board_led2);
  return UX_SUCCESS;
}
/* cppcheck-suppress-end [constParameterCallback] */

/**
 * @brief Storage media-status callback. Always reports media-present.
 *
 * @details Synthesized LUNs never go absent; status is constant 0.
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
 * @note Synthesized volumes; never report media-not-present.
 * @since 0.1.0
 */
static UINT ospirw_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
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
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t ospirw_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_ospirw_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_ospirw_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_ospirw_print_cap).
 *
 * @details Linear scan with a hard upper bound.
 *
 * @param[in] text NUL-terminated string.
 *
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the length.
 * @post No state changes.
 * @post Return value never exceeds ::k_ospirw_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t ospirw_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_ospirw_print_cap) {
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
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_ospirw_sci_channel, data, len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::ospirw_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_ospirw_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_print(const char* text)
{
  return ospirw_sci_write((const uint8_t*)text, ospirw_str_len(text));
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
[[nodiscard]] static ra_err_t ospirw_print_dec(uint32_t value)
{
  uint8_t  scratch[k_ospirw_dec_chars_u32] = {};
  uint8_t  out[k_ospirw_dec_chars_u32]     = {};
  uint8_t  count                           = 0U;
  uint32_t v                               = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return ospirw_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_ospirw_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_ospirw_dec_radix));
    v              = v / k_ospirw_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return ospirw_sci_write(out, (uint32_t)count);
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
 * @pre @p digits is at most ::k_ospirw_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_ospirw_hex_chars_u32] = {};
  uint8_t width                       = digits;
  if (width > (uint8_t)k_ospirw_hex_chars_u32) {
    width = (uint8_t)k_ospirw_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_ospirw_nibble_bits);
    out[i]              = ospirw_nibble_to_hex((value >> shift) & k_ospirw_nibble_mask);
  }
  return ospirw_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; first failing chunk's code returned.
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
[[nodiscard]] static ra_err_t ospirw_print_fail(const char* what, ra_err_t err)
{
  ra_err_t e = ospirw_print("ra8d2 ospirw: FAIL ");
  if (e != k_ra_ok) {
    return e;
  }
  e = ospirw_print(what);
  if (e != k_ra_ok) {
    return e;
  }
  e = ospirw_print(" err=0x");
  if (e != k_ra_ok) {
    return e;
  }
  e = ospirw_print_hex((uint32_t)err, (uint8_t)k_ospirw_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return ospirw_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX MSC with two LUNs                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring USBX system + device stack up with the MSC framework.
 *
 * @details One-shot USBX pool + device-stack init (FS-only framework).
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT ospirw_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_ospirw_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief Populate the writable LUN parameter slot with geometry + callbacks.
 *
 * @details The LUN is a writable FAT-disk-typed,
 * removable, 64-sector OSPI-backed volume whose media callbacks program
 * and read the OSPI flash window.
 *
 * @param[in,out] p   The class parameter block.
 * @param[in]     idx LUN slot index (0..1).
 *
 * @pre @p p is zeroed and being filled before class register.
 * @pre @p idx is below ::k_ospirw_count.
 * @post Slot @p idx carries the geometry + media callbacks.
 * @post No other slot is touched.
 *
 * @note Helper to keep the register function within the size cap.
 * @since 0.1.0
 */
static void ospirw_fill_lun(UX_SLAVE_CLASS_STORAGE_PARAMETER* p, uint32_t idx)
{
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_last_lba =
    (ULONG)k_ospirw_sectors - 1UL;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_block_length =
    (ULONG)k_ospirw_block_size;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read_only_flag =
    UX_FALSE;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_read = ospirw_msc_read;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_write =
    ospirw_msc_write;
  p->ux_slave_class_storage_parameter_lun[idx].ux_slave_class_storage_media_status =
    ospirw_msc_status;
}

/**
 * @brief Register the Mass-Storage class with one writable LUN.
 *
 * @details Sets ``number_lun`` = 1 and fills the LUN slot via
 * ::ospirw_fill_lun, so the host's GET_MAX_LUN returns 0. One is the
 * single writable unit this self-loop exercises.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::ospirw_usbx_stack_up has succeeded.
 * @pre Media callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0, with 1 LUN.
 * @post GET_MAX_LUN will report 0.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT ospirw_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = (ULONG)k_ospirw_count;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;
  for (uint32_t idx = 0U; idx < (uint32_t)k_ospirw_count; idx++) {
    ospirw_fill_lun(&msc_params, idx);
  }
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_storage",
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Bring the OSPI flash up and erase the writable window.
 *
 * @details Mirrors the read-only OSPI self-loop's bring-up: U15 expander
 * courtesy write, ``ra_board_xspi_pins_init`` (OCTA pins + RESET pulse),
 * ``ra_xspi_init`` 1S-1S-1S, JEDEC-id readback (::s_dbg_ospi_id), then a
 * one-shot erase of the ::k_ospirw_sectors window at ::k_ospirw_offset so
 * the per-write path is fast program-only. The host owns the data.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok OSPI is up and the write window is erased.
 * @retval (other) A pins / init / erase step failed.
 *
 * @pre CGC is initialized (main ran ra_cgc_init).
 * @pre Single caller (the device worker, before USB attach).
 * @post ::s_dbg_ospi_id / ::s_dbg_ospi_prov reflect the outcome.
 * @post The 32 KiB OSPI window is erased (0xFF) on success.
 *
 * @note Blocking; runs once at boot before USB attach.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_ospi_provision(void)
{
  (void)ra_board_io_expander_set_octospi_active();
  ra_err_t err = ra_board_xspi_pins_init();
  if (err != k_ra_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  err = ra_xspi_init((uint8_t)k_ospirw_instance, k_ra_xspi_lio_1s1s1s);
  if (err != k_ra_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  uint32_t id = 0U;
  (void)ra_xspi_flash_read_id((uint8_t)k_ospirw_instance, &id);
  s_dbg_ospi_id = id;
  const uint32_t windows =
    ((uint32_t)k_ospirw_sectors * (uint32_t)k_ospirw_block_size) / (uint32_t)k_ospirw_erase_sector;
  for (uint32_t e = 0U; e < windows; e++) {
    const uint32_t addr = (uint32_t)k_ospirw_offset + (e * (uint32_t)k_ospirw_erase_sector);
    err                 = ra_xspi_flash_erase_sector((uint8_t)k_ospirw_instance, addr);
    if (err != k_ra_ok) {
      s_dbg_ospi_prov = (uint32_t)err;
      return err;
    }
  }
  s_dbg_ospi_prov = 0U;
  return k_ra_ok;
}

/**
 * @brief Device-side worker: bring OSPI + the writable device up, then park.
 *
 * @details Provisions the OSPI write window (::ospirw_ospi_provision), then
 * USBX system + device stack + writable MSC class + DCD bridge on the
 * USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB state
 * machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post OSPI window erased, FS device attached + serviceable on its LUN.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID ospirw_device_worker(ULONG arg)
{
  (void)arg;

  if (ospirw_ospi_provision() != k_ra_ok) {
    return; /* s_dbg_ospi_prov records the failure; host READ/WRITE will FAIL */
  }
  UINT ux = ospirw_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_ospirw_dev_step_stack;
  ux             = ospirw_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_ospirw_dev_step_class;
  ra_err_t e     = ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs);
  if (e != k_ra_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_ospirw_dev_step_dcd;
  e              = ra_usb_device_attach(k_ra_usb_speed_fs, true);
  if (e != k_ra_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_ospirw_dev_step_attach;

  while (1) {
    s_dbg_dev_step = (uint32_t)k_ospirw_dev_step_parked;
    tx_thread_sleep(k_ospirw_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* Host side: ra_usb_hmsc enumerate + WRITE(10) then read-verify              */
/* -------------------------------------------------------------------------- */

/**
 * @brief WRITE(10) the per-LBA pattern across the whole OSPI window.
 *
 * @details Fills an 8-block burst from ::ospirw_pattern_fill and pushes it
 * with raw ::ra_usb_hmsc_write10 until every sector is written. This is
 * the host data-OUT phase that drives the device bulk-OUT receive path
 * (the gating mechanism for every writable / repeated-bulk matrix item).
 *
 * @param[in] lun Logical unit to write (0).
 *
 * @return ra_err_t verdict.
 * @retval k_ra_ok The whole window was written.
 * @retval k_ra_err_hw_timeout A WRITE(10) data-OUT phase did not complete.
 *
 * @pre The host has enumerated the device.
 * @pre The LUN is writable (read_only_flag = UX_FALSE).
 * @post The device OSPI window holds the pattern on success.
 * @post On failure the offending step printed its error.
 *
 * @note Blocking; 8 four-KiB WRITE(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_write_disk(uint32_t lun)
{
  static uint8_t s_wbuf[k_ospirw_burst_bytes] = {};
  for (uint32_t blk = 0U; blk < (uint32_t)k_ospirw_sectors;
       blk += (uint32_t)k_ospirw_burst_blocks) {
    for (uint32_t s = 0U; s < (uint32_t)k_ospirw_burst_blocks; s++) {
      ospirw_pattern_fill(lun, blk + s, &s_wbuf[s * (uint32_t)k_ospirw_block_size]);
    }
    const ra_err_t err =
      ra_usb_hmsc_write10((uint8_t)lun, blk, (uint16_t)k_ospirw_burst_blocks, s_wbuf);
    if (err != k_ra_ok) {
      (void)ospirw_print_fail("WRITE(10)", err);
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Read + verify one LUN's full sector range against its pattern.
 *
 * @details READ_CAPACITY (must report ::k_ospirw_sectors), then a raw
 * multi-block READ(10) sweep in 8-block bursts, checking each sector
 * against ::ospirw_pattern_fill for this @p lun.
 *
 * @param[in] lun Logical unit to verify (0..1).
 *
 * @return ra_err_t verdict.
 * @retval k_ra_ok            The whole LUN matched its pattern.
 * @retval k_ra_err_invalid_size  READ_CAPACITY reported wrong geometry.
 * @retval k_ra_err_invalid_state A byte differed from the pattern.
 *
 * @pre The host has enumerated the device.
 * @pre @p lun is below ::k_ospirw_count.
 * @post ::s_dbg_mismatch records (lun<<24 | sector) on mismatch.
 * @post Nothing is retained between LUNs.
 *
 * @note Blocking; 32 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_verify_one(uint32_t lun)
{
  static uint8_t s_burst[k_ospirw_burst_bytes] = {};
  static uint8_t s_expect[k_ospirw_block_size] = {};

  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;
  ra_err_t err         = ra_usb_hmsc_read_capacity((uint8_t)lun, &block_count, &block_size);
  if (err != k_ra_ok) {
    (void)ospirw_print_fail("read_capacity", err);
    return err;
  }
  if (block_count != (uint32_t)k_ospirw_sectors) {
    (void)ospirw_print_fail("capacity mismatch", k_ra_err_invalid_size);
    return k_ra_err_invalid_size;
  }
  for (uint32_t blk = 0U; blk < (uint32_t)k_ospirw_sectors;
       blk += (uint32_t)k_ospirw_burst_blocks) {
    err = ra_usb_hmsc_read10((uint8_t)lun, blk, (uint16_t)k_ospirw_burst_blocks, s_burst);
    if (err != k_ra_ok) {
      (void)ospirw_print_fail("READ(10)", err);
      return err;
    }
    for (uint32_t s = 0U; s < (uint32_t)k_ospirw_burst_blocks; s++) {
      ospirw_pattern_fill(lun, blk + s, s_expect);
      const uint32_t boff = s * (uint32_t)k_ospirw_block_size;
      if (memcmp(&s_burst[boff], s_expect, (size_t)k_ospirw_block_size) != 0) {
        s_dbg_mismatch = (lun << (uint32_t)k_ospirw_mismatch_lun_shift) | (blk + s);
        (void)ospirw_print_fail("LUN data mismatch", k_ra_err_invalid_state);
        return k_ra_err_invalid_state;
      }
    }
  }
  return k_ra_ok;
}

/**
 * @brief Print "LUN n OK" for the verified writable unit.
 *
 * @param[in] lun The LUN that just verified.
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The line is queued.
 *
 * @pre ::ospirw_verify_one returned k_ra_ok for @p lun.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_print_lun_ok(uint32_t lun)
{
  ra_err_t err = ospirw_print("ra8d2 ospirw: LUN ");
  if (err != k_ra_ok) {
    return err;
  }
  err = ospirw_print_dec(lun);
  if (err != k_ra_ok) {
    return err;
  }
  return ospirw_print(" OK (64 sectors, write+read verified)\r\n");
}

/**
 * @brief Enumerate the looped device and print its PID + GET_MAX_LUN.
 *
 * @details Sets the enum phase, runs ::ra_usb_hmsc_enumerate, records
 * the reported max-LUN in ::s_dbg_max_lun, and streams the
 * ``enumerated pid=... GET_MAX_LUN=...`` banner. On enumerate failure
 * the host controller is closed so the next retry re-attaches clean.
 *
 * @param[out] device Receives the enumerated descriptor snapshot.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Device enumerated and the banner printed.
 *
 * @pre @p device is non-null.
 * @pre ::ra_usb_hmsc_init has succeeded on this pass.
 * @post ::s_dbg_max_lun mirrors the device's GET_MAX_LUN.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_host_enumerate(ra_usb_hmsc_device_t* device)
{
  s_dbg_phase  = (uint32_t)k_ospirw_phase_enum;
  ra_err_t err = ra_usb_hmsc_enumerate(device);
  if (err != k_ra_ok) {
    (void)ospirw_print_fail("enumerate", err);
    (void)ra_usb_hmsc_close();
    return err;
  }
  s_dbg_max_lun = (uint32_t)device->max_lun;
  err           = ospirw_print("ra8d2 ospirw: enumerated pid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = ospirw_print_hex((uint32_t)device->product_id, (uint8_t)k_ospirw_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err = ospirw_print(", GET_MAX_LUN=");
  if (err != k_ra_ok) {
    return err;
  }
  err = ospirw_print_dec(s_dbg_max_lun);
  if (err != k_ra_ok) {
    return err;
  }
  return ospirw_print("\r\n");
}

/**
 * @brief One full host-side pass: enumerate, WRITE(10) then verify.
 *
 * @details Phases mirror ::ospirw_phase_t. On any failure the host
 * controller is closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The pass printed WRITABLE-OSPI PASS.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ospirw_host_pass(void)
{
  s_dbg_phase  = (uint32_t)k_ospirw_phase_init;
  ra_err_t err = ospirw_print("ra8d2 ospirw: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_hmsc_init(k_ra_usb_speed_hs);
  if (err != k_ra_ok) {
    (void)ospirw_print_fail("host init", err);
    return err;
  }

  ra_usb_hmsc_device_t device = {};
  err                         = ospirw_host_enumerate(&device);
  if (err != k_ra_ok) {
    return err;
  }

  s_dbg_phase   = (uint32_t)k_ospirw_phase_verify;
  s_dbg_luns_ok = 0U;
  for (uint32_t lun = 0U; lun < (uint32_t)k_ospirw_count; lun++) {
    err = ospirw_write_disk(lun);
    if (err != k_ra_ok) {
      (void)ra_usb_hmsc_close();
      return err;
    }
    err = ospirw_verify_one(lun);
    if (err != k_ra_ok) {
      (void)ra_usb_hmsc_close();
      return err;
    }
    s_dbg_luns_ok++;
    err = ospirw_print_lun_ok(lun);
    if (err != k_ra_ok) {
      return err;
    }
  }

  s_dbg_phase = (uint32_t)k_ospirw_phase_pass;
  s_dbg_pass_count++;
  err = ospirw_print("ra8d2 ospirw: USB SELFTEST WRITABLE-OSPI PASS\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led2);
  return k_ra_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops
 * ::ospirw_host_pass with a retry pause until the writable LUN verifies;
 * afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra_time.
 * @since 0.1.0
 */
static VOID ospirw_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_ospirw_boot_wait_ticks);
  for (;;) {
    const ra_err_t err = ospirw_host_pass();
    if (err == k_ra_ok) {
      break;
    }
    tx_thread_sleep(k_ospirw_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_ospirw_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Device worker at priority 8, host worker at 24 (below the
 * USBX class threads). Sets ::s_tx_kernel_up so SysTick may feed
 * ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post Two auto-start worker threads are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         "ospirw_device",
                         ospirw_device_worker,
                         0UL,
                         s_device_stack,
                         k_ospirw_thread_stack,
                         (UINT)k_ospirw_dev_priority,
                         (UINT)k_ospirw_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "ospirw_host",
                         ospirw_host_worker,
                         0UL,
                         s_host_stack,
                         k_ospirw_host_stack,
                         (UINT)k_ospirw_host_priority,
                         (UINT)k_ospirw_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void ospirw_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (else
 * peripheral routing forces host VBUSEN and blocks device enum),
 * P8_14/P8_15 data. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::ospirw_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void ospirw_route_usb_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_ospirw_pin_fs_vbus, k_ra_psel_usb_fs, "ospirw.fs_vbus") !=
      k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_gpio_output_init(k_ospirw_pin_fs_vbusen, k_ra_level_low) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ospirw_pin_fs_dp, k_ra_psel_usb_fs, "ospirw.fs_dp") != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ospirw_pin_fs_dm, k_ra_psel_usb_fs, "ospirw.fs_dm") != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_gpio_output_init(k_ospirw_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ospirw_pin_hs_vbus, k_ra_psel_usb_hs, "ospirw.hs_vbus") !=
      k_ra_ok) {
    ospirw_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference; USBHS needs its UTMI
 * PLL. SCI8 is the J-Link OB CDC console at 115200.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called once from main.
 * @since 0.1.0
 */
static void ospirw_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ospirw_pin_sci_tx, k_ra_psel_sci_async, "ospirw.txd8") != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_ospirw_pin_sci_rx, k_ra_psel_sci_async, "ospirw.rxd8") != k_ra_ok) {
    ospirw_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_ospirw_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_ospirw_sci_channel, &sci_cfg) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    ospirw_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    ospirw_panic_halt();
  }
  ospirw_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  ospirw_setup_or_halt();

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  ospirw_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
