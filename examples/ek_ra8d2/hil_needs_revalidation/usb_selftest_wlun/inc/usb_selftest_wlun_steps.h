/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file
 * examples/ek_ra8d2/hil_needs_revalidation/usb_selftest_wlun/src/usb_selftest_wlun_steps.h
 * @brief Shared seam for the USB writable-LUN self-loop: constants + helpers
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Splits the writable-LUN self-loop example into cohesive translation units
 * so each one stays under the per-file line cap. This header is the single
 * shared contract between ``main.c`` (board bring-up + device side) and the
 * two sibling units:
 *
 *  - ``usb_selftest_wlun_console.c`` -- the deterministic per-(LUN,LBA)
 *    pattern generator plus the SCI8 polled console formatters.
 *  - ``usb_selftest_wlun_host.c``    -- the polled host MSC ladder:
 *    enumerate, WRITE(10) the pattern across the whole RAM disk, then
 *    READ(10) it back and byte-check every sector.
 *
 * It carries the compile-time tunables, the geometry / pattern constants,
 * the host-ladder phase markers, and the prototypes of every helper that is
 * referenced across translation units. ``main.c`` keeps ``main()``, the
 * SysTick override, ``tx_application_define``, the USB descriptors, the
 * device-side media callbacks, and the device-worker bring-up.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb_hmsc.h"

#ifndef RA8_OFF_TARGET
#include "ux_api.h"
#endif

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/**
 * @enum wlun_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_wlun_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_wlun_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_wlun_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_wlun_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_wlun_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_wlun_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_wlun_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_wlun_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_wlun_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_wlun_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} wlun_config_t;

/**
 * @enum wlun_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_wlun_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_wlun_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_wlun_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_wlun_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_wlun_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} wlun_hex_t;

/**
 * @enum wlun_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_wlun_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_wlun_dec_radix   = 10U,  /**< Base for decimal conversion. */
} wlun_mask_t;

/**
 * @enum wlun_geom_t
 * @brief LUN geometry + verification constants.
 */
typedef enum : uint32_t {
  k_wlun_count              = 1U,          /**< Logical units exposed (single writable). */
  k_wlun_sectors            = 64U,         /**< 512-byte sectors (RAM disk = 32 KiB).    */
  k_wlun_block_size         = 512U,        /**< SCSI logical block size.                 */
  k_wlun_burst_blocks       = 8U,          /**< Blocks per READ(10) burst.               */
  k_wlun_burst_bytes        = 4096U,       /**< 8 x 512 B burst buffer.                  */
  k_wlun_target_lun0        = 0U,          /**< First LUN index.                         */
  k_wlun_no_mismatch        = 0xFFFFFFFFU, /**< Probe: no mismatch.                      */
  k_wlun_pat_lun_mul        = 97U,         /**< Per-LUN pattern multiplier.              */
  k_wlun_pat_lba_mul        = 7U,          /**< Per-LBA pattern multiplier.              */
  k_wlun_pat_bias           = 0x5AU,       /**< Pattern constant bias.                   */
  k_wlun_byte_mask          = 0xFFU,       /**< Byte mask.                               */
  k_wlun_mismatch_lun_shift = 24U,         /**< s_dbg_mismatch: LUN in bits 31:24.       */
} wlun_geom_t;

/**
 * @enum wlun_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_wlun_phase_boot   = 0U, /**< Host thread not started.  */
  k_wlun_phase_init   = 1U, /**< ra8_usb_hmsc_init issued. */
  k_wlun_phase_enum   = 2U, /**< Enumerating.              */
  k_wlun_phase_verify = 3U, /**< Reading + checking LUNs.  */
  k_wlun_phase_pass   = 4U, /**< All LUNs verified.        */
} wlun_phase_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* Shared per-(LUN,LBA) pattern */
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
 * @pre @p out has ::k_wlun_block_size writable bytes.
 * @pre @p lun and @p lba are within the exposed geometry.
 * @post @p out holds the sector's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
void wlun_pattern_fill(uint32_t lun, uint32_t lba, UCHAR* out);

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
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
uint8_t wlun_nibble_to_hex(uint32_t nibble);

/**
 * @brief Bounded ASCII string length (cap ::k_wlun_print_cap).
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
 * @post Return value never exceeds ::k_wlun_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
uint32_t wlun_str_len(const char* text);

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @details Thin wrapper fixing the console channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t wlun_sci_write(const uint8_t* data, uint32_t len);

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::wlun_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_wlun_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t wlun_print(const char* text);

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
[[nodiscard]] ra8_err_t wlun_print_dec(uint32_t value);

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
 * @pre @p digits is at most ::k_wlun_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t wlun_print_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; first failing chunk's code returned.
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
[[nodiscard]] ra8_err_t wlun_print_fail(const char* what, ra8_err_t err);

/* -------------------------------------------------------------------------- */
/* Host side: ra8_usb_hmsc enumerate + WRITE(10) then read-verify */
/* -------------------------------------------------------------------------- */

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full
 * enumerate / WRITE(10) / verify pass with a retry pause until the
 * writable LUN verifies; afterwards parks so the verdict stays on the
 * wire.
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
VOID wlun_host_worker(ULONG arg);

#endif /* !RA8_OFF_TARGET */
