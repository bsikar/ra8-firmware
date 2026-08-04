/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file usb_selftest_console.c
 * @brief SCI8 / J-Link OB CDC console + text formatters (see header)
 *
 * @details
 * The shared console for the soak self-test: a thin wrapper over polled SCI8
 * writes plus minimal heap-free decimal / hex / fail-line formatters. Split out
 * of main.c so every TU prints through one path.
 *
 *
 * @since 0.1.0
 */

#include "usb_selftest_console.h"

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "usb_selftest_common.h"

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
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::selftest_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_selftest_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print(const char* text)
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
[[nodiscard]] ra8_err_t selftest_print_dec(uint32_t value)
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
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_selftest_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_print_hex(uint32_t value, uint8_t digits)
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
[[nodiscard]] ra8_err_t selftest_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = selftest_print("ra8d2 selftest: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print_hex((uint32_t)err, (uint8_t)k_selftest_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return selftest_print("\r\n");
}
