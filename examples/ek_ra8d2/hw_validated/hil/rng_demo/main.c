/**
 * @file examples/ek_ra8d2/rng_demo/main.c
 * @brief PSA TRNG dump over SCI8 for the bare EK-RA8D2 EVM
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + ``ra_psa_crypto`` and
 * once a second pulls 32 bytes from ``ra_psa_crypto_random()`` and
 * emits them as an ASCII hex line on the on-board J-Link OB CDC
 * channel (115200 8N1, TXD8 = PD_02 / RXD8 = PD_03). LED1 toggles
 * once per emit so the heartbeat is also visible without a serial
 * terminal.
 *
 * Sequence:
 *   1. ``ra_cgc_init`` -> CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
 *   2. ``ra_pfs_route_peripheral`` for PD_02/PD_03 SCI-async pins.
 *   3. ``ra_sci_init(8, 115200 8N1)``.
 *   4. ``ra_psa_crypto_init()``.
 *   5. Loop forever: ``ra_psa_crypto_random(buf, 32)`` -> emit
 *      ``"trng: <64 hex chars>\r\n"`` -> toggle LED1 -> ``ra_delay_ms(1000)``.
 *
 * No external transceiver, board, or harness is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_psa_crypto.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_rng_demo_baud        = 115200U,
  k_rng_demo_period_ms   = 1000U,
  k_rng_demo_sci_channel = 8U,
} rng_demo_config_t;

/** @brief Per-emit byte counts. */
typedef enum : uint8_t {
  k_rng_demo_bytes_per_line  = 32U,
  k_rng_demo_hex_per_byte    = 2U,
  k_rng_demo_nibble_mask     = 0x0FU,
  k_rng_demo_nibble_shift    = 4U,
  k_rng_demo_alpha_threshold = 10U,
} rng_demo_byte_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8). */
static const ra_port_pin_t k_rng_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_rng_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Fixed prefix and CR/LF tail used on every emit. */
static const uint8_t k_rng_demo_prefix[] = "trng: ";
static const uint8_t k_rng_demo_eol[]    = "\r\n";

/** @brief Park forever after a fatal init failure. */
static void rng_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Convert a nibble to its ASCII hex representation.
 *
 * @param[in] nibble Lower 4 bits used; upper 4 bits ignored.
 * @return ASCII byte in '0'..'9' or 'a'..'f'.
 *
 * @pre None.
 * @post Return value is a printable ASCII character.
 *
 * @since 0.1.0
 */
static uint8_t rng_demo_nibble_to_hex(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_rng_demo_nibble_mask);
  if (n < (uint8_t)k_rng_demo_alpha_threshold) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - (uint8_t)k_rng_demo_alpha_threshold));
}

/**
 * @brief Route PD_02 / PD_03 to SCI8 TXD/RXD via the PFS PSEL field.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success PD_02 and PD_03 are in SCI-async mode (PSEL = 0x04).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t rng_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_rng_demo_pin_txd, k_ra_psel_sci_async, "rng_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_rng_demo_pin_rxd, k_ra_psel_sci_async, "rng_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LED1 + PSA crypto up. */
static void rng_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (rng_demo_pins_init() != k_ra_ok) {
    rng_demo_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_rng_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_rng_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    rng_demo_panic_halt();
  }
  if (ra_psa_crypto_init() != k_ra_ok) {
    rng_demo_panic_halt();
  }
}

/**
 * @brief Emit one hex line of TRNG output.
 *
 * @par MC/DC:
 * Compound decision: ``random != ok || sci_write_prefix != ok ||
 * sci_write_payload != ok || sci_write_eol != ok``. Four atomic
 * conditions x N+1 = 5 vectors; the all-ok vector is the steady-state
 * runtime path and each error branch is covered by the host integration
 * tests.
 *
 * @return Error code from the first failing primitive, or k_ra_ok.
 * @retval k_ra_ok           Line transmitted.
 * @retval k_ra_err_hw_error Underlying primitive failed.
 *
 * @pre rng_demo_setup_or_halt() returned cleanly.
 * @post On success 70 bytes (6 prefix + 64 hex + 2 EOL) have been sent.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t rng_demo_emit_one_line(void)
{
  uint8_t        rng[k_rng_demo_bytes_per_line]                                     = {};
  uint8_t        hex[(uint16_t)k_rng_demo_bytes_per_line * k_rng_demo_hex_per_byte] = {};
  const uint32_t hex_len =
    (uint32_t)((uint16_t)k_rng_demo_bytes_per_line * (uint16_t)k_rng_demo_hex_per_byte);

  if (ra_psa_crypto_random(rng, sizeof(rng)) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  /* Stuck-bit detector: if every byte in the 32-byte sample is the
   * same value, the TRNG entropy path is almost certainly broken
   * (probability of 32 random bytes all matching is 1 in 2^248).
   * Without this check the demo would happily print "trng: 00...00"
   * or "trng: ff...ff" forever and the probe would pass. */
  bool all_same = true;
  for (uint8_t i = 1U; i < (uint8_t)k_rng_demo_bytes_per_line; ++i) {
    if (rng[i] != rng[0]) {
      all_same = false;
      break;
    }
  }
  if (all_same) {
    const uint8_t fail_banner[] = "rng_demo: FAIL stuck\r\n";
    (void)ra_sci_write_polling((uint8_t)k_rng_demo_sci_channel,
                               fail_banner,
                               (uint32_t)(sizeof(fail_banner) - 1U));
    return k_ra_err_hw_error;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_rng_demo_bytes_per_line; ++i) {
    const size_t hi = (size_t)i * (size_t)k_rng_demo_hex_per_byte;
    hex[hi]         = rng_demo_nibble_to_hex((uint8_t)(rng[i] >> (uint8_t)k_rng_demo_nibble_shift));
    hex[hi + 1U]    = rng_demo_nibble_to_hex(rng[i]);
  }

  if (ra_sci_write_polling((uint8_t)k_rng_demo_sci_channel,
                           k_rng_demo_prefix,
                           (uint32_t)(sizeof(k_rng_demo_prefix) - 1U)) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_sci_write_polling((uint8_t)k_rng_demo_sci_channel, hex, hex_len) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_sci_write_polling((uint8_t)k_rng_demo_sci_channel,
                           k_rng_demo_eol,
                           (uint32_t)(sizeof(k_rng_demo_eol) - 1U)) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  rng_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (rng_demo_emit_one_line() != k_ra_ok) {
      break;
    }
    (void)ra_board_led_toggle(k_ra_board_led1);
    ra_delay_ms(k_rng_demo_period_ms);
  }
  rng_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
