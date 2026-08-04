/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file
 * examples/ek_ra8d2/hil_needs_revalidation/usb_selftest_wlun/src/usb_selftest_wlun_console.c
 * @brief Pattern generator + SCI8 polled console formatters for the wlun loop
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The deterministic per-(LUN,LBA) sector pattern and the SCI8 (J-Link OB
 * CDC, 115200) polled text formatters used by the writable-LUN self-loop.
 * The pattern generator is shared by the device-side media-read callback in
 * ``main.c`` and the host-side verifier in ``usb_selftest_wlun_host.c``; the
 * console formatters stream the host ladder's verdicts. Split out of
 * ``main.c`` so each translation unit stays under the per-file line cap. The
 * full contracts live in ``usb_selftest_wlun_steps.h``.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "usb_selftest_wlun_steps.h"

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* Shared per-(LUN,LBA) pattern */
/* -------------------------------------------------------------------------- */

void wlun_pattern_fill(uint32_t lun, uint32_t lba, UCHAR* out)
{
  for (uint32_t i = 0U; i < (uint32_t)k_wlun_block_size; i++) {
    const uint32_t v = (lun * (uint32_t)k_wlun_pat_lun_mul) + (lba * (uint32_t)k_wlun_pat_lba_mul) +
                       i + (uint32_t)k_wlun_pat_bias;
    out[i]           = (UCHAR)(v & (uint32_t)k_wlun_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

uint8_t wlun_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_wlun_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_wlun_hex_digit_split);
}

uint32_t wlun_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_wlun_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

[[nodiscard]] ra8_err_t wlun_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

[[nodiscard]] ra8_err_t wlun_print(const char* text)
{
  return wlun_sci_write((const uint8_t*)text, wlun_str_len(text));
}

[[nodiscard]] ra8_err_t wlun_print_dec(uint32_t value)
{
  uint8_t  scratch[k_wlun_dec_chars_u32] = {};
  uint8_t  out[k_wlun_dec_chars_u32]     = {};
  uint8_t  count                         = 0U;
  uint32_t v                             = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return wlun_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_wlun_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_wlun_dec_radix));
    v              = v / k_wlun_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return wlun_sci_write(out, (uint32_t)count);
}

[[nodiscard]] ra8_err_t wlun_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_wlun_hex_chars_u32] = {};
  uint8_t width                     = digits;
  if (width > (uint8_t)k_wlun_hex_chars_u32) {
    width = (uint8_t)k_wlun_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_wlun_nibble_bits);
    out[i]              = wlun_nibble_to_hex((value >> shift) & k_wlun_nibble_mask);
  }
  return wlun_sci_write(out, (uint32_t)width);
}

[[nodiscard]] ra8_err_t wlun_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = wlun_print("ra8d2 wlun: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = wlun_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = wlun_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = wlun_print_hex((uint32_t)err, (uint8_t)k_wlun_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return wlun_print("\r\n");
}

#endif /* !RA8_OFF_TARGET */
