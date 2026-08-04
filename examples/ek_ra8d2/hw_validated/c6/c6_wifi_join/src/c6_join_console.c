/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/src/c6_join_console.c
 * @brief Bounded console formatters for the C6 Wi-Fi join application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Small serialisers -- string, unsigned decimal, hexadecimal, IPv4 dotted quad
 * and MAC address -- plus the banner. They exist so the application can narrate
 * itself without dragging newlib's ``printf`` (and its heap) into a bare-metal
 * image that has no heap at all. Every loop here is bounded by a constant from
 * ``c6_join.h`` (NASA Power of 10 Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_join.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_c6link.h"

void c6_join_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_c6_join_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

/** @brief Implementation of `c6_join_put_u32()` -- reversed division loop. */
void c6_join_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_join_dec_digits] = {};
  uint32_t count                        = 0U;
  uint32_t rest                         = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_join_dec_radix));
    rest          = rest / (uint32_t)k_c6_join_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_c6_join_dec_digits));

  uint8_t out[k_c6_join_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

void c6_join_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_c6_join_hex_digits)) {
    return;
  }
  uint8_t out[k_c6_join_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_join_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_join_hex_mask);
    out[i]               = (nibble < (uint8_t)k_c6_join_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_join_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

/** @brief Implementation of `c6_join_put_ip()` -- four octets, dot-separated. */
void c6_join_put_ip(uint32_t ip)
{
  const uint8_t shifts[k_c6_join_ip_octets] = {
    (uint8_t)k_c6_join_ip_shift_0,
    (uint8_t)k_c6_join_ip_shift_1,
    (uint8_t)k_c6_join_ip_shift_2,
    (uint8_t)k_c6_join_ip_shift_3,
  };
  for (uint8_t i = 0U; i < (uint8_t)k_c6_join_ip_octets; i++) {
    if (i != 0U) {
      c6_join_puts(".");
    }
    c6_join_put_u32((ip >> shifts[i]) & (uint32_t)k_c6_join_ip_mask);
  }
}

void c6_join_put_mac(const ra8_c6link_mac_t* mac)
{
  if (mac == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    if (i != 0U) {
      c6_join_puts(":");
    }
    c6_join_put_hex((uint32_t)mac->octet[i], (uint8_t)k_c6_join_hex_byte);
  }
}

void c6_join_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  c6_join_puts("c6_join: EK-RA8D2 <-> ESP32-C6 Wi-Fi join + DHCP + reachability\r\n");
  c6_join_puts("c6_join: cpuclk0_hz=");
  c6_join_put_u32(cpuclk_hz);
  c6_join_puts(" pclka_hz=");
  c6_join_put_u32(pclka_hz);
  c6_join_puts("\r\n");
  c6_join_puts("c6_join: spi sci=");
  c6_join_put_u32((uint32_t)k_ra8_board_pmod1_sci_channel);
  c6_join_puts(" sck_hz=");
  c6_join_put_u32((uint32_t)k_c6_join_sck_hz);
  c6_join_puts(" frame_bytes=");
  c6_join_put_u32((uint32_t)k_ra8_c6link_frame_bytes);
  c6_join_puts(" max_payload=");
  c6_join_put_u32((uint32_t)k_ra8_c6link_max_payload);
  c6_join_puts("\r\n");
}
