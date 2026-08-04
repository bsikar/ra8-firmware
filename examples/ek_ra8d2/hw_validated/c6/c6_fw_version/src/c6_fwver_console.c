/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/src/c6_fwver_console.c
 * @brief Bounded console formatters for the esp-hosted RPC round-trip app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Five tiny serialisers -- string, unsigned decimal, signed decimal,
 * hexadecimal and untrusted text -- plus the banner and the pump-statistics
 * line. They exist so the application can narrate itself without dragging
 * newlib's ``printf`` (and its heap and reentrancy machinery) into a
 * bare-metal image that has no heap at all.
 *
 * Every loop here is bounded by a constant from ``c6_fwver.h``, which is what
 * satisfies NASA Power of 10 Rule 2. ::c6_fwver_put_text is the one that
 * matters for more than that: the bytes it prints come off the wire from the
 * co-processor, so it sanitises rather than trusts.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_fwver.h"
#include "esp_hosted_transport.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_board_ek_ra8d2.h"

/**
 * @enum c6_fwver_ascii_t
 * @brief Printable-ASCII window used to sanitise co-processor text.
 * @details A protocol field is evidence; evidence that can reprogram a
 * terminal is not evidence. Anything outside this window is replaced.
 * @invariant ::k_c6_fwver_ascii_low is the first printable code point and
 *            ::k_c6_fwver_ascii_high the last, so the test is a range test.
 * @par Example:
 * @code
 * const bool ok = (b >= k_c6_fwver_ascii_low) && (b <= k_c6_fwver_ascii_high);
 * @endcode
 * @see c6_fwver_put_text
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_fwver_ascii_low  = 0x20U, /**< Space: lowest printable code point.  */
  k_c6_fwver_ascii_high = 0x7EU, /**< Tilde: highest printable code point. */
  k_c6_fwver_ascii_sub  = '.',   /**< Substitute for anything else.        */
} c6_fwver_ascii_t;

void c6_fwver_puts(const char* text)
{
  if (text == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while ((len < (uint32_t)k_c6_fwver_str_max) && (text[len] != '\0')) {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, (size_t)len);
}

/** @brief Implementation of `c6_fwver_put_u32()` -- reversed division loop. */
void c6_fwver_put_u32(uint32_t value)
{
  uint8_t  digits[k_c6_fwver_dec_digits] = {};
  uint32_t count                         = 0U;
  uint32_t rest                          = value;
  do {
    digits[count] = (uint8_t)('0' + (uint8_t)(rest % (uint32_t)k_c6_fwver_dec_radix));
    rest          = rest / (uint32_t)k_c6_fwver_dec_radix;
    count++;
  } while ((rest != 0U) && (count < (uint32_t)k_c6_fwver_dec_digits));

  uint8_t out[k_c6_fwver_dec_digits] = {};
  for (uint32_t i = 0U; i < count; i++) {
    out[i] = digits[count - 1U - i];
  }
  (void)ra8_board_uart_console_write(out, (size_t)count);
}

/** @brief Implementation of `c6_fwver_put_i32()` -- two's-complement magnitude. */
void c6_fwver_put_i32(int32_t value)
{
  uint32_t magnitude = (uint32_t)value;
  if (value < 0) {
    c6_fwver_puts("-");
    magnitude = ~(uint32_t)value + 1U;
  }
  c6_fwver_put_u32(magnitude);
}

void c6_fwver_put_hex(uint32_t value, uint8_t digits)
{
  if ((digits == 0U) || (digits > (uint8_t)k_c6_fwver_hex_digits)) {
    return;
  }
  uint8_t out[k_c6_fwver_hex_digits] = {};
  for (uint8_t i = 0U; i < digits; i++) {
    const uint8_t shift  = (uint8_t)((digits - 1U - i) * (uint8_t)k_c6_fwver_hex_bits);
    const uint8_t nibble = (uint8_t)((value >> shift) & (uint32_t)k_c6_fwver_hex_mask);
    out[i]               = (nibble < (uint8_t)k_c6_fwver_hex_alpha)
                             ? (uint8_t)('0' + nibble)
                             : (uint8_t)('a' + (uint8_t)(nibble - (uint8_t)k_c6_fwver_hex_alpha));
  }
  (void)ra8_board_uart_console_write(out, (size_t)digits);
}

/** @brief Implementation of `c6_fwver_put_text()` -- truncate, then sanitise. */
void c6_fwver_put_text(const uint8_t* text, size_t len)
{
  if ((text == nullptr) || (len == 0U)) {
    return;
  }
  const size_t take = (len < (size_t)k_c6_fwver_text_max) ? len : (size_t)k_c6_fwver_text_max;
  uint8_t      out[k_c6_fwver_text_max] = {};
  for (size_t i = 0U; i < take; i++) {
    const uint8_t byte = text[i];
    const bool    printable =
      (byte >= (uint8_t)k_c6_fwver_ascii_low) && (byte <= (uint8_t)k_c6_fwver_ascii_high);
    out[i] = printable ? byte : (uint8_t)k_c6_fwver_ascii_sub;
  }
  (void)ra8_board_uart_console_write(out, take);
}

void c6_fwver_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  c6_fwver_puts("c6_fwver: EK-RA8D2 <-> ESP32-C6 esp-hosted RPC round-trip\r\n");
  c6_fwver_puts("c6_fwver: cpuclk0_hz=");
  c6_fwver_put_u32(cpuclk_hz);
  c6_fwver_puts(" pclka_hz=");
  c6_fwver_put_u32(pclka_hz);
  c6_fwver_puts("\r\n");
  c6_fwver_puts("c6_fwver: spi sci=");
  c6_fwver_put_u32((uint32_t)k_ra8_board_pmod1_sci_channel);
  c6_fwver_puts(" sck_hz=");
  c6_fwver_put_u32((uint32_t)k_c6_fwver_sck_hz);
  c6_fwver_puts(" frame_bytes=");
  c6_fwver_put_u32((uint32_t)k_c6_fwver_frame_bytes);
  c6_fwver_puts(" max_payload=");
  c6_fwver_put_u32((uint32_t)MAX_PAYLOAD_SIZE);
  c6_fwver_puts("\r\n");
}

void c6_fwver_print_pump(const char* label, const c6_fwver_pump_stats_t* stats)
{
  if ((label == nullptr) || (stats == nullptr)) {
    return;
  }
  c6_fwver_puts("c6_fwver: pump ");
  c6_fwver_puts(label);
  c6_fwver_puts(" xfers=");
  c6_fwver_put_u32((uint32_t)stats->transfers);
  c6_fwver_puts(" frames=");
  c6_fwver_put_u32((uint32_t)stats->frames);
  c6_fwver_puts(" idle=");
  c6_fwver_put_u32((uint32_t)stats->idle);
  c6_fwver_puts(" badcsum=");
  c6_fwver_put_u32((uint32_t)stats->bad_checksum);
  c6_fwver_puts(" malformed=");
  c6_fwver_put_u32((uint32_t)stats->malformed);
  c6_fwver_puts(" hs_timeouts=");
  c6_fwver_put_u32((uint32_t)stats->hs_timeouts);
  c6_fwver_puts(" ifnum_defect=");
  c6_fwver_put_u32((uint32_t)stats->ifnum_defect);
  c6_fwver_puts(stats->bus_error ? " bus_error=y" : " bus_error=n");
  c6_fwver_puts(stats->sink_stopped ? " stopped=sink" : " stopped=budget");
  c6_fwver_puts("\r\n");
}
