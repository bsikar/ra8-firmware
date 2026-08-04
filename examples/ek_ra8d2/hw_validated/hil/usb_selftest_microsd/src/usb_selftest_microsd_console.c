/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file
 * examples/ek_ra8d2/hw_validated/hil/usb_selftest_microsd/src/usb_selftest_microsd_console.c
 * @brief SCI8 (J-Link OB CDC) console formatters for the USB-microSD self-test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of ``main.c`` to keep every translation unit under the
 * line-count cap. Holds the bounded string/hex/decimal text formatters that
 * stream verdicts over SCI8 at 115200 to the J-Link OB CDC bridge. The
 * internal nibble/length/byte-write helpers stay file-static here; the
 * public ``microsd_print*`` entry points are declared in
 * ``usb_selftest_microsd_steps.h`` and consumed by both ``main.c`` and the
 * host/device worker sibling.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "usb_selftest_microsd_steps.h"

#ifndef RA8_OFF_TARGET

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
static uint8_t microsd_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_microsd_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_microsd_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_microsd_print_cap).
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
 * @post Return value never exceeds ::k_microsd_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t microsd_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_microsd_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over the J-Link OB CDC console polled.
 *
 * @details Thin wrapper fixing the console channel onto the EK-RA8D2 BSP
 * SCI8 console transport.
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
[[nodiscard]] static ra8_err_t microsd_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

[[nodiscard]] ra8_err_t microsd_print(const char* text)
{
  return microsd_sci_write((const uint8_t*)text, microsd_str_len(text));
}

/** @brief Implementation of `microsd_print_dec()` -- digit-reversal scratch. */
[[nodiscard]] ra8_err_t microsd_print_dec(uint32_t value)
{
  uint8_t  scratch[k_microsd_dec_chars_u32] = {};
  uint8_t  out[k_microsd_dec_chars_u32]     = {};
  uint8_t  count                            = 0U;
  uint32_t v                                = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return microsd_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_microsd_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_microsd_dec_radix));
    v              = v / k_microsd_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return microsd_sci_write(out, (uint32_t)count);
}

/** @brief Implementation of `microsd_print_hex()` -- width clamped to 8. */
[[nodiscard]] ra8_err_t microsd_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_microsd_hex_chars_u32] = {};
  uint8_t width                        = digits;
  if (width > (uint8_t)k_microsd_hex_chars_u32) {
    width = (uint8_t)k_microsd_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_microsd_nibble_bits);
    out[i]              = microsd_nibble_to_hex((value >> shift) & k_microsd_nibble_mask);
  }
  return microsd_sci_write(out, (uint32_t)width);
}

[[nodiscard]] ra8_err_t microsd_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = microsd_print("ra8d2 microsd: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = microsd_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = microsd_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = microsd_print_hex((uint32_t)err, (uint8_t)k_microsd_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return microsd_print("\r\n");
}

#endif /* !RA8_OFF_TARGET */
