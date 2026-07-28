/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_hid/src/usb_selftest_hid_console.c
 * @brief USB HID self-loop console formatters (SCI8 -> J-Link OB CDC)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The console-formatter cluster split out of the `usb_selftest_hid` main
 * translation unit: the polled SCI8 text helpers that stream verdicts over
 * the J-Link OB CDC bridge. These are pure formatting routines (bounded
 * decimal / fixed-width hex / one-line FAIL diagnostics) with no USBX or
 * ThreadX dependency; they live behind the off-target guard solely because
 * the application as a whole is hardware-only.
 *
 * The public entry points (::hid_print, ::hid_print_dec, ::hid_print_hex,
 * ::hid_print_fail) are declared in `usb_selftest_hid_steps.h` and consumed
 * by both `main.c` and the host cluster.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "usb_selftest_hid_steps.h"

#ifndef RA8_OFF_TARGET

/**
 * @enum hid_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_hid_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_hid_dec_radix   = 10U,  /**< Base for decimal conversion. */
} hid_mask_t;

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
static uint8_t hid_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_hid_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_hid_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_hid_print_cap).
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
 * @post Return value never exceeds ::k_hid_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t hid_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_hid_print_cap) {
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
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; the BSP console init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

[[nodiscard]] ra8_err_t hid_print(const char* text)
{
  return hid_sci_write((const uint8_t*)text, hid_str_len(text));
}

[[nodiscard]] ra8_err_t hid_print_dec(uint32_t value)
{
  uint8_t  scratch[k_hid_dec_chars_u32] = {};
  uint8_t  out[k_hid_dec_chars_u32]     = {};
  uint8_t  count                        = 0U;
  uint32_t v                            = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return hid_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_hid_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_hid_dec_radix));
    v              = v / k_hid_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return hid_sci_write(out, (uint32_t)count);
}

[[nodiscard]] ra8_err_t hid_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_hid_hex_chars_u32] = {};
  uint8_t width                    = digits;
  if (width > (uint8_t)k_hid_hex_chars_u32) {
    width = (uint8_t)k_hid_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_hid_nibble_bits);
    out[i]              = hid_nibble_to_hex((value >> shift) & k_hid_nibble_mask);
  }
  return hid_sci_write(out, (uint32_t)width);
}

[[nodiscard]] ra8_err_t hid_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = hid_print("ra8d2 hid: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = hid_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = hid_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = hid_print_hex((uint32_t)err, (uint8_t)k_hid_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return hid_print("\r\n");
}

#endif /* !RA8_OFF_TARGET */
