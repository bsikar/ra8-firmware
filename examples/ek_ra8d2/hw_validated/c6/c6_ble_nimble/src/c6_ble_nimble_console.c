/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_nimble/src/c6_ble_nimble_console.c
 * @brief Bounded, thread-safe UART console formatters for `c6_ble_nimble`.
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

#include "c6_ble_nimble.h"
#include "ra8_ble.h"
#include "ra8_board_ek_ra8d2.h"
#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

#ifndef RA8_OFF_TARGET
static TX_MUTEX s_console_mtx;
static uint8_t  s_console_mtx_ready = 0U;

void c6_ble_nimble_console_lock_init(void)
{
  static CHAR s_mtx_name[] = {'c', '6', '_', 'c', 'o', 'n', '\0'};
  if (s_console_mtx_ready == 0U) {
    if (tx_mutex_create(&s_console_mtx, s_mtx_name, TX_INHERIT) == TX_SUCCESS) {
      s_console_mtx_ready = 1U;
    }
  }
}

void c6_ble_nimble_console_lock(void)
{
  if ((s_console_mtx_ready != 0U) && (tx_thread_identify() != TX_NULL)) {
    (void)tx_mutex_get(&s_console_mtx, TX_WAIT_FOREVER);
  }
}

void c6_ble_nimble_console_unlock(void)
{
  if ((s_console_mtx_ready != 0U) && (tx_thread_identify() != TX_NULL)) {
    (void)tx_mutex_put(&s_console_mtx);
  }
}
#else
void c6_ble_nimble_console_lock_init(void) {}
void c6_ble_nimble_console_lock(void) {}
void c6_ble_nimble_console_unlock(void) {}
#endif

void c6_ble_nimble_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_c6_ble_nimble_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

void c6_ble_nimble_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_ble_nimble_dec_digits] = {};
  uint32_t count                              = 0U;
  uint32_t rest                               = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_ble_nimble_dec_radix));
    rest          = rest / (uint32_t)k_c6_ble_nimble_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_c6_ble_nimble_dec_digits));

  uint8_t out[k_c6_ble_nimble_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

void c6_ble_nimble_put_i32(int32_t value)
{
  if (value < 0) {
    c6_ble_nimble_puts("-");
    const uint32_t mag = (uint32_t)(-(int64_t)value);
    c6_ble_nimble_put_u32(mag);
    return;
  }
  c6_ble_nimble_put_u32((uint32_t)value);
}

void c6_ble_nimble_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_c6_ble_nimble_hex_digits)) {
    return;
  }
  uint8_t out[k_c6_ble_nimble_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_ble_nimble_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_ble_nimble_hex_mask);
    out[i] = (nibble < (uint8_t)k_c6_ble_nimble_hex_alpha)
               ? (uint8_t)('0' + nibble)
               : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_ble_nimble_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

void c6_ble_nimble_put_bd_addr_le(const uint8_t bd_addr[k_ra8_ble_addr_bytes])
{
  if (bd_addr == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ble_addr_bytes; i++) {
    if (i != 0U) {
      c6_ble_nimble_puts(":");
    }
    const uint8_t idx = (uint8_t)((uint8_t)k_ra8_ble_addr_bytes - 1U - i);
    c6_ble_nimble_put_hex((uint32_t)bd_addr[idx], (uint8_t)k_c6_ble_nimble_hex_byte);
  }
}

void c6_ble_nimble_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  c6_ble_nimble_puts("\r\n============================================================\r\n");
  c6_ble_nimble_puts("c6_ble_nimble: NimBLE 1.10.0 over C6 SPI HCI (ble_hci_c6link)\r\n");
  c6_ble_nimble_puts("c6_ble_nimble: CPUCLK=");
  c6_ble_nimble_put_u32(cpuclk_hz);
  c6_ble_nimble_puts(" Hz PCLKA=");
  c6_ble_nimble_put_u32(pclka_hz);
  c6_ble_nimble_puts(" Hz\r\n");
  c6_ble_nimble_puts("============================================================\r\n");
}
