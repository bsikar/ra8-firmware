/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_hci/src/c6_ble_console.c
 * @brief Bounded UART console formatters for the C6 BLE HCI example.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_ble_hci.h"
#include "ra8_ble.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_c6link.h"

typedef enum : uint8_t {
  k_c6_ble_ascii_low  = 0x20U,
  k_c6_ble_ascii_high = 0x7EU,
  k_c6_ble_ascii_sub  = '.',
} c6_ble_ascii_t;

void c6_ble_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_c6_ble_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

void c6_ble_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_ble_dec_digits] = {};
  uint32_t count                       = 0U;
  uint32_t rest                        = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_ble_dec_radix));
    rest          = rest / (uint32_t)k_c6_ble_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_c6_ble_dec_digits));

  uint8_t out[k_c6_ble_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

void c6_ble_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_c6_ble_hex_digits)) {
    return;
  }
  uint8_t out[k_c6_ble_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_ble_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_ble_hex_mask);
    out[i]               = (nibble < (uint8_t)k_c6_ble_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_ble_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

void c6_ble_put_text(const uint8_t* text, size_t len)
{
  if ((text == nullptr) || (len == 0U)) {
    return;
  }
  uint8_t      out[k_c6_ble_text_max] = {};
  const size_t capped = (len < (size_t)k_c6_ble_text_max) ? len : (size_t)k_c6_ble_text_max;
  for (size_t i = 0U; i < capped; i++) {
    const uint8_t ch = text[i];
    out[i]           = ((ch >= (uint8_t)k_c6_ble_ascii_low) && (ch <= (uint8_t)k_c6_ble_ascii_high))
                         ? ch
                         : (uint8_t)k_c6_ble_ascii_sub;
  }
  (void)ra8_board_uart_console_write(out, capped);
}

void c6_ble_put_bd_addr(const uint8_t bd_addr[k_ra8_ble_addr_bytes])
{
  if (bd_addr == nullptr) {
    return;
  }
  /* Read_BD_ADDR returns 6 octets LSB-first; print conventional MSB-first MAC order. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ble_addr_bytes; i++) {
    if (i != 0U) {
      c6_ble_puts(":");
    }
    const uint8_t idx = (uint8_t)((uint8_t)k_ra8_ble_addr_bytes - 1U - i);
    c6_ble_put_hex((uint32_t)bd_addr[idx], (uint8_t)k_c6_ble_hex_byte);
  }
}

void c6_ble_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  c6_ble_puts("c6_ble: EK-RA8D2 <-> ESP32-C6 BLE HCI over ra8_c6link\r\n");
  c6_ble_puts("c6_ble: cpuclk0_hz=");
  c6_ble_put_u32(cpuclk_hz);
  c6_ble_puts(" pclka_hz=");
  c6_ble_put_u32(pclka_hz);
  c6_ble_puts("\r\n");
  c6_ble_puts("c6_ble: spi sci=");
  c6_ble_put_u32((uint32_t)k_ra8_board_pmod1_sci_channel);
  c6_ble_puts(" sck_hz=");
  c6_ble_put_u32((uint32_t)k_c6_ble_sck_hz);
  c6_ble_puts(" frame_bytes=");
  c6_ble_put_u32((uint32_t)k_ra8_c6link_frame_bytes);
  c6_ble_puts("\r\n");
}
