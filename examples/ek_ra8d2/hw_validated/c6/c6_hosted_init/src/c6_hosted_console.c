/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_hosted_init/src/c6_hosted_console.c
 * @brief Bounded console formatters for the esp-hosted port bring-up app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Four tiny serialisers -- string, unsigned decimal, signed decimal and
 * hexadecimal -- plus the two printers that turn a pin identity into a
 * console field. They exist so the application can narrate itself without
 * dragging newlib's ``printf`` (and its heap and reentrancy machinery) into
 * a bare-metal image with no heap at all.
 *
 * Every loop here is bounded by a constant from ``c6_hosted.h``, which is
 * what satisfies NASA Power of 10 Rule 2. Neither pin printer writes an
 * EK-RA8D2 pin number: both decode whatever the port's pin table and the
 * ``H_GPIO_*`` macro pairs resolve to at run time.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_hosted.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_esp_hosted_pins.h"

void c6_hosted_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_c6_hosted_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

/** @brief Implementation of `c6_hosted_put_u32()` -- reversed division loop. */
void c6_hosted_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_hosted_dec_digits] = {};
  uint32_t count                          = 0U;
  uint32_t rest                           = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_hosted_dec_radix));
    rest          = rest / (uint32_t)k_c6_hosted_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_c6_hosted_dec_digits));

  uint8_t out[k_c6_hosted_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

/** @brief Implementation of `c6_hosted_put_i32()` -- two's-complement magnitude. */
void c6_hosted_put_i32(int32_t value)
{
  uint32_t magnitude = (uint32_t)value;
  if (value < 0) {
    c6_hosted_puts("-");
    magnitude = ~(uint32_t)value + 1U;
  }
  c6_hosted_put_u32(magnitude);
}

void c6_hosted_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_c6_hosted_hex_digits)) {
    return;
  }
  uint8_t out[k_c6_hosted_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_hosted_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_hosted_hex_mask);
    out[i]               = (nibble < (uint8_t)k_c6_hosted_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_hosted_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

void c6_hosted_print_gpio(const char* label, const void* gpio_port, int32_t gpio_pin)
{
  c6_hosted_puts(label);
  if (gpio_pin < 0) {
    c6_hosted_puts("=unwired");
    return;
  }
  c6_hosted_puts("=port");
  c6_hosted_put_u32((uint32_t)(uintptr_t)gpio_port);
  c6_hosted_puts(".pin");
  c6_hosted_put_i32(gpio_pin);
}

/** @brief Implementation of `c6_hosted_print_pin()` -- unpacks, then delegates. */
void c6_hosted_print_pin(const char* label, ra8_esp_hosted_pin_t pin)
{
  const bool wired = ((uint16_t)pin != (uint16_t)k_ra8_pin_none);
  c6_hosted_print_gpio(label,
                       (const void*)(uintptr_t)RA8_PIN_PORT(pin),
                       wired ? (int32_t)RA8_PIN_PIN(pin) : -1);
}
