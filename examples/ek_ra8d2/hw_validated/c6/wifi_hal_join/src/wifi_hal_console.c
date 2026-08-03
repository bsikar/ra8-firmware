/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_console.c
 * @brief Bounded console formatters for the HAL Wi-Fi join application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Small serialisers -- string, unsigned decimal, hexadecimal, IPv4 dotted quad
 * and MAC address -- plus the banner. They exist so the application can narrate
 * itself without dragging newlib's ``printf`` (and its heap) into a bare-metal
 * image that has no heap at all. Every loop here is bounded by a constant from
 * ``wifi_hal_join.h`` (NASA Power of 10 Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_c6link.h"
#include "ra8_wifi.h"
#include "wifi_hal_join.h"

void wifi_hal_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_wifi_hal_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

/** @brief Implementation of `wifi_hal_put_u32()` -- reversed division loop. */
void wifi_hal_put_u32(uint32_t value)
{
  uint8_t  digits[k_wifi_hal_dec_digits] = {};
  uint32_t count                         = 0U;
  uint32_t rest                          = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_wifi_hal_dec_radix));
    rest          = rest / (uint32_t)k_wifi_hal_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_wifi_hal_dec_digits));

  uint8_t out[k_wifi_hal_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

void wifi_hal_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_wifi_hal_hex_digits)) {
    return;
  }
  uint8_t out[k_wifi_hal_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_wifi_hal_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_wifi_hal_hex_mask);
    out[i]               = (nibble < (uint8_t)k_wifi_hal_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_wifi_hal_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

/** @brief Implementation of `wifi_hal_put_ip()` -- four octets, dot-separated. */
void wifi_hal_put_ip(uint32_t ip)
{
  const uint8_t shifts[k_wifi_hal_ip_octets] = {
    (uint8_t)k_wifi_hal_ip_shift_0,
    (uint8_t)k_wifi_hal_ip_shift_1,
    (uint8_t)k_wifi_hal_ip_shift_2,
    (uint8_t)k_wifi_hal_ip_shift_3,
  };
  for (uint8_t i = 0U; i < (uint8_t)k_wifi_hal_ip_octets; i++) {
    if (i != 0U) {
      wifi_hal_puts(".");
    }
    wifi_hal_put_u32((ip >> shifts[i]) & (uint32_t)k_wifi_hal_ip_mask);
  }
}

void wifi_hal_put_mac(const ra8_wifi_mac_t* mac)
{
  if (mac == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_wifi_mac_bytes; i++) {
    if (i != 0U) {
      wifi_hal_puts(":");
    }
    wifi_hal_put_hex((uint32_t)mac->octet[i], (uint8_t)k_wifi_hal_hex_byte);
  }
}

void wifi_hal_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  wifi_hal_puts("wifi_hal: EK-RA8D2 <-> ESP32-C6 Wi-Fi join + DHCP via ra8_wifi\r\n");
  wifi_hal_puts("wifi_hal: cpuclk0_hz=");
  wifi_hal_put_u32(cpuclk_hz);
  wifi_hal_puts(" pclka_hz=");
  wifi_hal_put_u32(pclka_hz);
  wifi_hal_puts("\r\n");
  wifi_hal_puts("wifi_hal: spi sci=");
  wifi_hal_put_u32((uint32_t)k_ra8_board_pmod1_sci_channel);
  wifi_hal_puts(" sck_hz=");
  wifi_hal_put_u32((uint32_t)k_wifi_hal_sck_hz);
  wifi_hal_puts(" frame_bytes=");
  wifi_hal_put_u32((uint32_t)k_ra8_c6link_frame_bytes);
  wifi_hal_puts("\r\n");
}
