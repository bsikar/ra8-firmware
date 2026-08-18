/**
 * @file
 * examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops/src/usb_host_file_ops_steps.h
 * @brief Console helpers + ra8_fs file-op suite for the USB host file-ops app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the shared compile-time configuration enums, the polled-console
 * print helpers, and the ra8_fs-over-USB-MSC suite routines that back the
 * `usb_host_file_ops` manual HIL test. The definitions live in
 * usb_host_file_ops_steps.c; `main.c` keeps only the boot/bring-up code and
 * the retry ladder and calls into this interface.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"

/* =============================================================================
 * Compile-time configuration (shared between main.c and the steps TU)
 * =============================================================================
 */

/**
 * @enum usb_fileops_config_t
 * @brief Compile-time settings for the host file-ops suite.
 */
typedef enum : uint32_t {
  k_fileops_baud       = 115200U, /**< J-Link OB CDC log baud.            */
  k_fileops_idle_ms    = 50U,     /**< Idle tick in the parked main loop. */
  k_fileops_retry_ms   = 5000U,   /**< Pause between ladder retries.      */
  k_fileops_target_lun = 0U,      /**< Exercise LUN 0 (typical stick).    */
  k_fileops_print_cap  = 160U,    /**< Bound for console-string scans.    */
} usb_fileops_config_t;

/**
 * @enum usb_fileops_size_t
 * @brief Buffer sizing constants.
 */
typedef enum : uint16_t {
  k_fileops_sector_bytes = 512U, /**< One SCSI block / FAT sector. */
  k_fileops_name_cap     = 64U,  /**< Listdir name compare bound.  */
} usb_fileops_size_t;

/**
 * @enum usb_fileops_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_fileops_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_fileops_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_fileops_dec_chars_u64   = 20U, /**< Max digits for a 64-bit count. */
  k_fileops_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_fileops_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} usb_fileops_hex_t;

/**
 * @enum usb_fileops_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_fileops_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_fileops_dec_radix   = 10U,  /**< Base for decimal conversion. */
} usb_fileops_mask_t;

/**
 * @enum usb_fileops_probe_t
 * @brief Sizing for the on-mount-failure disk-layout probe dump.
 */
typedef enum : uint32_t {
  k_fileops_probe_tbl_off  = 432U, /**< LBA0 dump start (partition table). */
  k_fileops_probe_tbl_len  = 80U,  /**< LBA0 dump length (incl. 0x55AA).   */
  k_fileops_probe_head_len = 64U,  /**< LBA1/LBA2 head dump length.        */
  k_fileops_probe_row      = 16U,  /**< Hex bytes per dump row.            */
  k_fileops_probe_lba_max  = 3U,   /**< Probe reads LBAs 0..2.             */
} usb_fileops_probe_t;

/* =============================================================================
 * Console helpers
 * =============================================================================
 */

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_fileops_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_print(const char* text);

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @param[in] value Value to print.
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_print_dec(uint64_t value);

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_fileops_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_print_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The diagnostic line is queued.
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_print_fail(const char* what, ra8_err_t err);

/* =============================================================================
 * Mount + suite + probe
 * =============================================================================
 */

/**
 * @brief Mount the attached drive through the USB-MSC backend.
 *
 * @param[out] out_mount Receives the mount handle on success.
 * @return ra8_err_t from the block-device bind, the ra8_fs bridge, or
 *         ::ra8_fs_mount.
 * @retval k_ra8_ok Volume mounted; the type line was printed.
 * @pre ::ra8_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p out_mount is non-NULL.
 * @post On k_ra8_ok the mount handle is live and must be unmounted later.
 * @post The "mounted fs=" line is queued on success.
 * @note Reads the MBR/BPB chain over USB.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_mount_volume(ra8_fs_mount_t** out_mount);

/**
 * @brief Suite steps 1..5: cleanup, baseline listdir, write, verify, list.
 *
 * @param[in] mount Live mount handle.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Steps 1-5 all passed and printed their verdicts.
 * @pre @p mount is a live handle from ::ra8_fs_mount.
 * @pre SCI8 init already ran.
 * @post On k_ra8_ok the volume holds ::k_fileops_name_a with the payload.
 * @post Each completed step toggled LED2 and queued its verdict line.
 * @note Mutates the volume (creates the test file).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_suite_create(ra8_fs_mount_t* mount);

/**
 * @brief Suite steps 6..9: rename, old-gone/new-intact, list, delete.
 *
 * @param[in] mount Live mount handle.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Steps 6-9 all passed and printed their verdicts.
 * @pre ::fileops_suite_create completed on this mount.
 * @pre SCI8 init already ran.
 * @post On k_ra8_ok the volume no longer holds either test name.
 * @post Each completed step toggled LED2 and queued its verdict line.
 * @note Mutates the volume (rename + unlink).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fileops_suite_mutate(ra8_fs_mount_t* mount);

/**
 * @brief Dump the partition table + LBA1/LBA2 heads after a mount failure.
 *
 * @details Reads LBA 0 (MBR partition entries at 0x1BE), LBA 1, and LBA 2
 * straight through the USB backend and prints hex excerpts, so an
 * unrecognized disk layout (e.g. GPT) can be identified from the log.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the attached drive.
 * @pre SCI8 init already ran.
 * @post Three dump blocks are queued on the console.
 * @post No filesystem state changes.
 * @note Print/read errors are swallowed -- diagnostic only.
 * @since 0.1.0
 */
void fileops_probe_layout(void);
