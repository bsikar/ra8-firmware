/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_spi_probe/src/c6_console.c
 * @brief Bounded console formatters for the ESP32-C6 SPI probe
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Three tiny serialisers -- string, decimal and hexadecimal -- so the probe
 * can narrate itself without dragging newlib's ``printf`` (and its heap and
 * reentrancy machinery) into a bare-metal image. Every loop here is bounded
 * by a constant from ``c6_probe.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_probe.h"
#include "ra8_board_ek_ra8d2.h"

void c6_probe_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (len < (uint32_t)k_c6_probe_str_max && text[len] != '\0') {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

/** @brief Implementation of `c6_probe_put_u32()` -- reversed division loop. */
void c6_probe_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_fmt_dec_digits] = {};
  uint32_t count                       = 0U;
  uint32_t rest                        = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_fmt_dec_radix));
    rest          = rest / (uint32_t)k_c6_fmt_dec_radix;
    count++;
  } while (rest != 0U && count < (uint32_t)k_c6_fmt_dec_digits);

  uint8_t out[k_c6_fmt_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

void c6_probe_put_hex(uint32_t value, uint8_t digits)
{
  if (digits == 0U || digits > (uint8_t)k_c6_fmt_hex_max) {
    return;
  }
  uint8_t out[k_c6_fmt_hex_max] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_fmt_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_fmt_hex_mask);
    out[i]               = (nibble < (uint8_t)k_c6_fmt_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_fmt_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}
