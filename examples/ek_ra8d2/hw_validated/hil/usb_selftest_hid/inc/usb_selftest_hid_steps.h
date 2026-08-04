/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_hid/inc/usb_selftest_hid_steps.h
 * @brief Shared contract for the USB HID self-loop console + host clusters
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The `usb_selftest_hid` application is split across three translation units
 * to keep every file under the source-size cap:
 *
 *  - `main.c` owns boot bring-up, the USBX HID device side, the ThreadX
 *    workers' creation, and startup.
 *  - `src/usb_selftest_hid_console.c` owns the SCI8 -> J-Link OB CDC console
 *    formatters (decimal / hex / fail-line printers).
 *  - `src/usb_selftest_hid_host.c` owns the self-contained polled USB host
 *    enumeration ladder, the interrupt-IN report reader, and the host worker.
 *
 * This header is the seam between those units. It carries the compile-time
 * constant enums shared across the three TUs, the console-formatter
 * prototypes, the shared report-pattern helper, and the host worker entry.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/**
 * @enum hid_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_hid_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_hid_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_hid_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_hid_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_hid_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_hid_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_hid_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_hid_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_hid_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_hid_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} hid_config_t;

/**
 * @enum hid_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_hid_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_hid_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_hid_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_hid_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_hid_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} hid_hex_t;

/**
 * @enum hid_geom_t
 * @brief HID report + interrupt-pipe + pattern constants.
 */
typedef enum : uint32_t {
  k_hid_mps         = 64U,         /**< Interrupt endpoint wMaxPacketSize. */
  k_hid_report_len  = 8U,          /**< Vendor input report width (bytes). */
  k_hid_rounds      = 8U,          /**< Reports the host reads + checks.   */
  k_hid_read_buf    = 64U,         /**< One-MPS receive buffer (bytes).    */
  k_hid_dev_addr    = 1U,          /**< Address the host assigns.          */
  k_hid_ep_in_num   = 1U,          /**< Device interrupt-IN endpoint num.  */
  k_hid_pipe_in     = 1U,          /**< Host pipe for the device IN.       */
  k_hid_seq_idx     = 0U,          /**< Report byte 0: rolling seq.        */
  k_hid_body_idx    = 1U,          /**< Report body starts at byte 1.      */
  k_hid_no_mismatch = 0xFFFFFFFFU, /**< Probe: no mismatch.                */
  k_hid_pat_idx_mul = 7U,          /**< Per-index pattern multiplier.      */
  k_hid_pat_bias    = 0x5AU,       /**< Pattern constant bias.             */
  k_hid_byte_mask   = 0xFFU,       /**< Byte mask.                         */
} hid_geom_t;

/**
 * @brief Fill the fixed body of a HID report (bytes 1..len-1).
 *
 * @details Body byte i = ``(i*7 + 0x5A) & 0xFF``, independent of the report
 * sequence. Byte 0 (the seq) is left untouched -- the device stamps it with
 * a rolling counter and the host ignores it for the pattern check. Both the
 * device (to build) and the host (to verify) compute the same body.
 *
 * @param[out] out Report buffer.
 * @param[in]  len Report width in bytes (>= 1).
 *
 * @pre @p out has @p len writable bytes.
 * @pre @p len is at most ::k_hid_read_buf.
 * @post @p out[1..len-1] hold the fixed pattern bytes.
 * @post @p out[0] is unchanged.
 *
 * @note Pure function (apart from the caller's buffer).
 * @since 0.1.0
 */
void hid_fill_report_body(uint8_t* out, uint32_t len);

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by the console-string cap.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_hid_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t hid_print(const char* text);

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
[[nodiscard]] ra8_err_t hid_print_dec(uint32_t value);

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
 * @pre @p digits is at most ::k_hid_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t hid_print_hex(uint32_t value, uint8_t digits);

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
[[nodiscard]] ra8_err_t hid_print_fail(const char* what, ra8_err_t err);

#ifndef RA8_OFF_TARGET
/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full host
 * pass with a retry pause until every report round verifies; afterwards
 * parks so the verdict stays on the wire.
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
VOID hid_host_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
