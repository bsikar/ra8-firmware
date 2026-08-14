/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream/src/c6_cam_console.c
 * @brief Heap-free console formatting for camera livestream diagnostics.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_camera_livestream.h"
#include "ra8_board_ek_ra8d2.h"

/** @brief Bounded formatting sizes and IPv4 extraction constants. */
typedef enum : uint32_t {
  k_c6_cam_console_text_max = 512U,
  k_c6_cam_decimal_digits   = 10U,
  k_c6_cam_ipv4_high_shift  = 24U,
  k_c6_cam_ipv4_octet_mask  = 0xFFU,
} c6_cam_console_t;

void c6_cam_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t length = 0U;
  while ((length < (uint32_t)k_c6_cam_console_text_max) && (text[length] != '\0')) {
    length++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)length);
}

void c6_cam_put_u32(uint32_t value)
{
  uint8_t  reverse[k_c6_cam_decimal_digits] = {};
  uint8_t  output[k_c6_cam_decimal_digits]  = {};
  uint32_t count                            = 0U;
  do {
    reverse[count] = (uint8_t)('0' + (uint8_t)(value % (uint32_t)k_c6_cam_decimal_digits));
    value /= (uint32_t)k_c6_cam_decimal_digits;
    count++;
  } while ((value != 0U) && (count < (uint32_t)k_c6_cam_decimal_digits));
  for (uint32_t i = 0U; i < count; i++) {
    output[i] = reverse[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(output, (size_t)count);
}

void c6_cam_put_ip(uint32_t ip)
{
  static const uint8_t k_shifts[4] = {(uint8_t)k_c6_cam_ipv4_high_shift, 16U, 8U, 0U};
  for (uint32_t i = 0U; i < 4U; i++) {
    if (i != 0U) {
      c6_cam_puts(".");
    }
    c6_cam_put_u32((ip >> k_shifts[i]) & (uint32_t)k_c6_cam_ipv4_octet_mask);
  }
}
