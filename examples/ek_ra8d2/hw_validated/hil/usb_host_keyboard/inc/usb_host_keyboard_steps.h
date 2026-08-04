/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_keyboard/inc/usb_host_keyboard_steps.h
 * @brief Shared seam between usb_host_keyboard main.c and its sibling TUs
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the helper clusters carved out of the oversized
 * `usb_host_keyboard/main.c` so that every translation unit stays under the
 * 1000-line cap. Two siblings sit behind this header:
 *
 *  - `usb_host_keyboard_console.c` -- the SCI8 -> J-Link OB CDC console
 *    formatters (`hid_print*`, `hid_sci_write`, `hid_str_len`,
 *    `hid_nibble_to_hex`). main.c's host-side ladder calls these.
 *  - `usb_host_keyboard_device.c` -- the USBX HID device worker (descriptors,
 *    activate / deactivate callbacks, report send loop, and the device-thread
 *    bring-up helper). main.c's `tx_application_define` spawns the worker.
 *
 * Only the symbols that genuinely cross a TU boundary are published here. The
 * shared compile-time enums live in this header so both siblings and main.c
 * agree on one definition. The shared report-pattern builder
 * (`hid_fill_report_body`) is computed by the device side and re-computed by
 * the host side to verify, so it is published too. The activation semaphore is
 * the single mutable object the device worker and main.c share; it is defined
 * once in main.c as `s_usb_host_keyboard_hid_active_sem` and externed here.
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

/* -------------------------------------------------------------------------- */
/* Shared compile-time constants */
/* -------------------------------------------------------------------------- */

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
 * @enum hid_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_hid_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_hid_dec_radix   = 10U,  /**< Base for decimal conversion. */
} hid_mask_t;

/**
 * @enum hid_geom_t
 * @brief HID report + interrupt-pipe + pattern constants.
 */
typedef enum : uint32_t {
  k_hid_mps         = 64U,         /**< Interrupt endpoint wMaxPacketSize.     */
  k_hid_report_len  = 8U,          /**< Vendor input report width (bytes).     */
  k_hid_rounds      = 8U,          /**< Reports the host reads + checks.       */
  k_hid_read_buf    = 64U,         /**< One-MPS receive buffer (bytes).        */
  k_hid_dev_addr    = 1U,          /**< Address the host assigns.              */
  k_hid_ep_in_num   = 1U,          /**< Device interrupt-IN endpoint num.      */
  k_hid_pipe_in     = 1U,          /**< Host pipe for the device IN.           */
  k_hid_seq_idx     = 0U,          /**< Report byte 0: modifier / rolling seq. */
  k_hid_body_idx    = 1U,          /**< Report body starts at byte 1.          */
  k_hid_no_mismatch = 0xFFFFFFFFU, /**< Probe: no mismatch.                    */
  k_hid_pat_idx_mul = 7U,          /**< Per-index pattern multiplier.          */
  k_hid_pat_bias    = 0x5AU,       /**< Pattern constant bias.                 */
  k_hid_byte_mask   = 0xFFU,       /**< Byte mask.                             */
  k_hid_key0_idx    = 2U,          /**< Boot-keyboard first-keycode byte.      */
  k_hid_nkeys       = 5U,          /**< Keycodes typed ("RA8D2").              */
} hid_geom_t;

/**
 * @enum hid_keycode_t
 * @brief HID Usage-Table keycode ranges for decoding keycodes back to ASCII.
 */
typedef enum : uint8_t {
  k_hid_kc_a = 0x04U, /**< Keycode for 'a' / 'A'.           */
  k_hid_kc_z = 0x1DU, /**< Keycode for 'z' / 'Z'.           */
  k_hid_kc_1 = 0x1EU, /**< Keycode for '1'.                 */
  k_hid_kc_0 = 0x27U, /**< Keycode for '0' (top of digits). */
  k_hid_kc_r = 0x15U, /**< Keycode for 'r' / 'R'.           */
  k_hid_kc_8 = 0x25U, /**< Keycode for '8'.                 */
  k_hid_kc_d = 0x07U, /**< Keycode for 'd' / 'D'.           */
  k_hid_kc_2 = 0x1FU, /**< Keycode for '2'.                 */
} hid_keycode_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* Shared mutable state (defined once in main.c) */
/* -------------------------------------------------------------------------- */

/**
 * @var s_usb_host_keyboard_hid_active_sem
 * @brief Activation semaphore shared by the device worker and main.c.
 * @details Posted by the HID activate callback (device sibling) so the send
 *          worker blocks on it instead of polling; created by
 *          `tx_application_define` in main.c. Defined once in main.c.
 * @note Single-producer (class thread), single-consumer (send worker).
 * @since 0.1.0
 */
extern TX_SEMAPHORE s_usb_host_keyboard_hid_active_sem;

/* -------------------------------------------------------------------------- */
/* Shared HID report pattern */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill the fixed body of a HID report (bytes 1..len-1).
 *
 * @details Boot-keyboard report: byte 1 = reserved, bytes 2.. = the typed
 * "RA8D2" keycodes, the remainder 0. Byte 0 (the seq) is left untouched. Both
 * the device (to build) and the host (to verify) compute the same body.
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

/* -------------------------------------------------------------------------- */
/* Device side: USBX HID interrupt-IN reports */
/* -------------------------------------------------------------------------- */

/**
 * @brief Device-side worker: bring the HID device up, then send reports.
 *
 * @details USBX system + device stack + HID class + DCD bridge on the USBFS
 * controller, then DPRPU attach. Blocks on the activation semaphore until the
 * host configures the device, then loops to keep the interrupt-IN report queue
 * fed.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and streaming input reports.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
VOID hid_device_worker(ULONG arg);

/**
 * @brief Create + auto-start the USBX HID device worker thread.
 *
 * @details Owns the device thread's TCB + stack storage at file scope in the
 * device sibling and spawns ::hid_device_worker at ::k_hid_dev_priority. Called
 * once from `tx_application_define` in main.c.
 *
 * @pre Called from `tx_kernel_enter` after scheduler init.
 * @pre The activation semaphore exists.
 * @post One auto-start device worker is queued.
 * @post The device thread runs at ::k_hid_dev_priority.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
void usb_host_keyboard_device_thread_create(void);

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @param[in] nibble 4-bit value.
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
uint8_t hid_nibble_to_hex(uint32_t nibble);

/**
 * @brief Bounded ASCII string length (cap ::k_hid_print_cap).
 *
 * @param[in] text NUL-terminated string.
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the length.
 * @post No state changes.
 * @post Return value never exceeds ::k_hid_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
uint32_t hid_str_len(const char* text);

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
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
[[nodiscard]] ra8_err_t hid_sci_write(const uint8_t* data, uint32_t len);

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @param[in] text String to print (CR/LF included by the caller).
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
 * @param[in] value Value to print.
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
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
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
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
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

#endif /* !RA8_OFF_TARGET */
