/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file
 * examples/ek_ra8d2/hw_validated/hil/usb_selftest_microsd/src/usb_selftest_microsd_steps.h
 * @brief Shared contract for the USB-microSD self-test console + worker steps
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the constants, the two cross-translation-unit J-Link probes, and
 * the function prototypes shared between ``main.c`` and the helper siblings
 * ``usb_selftest_microsd_console.c`` (SCI8 console formatters) and
 * ``usb_selftest_microsd_steps.c`` (USBX device worker, read-only MSC media
 * callbacks, and the polled host enumerate/verify ladder). The header is
 * self-contained: it pulls in ``<stdint.h>`` for the typed-enum underlying
 * types and ``ra8_err.h`` for ::ra8_err_t. The ThreadX worker entry points are
 * only declared when the firmware (on-target) USB stack is compiled in.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum microsd_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_microsd_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_microsd_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_microsd_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_microsd_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_microsd_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_microsd_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_microsd_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_microsd_sd_spi_channel  = 0U,      /**< SCI0 Simple-SPI -> Pmod2 microSD.  */
  k_microsd_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_microsd_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_microsd_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} microsd_config_t;

/**
 * @enum microsd_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_microsd_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_microsd_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_microsd_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_microsd_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_microsd_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} microsd_hex_t;

/**
 * @enum microsd_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_microsd_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_microsd_dec_radix   = 10U,  /**< Base for decimal conversion. */
} microsd_mask_t;

/**
 * @enum microsd_geom_t
 * @brief LUN geometry + verification constants.
 */
typedef enum : uint32_t {
  k_microsd_count        = 1U,          /**< Single read-only logical unit. */
  k_microsd_sectors      = 64U,         /**< Exposed sectors (LBA 0..63).   */
  k_microsd_block_size   = 512U,        /**< SCSI logical block size.       */
  k_microsd_burst_blocks = 8U,          /**< Blocks per READ(10) burst.     */
  k_microsd_burst_bytes  = 4096U,       /**< 8 x 512 B burst buffer.        */
  k_microsd_target_lun0  = 0U,          /**< First LUN index.               */
  k_microsd_no_mismatch  = 0xFFFFFFFFU, /**< Probe: no mismatch.            */
  k_microsd_snap_bytes   = 64U * 512U,  /**< RAM snapshot of LBA 0..63.     */
  k_microsd_bootsig_off  = 510U,        /**< FAT/exFAT 0x55AA at byte 510.  */
  k_microsd_bootsig_lo   = 0x55U,       /**< Boot-signature byte 510.       */
  k_microsd_bootsig_hi   = 0xAAU,       /**< Boot-signature byte 511.       */
} microsd_geom_t;

/**
 * @enum microsd_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_microsd_phase_boot   = 0U, /**< Host thread not started.  */
  k_microsd_phase_init   = 1U, /**< ra8_usb_hmsc_init issued. */
  k_microsd_phase_enum   = 2U, /**< Enumerating.              */
  k_microsd_phase_verify = 3U, /**< Reading + checking LUNs.  */
  k_microsd_phase_pass   = 4U, /**< All LUNs verified.        */
} microsd_phase_t;

/**
 * @enum microsd_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_microsd_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_microsd_dev_step_class  = 2U, /**< MSC class registered.          */
  k_microsd_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_microsd_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_microsd_dev_step_parked = 5U, /**< Bring-up done; worker parked.  */
} microsd_dev_step_t;

/** @brief SCSI sense triple for an unsupported / out-of-range request. */
typedef enum : uint8_t {
  k_scsi_sense_illegal_request = 0x05U, /**< Sense key: ILLEGAL REQUEST. */
  k_scsi_asc_lba_out_of_range  = 0x21U, /**< ASC: LBA out of range.      */
  k_scsi_ascq_none             = 0x00U, /**< ASCQ: none.                 */
  k_scsi_sense_data_protect    = 0x07U, /**< Sense key: DATA PROTECT.    */
  k_scsi_asc_write_protected   = 0x27U, /**< ASC: WRITE PROTECTED.       */
} scsi_sense_code_t;

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_usb_selftest_microsd_sd_snapshot
 * @brief Read-only RAM snapshot of SD LBA 0..63, taken once at boot.
 * @details Defined in ``main.c`` (populated before the kernel starts by the
 *          SD bring-up step) and read by both the device-side media-read
 *          callback and the host-side verify ladder in
 *          ``usb_selftest_microsd_steps.c``.
 * @note Single-writer at boot; read-only afterwards.
 * @since 0.1.0
 */
extern unsigned char s_usb_selftest_microsd_sd_snapshot[(uint32_t)k_microsd_snap_bytes];

/**
 * @var s_usb_selftest_microsd_dbg_bootsig
 * @brief 1 = LBA0 carries the 0x55AA FAT/exFAT boot signature.
 * @details Defined and stamped in ``main.c`` during SD bring-up; read by the
 *          host-side verify ladder to confirm the snapshot is a valid boot
 *          region before the READ(10) sweep.
 * @note Single-writer at boot; read-only afterwards.
 * @since 0.1.0
 */
extern volatile uint32_t s_usb_selftest_microsd_dbg_bootsig;

/**
 * @brief Print a NUL-terminated ASCII string over the SCI8 console.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_microsd_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t microsd_print(const char* text);

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
[[nodiscard]] ra8_err_t microsd_print_dec(uint32_t value);

/**
 * @brief Print a value as fixed-width uppercase hex over the SCI8 console.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_microsd_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t microsd_print_hex(uint32_t value, uint8_t digits);

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
[[nodiscard]] ra8_err_t microsd_print_fail(const char* what, ra8_err_t err);

#ifndef RA8_OFF_TARGET
#include "tx_api.h"

/**
 * @brief Device-side worker: bring the read-only SD FS device up, then park.
 *
 * @details USBX system + device stack + MSC class (1 LUN) + DCD bridge
 * on the USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB
 * state machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable on its single LUN.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
VOID microsd_device_worker(ULONG arg);

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full
 * enumerate/verify pass with a retry pause until the SD LUN verifies;
 * afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra8_time.
 * @since 0.1.0
 */
VOID microsd_host_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
