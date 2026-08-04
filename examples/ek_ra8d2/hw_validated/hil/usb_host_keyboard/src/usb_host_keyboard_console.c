/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_keyboard/src/usb_host_keyboard_console.c
 * @brief SCI8 -> J-Link OB CDC console formatters for usb_host_keyboard
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The text-formatting console cluster carved out of `usb_host_keyboard/main.c`
 * so each translation unit stays under the 1000-line cap. These helpers push
 * fixed strings, decimal counts, and fixed-width hex tokens over SCI8 (the
 * J-Link OB CDC bridge, 115200) using polled TX. The host-side ladder in
 * main.c calls them; the prototypes and the shared formatter enums live in
 * `usb_host_keyboard_steps.h`. Pure code move -- no logic change.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "usb_host_keyboard_steps.h"

#ifndef RA8_OFF_TARGET

uint8_t hid_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_hid_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_hid_hex_digit_split);
}

uint32_t hid_str_len(const char* text)
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

[[nodiscard]] ra8_err_t hid_sci_write(const uint8_t* data, uint32_t len)
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
