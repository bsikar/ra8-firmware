/**
 * @file
 * examples/ek_ra8d2/hw_pending/manual/usb_msc_sdcard/src/usb_msc_sdcard_steps.h
 * @brief Shared contract for the live-SD USB MSC device console + worker steps
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the constants, the cross-translation-unit card-capacity probe, and
 * the function prototypes shared between ``main.c`` and the helper siblings
 * ``usb_msc_sdcard_console.c`` (SCI8 console formatters) and
 * ``usb_msc_sdcard_steps.c`` (USBX device worker plus the read/write MSC media
 * callbacks that drive the live SD card). The header is self-contained: it
 * pulls in ``<stdint.h>`` for the typed-enum underlying types and ``ra8_err.h``
 * for ::ra8_err_t. The ThreadX worker entry point is only declared when the
 * firmware (on-target) USB stack is compiled in.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-08
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum sdmsc_config_t
 * @brief Compile-time settings: thread, pool, console, SD bus, cadence.
 */
typedef enum : uint32_t {
  k_sdmsc_thread_stack    = 4096U,   /**< Device worker stack (bytes).      */
  k_sdmsc_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).         */
  k_sdmsc_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).     */
  k_sdmsc_baud            = 115200U, /**< J-Link OB CDC log baud.           */
  k_sdmsc_sd_spi_channel  = 0U,      /**< SCI0 Simple-SPI -> Pmod2 microSD. */
  k_sdmsc_print_cap       = 160U,    /**< Bound for console-string scans.   */
  k_sdmsc_dev_priority    = 8U,      /**< Device bring-up worker priority.  */
  k_sdmsc_block_size      = 512U,    /**< SCSI logical block size (bytes).  */
  k_sdmsc_blocks_per_mib  = 2048U,   /**< 512-B blocks per MiB.             */
} sdmsc_config_t;

/**
 * @enum sdmsc_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_sdmsc_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_sdmsc_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_sdmsc_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_sdmsc_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} sdmsc_hex_t;

/**
 * @enum sdmsc_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_sdmsc_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_sdmsc_dec_radix   = 10U,  /**< Base for decimal conversion. */
} sdmsc_mask_t;

/**
 * @enum sdmsc_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_sdmsc_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_sdmsc_dev_step_class  = 2U, /**< MSC class registered.          */
  k_sdmsc_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_sdmsc_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_sdmsc_dev_step_parked = 5U, /**< Bring-up done; worker parked.  */
} sdmsc_dev_step_t;

/**
 * @enum scsi_sense_code_t
 * @brief SCSI sense triples (key / ASC / ASCQ) the media callbacks report.
 *
 * @details Per SPC-4 / SBC-3: an out-of-range LBA is ILLEGAL REQUEST with
 * "LOGICAL BLOCK ADDRESS OUT OF RANGE"; a failed card read is MEDIUM ERROR
 * with "UNRECOVERED READ ERROR"; a failed card write is MEDIUM ERROR with
 * "PERIPHERAL DEVICE WRITE FAULT". The class stores the triple into the
 * LUN's REQUEST SENSE data, so the host sees the true failure cause.
 */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST.      */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.           */
  k_scsi_sense_medium_error    = 0x03U, /**< Sense key: MEDIUM ERROR.         */
  k_scsi_asc_unrecovered_read  = 0x11U, /**< ASC: unrecovered read error.     */
  k_scsi_asc_write_fault       = 0x03U, /**< ASC: peripheral dev write fault. */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                      */
} scsi_sense_code_t;

/**
 * @enum usb_langid_byte_t
 * @brief USBX LANGID descriptor 0x0409 (English-US), little-endian pair.
 */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_usb_msc_sdcard_blocks
 * @brief Live SD card capacity in 512-byte blocks (0 = no card).
 * @details Defined in ``main.c`` and stamped once during the pre-kernel SD
 *          bring-up from ``ra8_sdmmc_spi_get_capacity`` (CSD-derived). Read by
 *          the worker in ``usb_msc_sdcard_steps.c`` to size the MSC LUN and
 *          by the media callbacks to bounds-check every SCSI request.
 * @note Single-writer at boot; read-only afterwards.
 * @warning Do not modify after the USBX class registers -- the LUN geometry
 *          is latched from this value.
 * @since 0.1.0
 */
extern volatile uint32_t s_usb_msc_sdcard_blocks;

/**
 * @brief Print a NUL-terminated ASCII string over the SCI8 console.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_sdmsc_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t sdmsc_print(const char* text);

/**
 * @brief Print a uint32_t as ASCII decimal over the SCI8 console.
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
[[nodiscard]] ra8_err_t sdmsc_print_dec(uint32_t value);

/**
 * @brief Print a value as fixed-width uppercase hex over the SCI8 console.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (up to 8).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_sdmsc_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t sdmsc_print_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own console line.
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
[[nodiscard]] ra8_err_t sdmsc_print_fail(const char* what, ra8_err_t err);

#ifndef RA8_OFF_TARGET
#include "tx_api.h"

/**
 * @brief Device worker: bring the live-SD read/write MSC device up, then park.
 *
 * @details USBX system + device stack + MSC class (1 writable LUN sized from
 * ::s_usb_msc_sdcard_blocks) + DCD bridge on the USBFS controller, then DPRPU
 * attach and the READY banner. USBX runs the SCSI/BBB state machine on its
 * own class threads after this; the media callbacks stream the live card via
 * ``ra8_sdmmc_spi_read_blocks`` / ``ra8_sdmmc_spi_write_blocks``.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock + the SD card are up (main did all three).
 * @post The FS device is attached and serviceable on its single LUN.
 * @post On any bring-up failure the thread prints the step and exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
VOID sdmsc_device_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
