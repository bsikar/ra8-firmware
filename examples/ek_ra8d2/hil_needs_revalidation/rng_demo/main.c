/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/rng_demo/main.c
 * @brief ra8_psa_crypto_random() dump over SCI8 for the bare EK-RA8D2 EVM
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + ``ra8_psa_crypto`` and once a second
 * pulls 32 bytes from ``ra8_psa_crypto_random()`` and emits them as an ASCII hex
 * line on the on-board J-Link OB CDC channel (115200 8N1, TXD8 = PD_02 /
 * RXD8 = PD_03). LED1 toggles once per emit so the heartbeat is also visible
 * without a serial terminal.
 *
 * @warning This demo does NOT prove hardware entropy. The RSIP-E50D TRNG has no
 * working register interface on this silicon (see ra8_rsip_trng_read, which fails
 * closed), so ``ra8_psa_crypto_random`` here returns a DETERMINISTIC software
 * stub (the same stream every boot), NOT secure random bytes. The verdict line
 * says so explicitly. A green here means only that the API path runs and the
 * stub is not stuck -- real entropy needs an FSP-derived RSIP TRNG procedure.
 *
 * Sequence:
 *   1. ``ra8_cgc_init`` -> CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
 *   2. ``ra8_board_uart_console_init(115200)`` -> routes PD_02/PD_03 and
 *      brings up SCI8 at 115200 8N1.
 *   3. ``ra8_psa_crypto_init()``.
 *   4. Loop forever: ``ra8_psa_crypto_random(buf, 32)`` -> emit
 *      ``"rng: <64 hex chars>\r\n"`` -> toggle LED1 -> ``ra8_delay_ms(1000)``.
 *
 * No external transceiver, board, or harness is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_psa_crypto.h"
#include "ra8_time.h"

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_rng_demo_baud      = 115200U, /**< Rng demo baud.      */
  k_rng_demo_period_ms = 1000U,   /**< Rng demo period ms. */
} rng_demo_config_t;

/** @brief Per-emit byte counts. */
typedef enum : uint8_t {
  k_rng_demo_bytes_per_line  = 32U,   /**< Rng demo bytes per line.  */
  k_rng_demo_hex_per_byte    = 2U,    /**< Rng demo hex per byte.    */
  k_rng_demo_nibble_mask     = 0x0FU, /**< Rng demo nibble mask.     */
  k_rng_demo_nibble_shift    = 4U,    /**< Rng demo nibble shift.    */
  k_rng_demo_alpha_threshold = 10U,   /**< Rng demo alpha threshold. */
} rng_demo_byte_t;

/** @brief Fixed prefix and CR/LF tail used on every emit. */
static const uint8_t s_rng_demo_prefix[] = "rng: ";
static const uint8_t s_rng_demo_eol[]    = "\r\n";

/**
 * @brief Verdict line emitted only after a non-stuck sample is dumped.
 * @details Deliberately does NOT claim entropy -- ra8_psa_crypto_random returns a
 *          deterministic software stub on this silicon (no working RSIP TRNG).
 */
static const uint8_t s_rng_demo_pass_msg[] = "rng: PRNG stub OK (deterministic, NOT entropy)\r\n";

/**
 * @brief Park the processor after a fatal setup or entropy-path failure.
 *
 * @details Executes wait-for-interrupt indefinitely so a failed diagnostic
 * cannot continue producing output that could be mistaken for valid samples.
 *
 * @pre A required setup step or runtime entropy check has failed.
 * @pre No remaining foreground recovery operation can make progress safely.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop.
 * @note This terminal path preserves the diagnostic failure state for probing.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rng_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Convert a nibble to its ASCII hex representation.
 *
 * @details Masks the input to four bits and maps values below ten to decimal
 * digits and the remaining values to lowercase hexadecimal letters.
 *
 * @param[in] nibble Lower 4 bits used; upper 4 bits ignored.
 * @return ASCII byte in '0'..'9' or 'a'..'f'.
 * @retval 0x30..0x39 Decimal ASCII digit for nibble values zero through nine.
 * @retval 0x61..0x66 Lowercase ASCII letter for nibble values ten through fifteen.
 *
 * @pre The caller accepts that bits above the low nibble are discarded.
 * @pre The execution character set uses contiguous ASCII digit and letter codes.
 * @post Return value is a printable ASCII character.
 * @post The input value and all shared state remain unchanged.
 * @note The conversion deliberately emits lowercase hexadecimal.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_rng_demo_nibble_to_hex(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_rng_demo_nibble_mask);
  if (n < (uint8_t)k_rng_demo_alpha_threshold) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - (uint8_t)k_rng_demo_alpha_threshold));
}

/**
 * @brief Bring CGC, SysTick, SCI8, LED1, and PSA crypto up.
 *
 * @details Initializes each prerequisite in dependency order and transfers to
 * ::internal_rng_demo_panic_halt on the first error.
 *
 * @pre The function runs during single-threaded application startup.
 * @pre EK-RA8D2 board registers are accessible through the platform mapping.
 * @post On return, timing, console, LED1, and the PSA crypto facade are ready.
 * @post Any prerequisite failure prevents a return to the caller.
 * @note The helper centralizes the fail-closed startup policy for this demo.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rng_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_rng_demo_baud) != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
  if (ra8_psa_crypto_init() != k_ra8_ok) {
    internal_rng_demo_panic_halt();
  }
}

/**
 * @brief Emit one hex line of TRNG output.
 *
 * @details Requests one bounded sample, rejects an all-equal stuck pattern,
 * converts every byte to lowercase hex, and emits the sample plus verdict.
 *
 * @par MC/DC:
 * Compound decision: ``random != ok || sci_write_prefix != ok ||
 * sci_write_payload != ok || sci_write_eol != ok``. Four atomic
 * conditions x N+1 = 5 vectors; the all-ok vector is the steady-state
 * runtime path and each error branch is covered by the host integration
 * tests.
 *
 * @return Error code from the first failing primitive, or k_ra8_ok.
 * @retval k_ra8_ok           Line transmitted.
 * @retval k_ra8_err_hw_error Underlying primitive failed.
 *
 * @pre ::internal_rng_demo_setup_or_halt returned cleanly.
 * @pre The console sink can accept the fixed prefix, payload, and line ending.
 * @post On success 70 bytes (6 prefix + 64 hex + 2 EOL) plus the fixed
 *       ``"trng: entropy OK\r\n"`` verdict line have been sent.
 * @post On failure, the function returns before reporting a successful verdict.
 * @note The PSA backend is deterministic on this silicon and is not claimed as entropy.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_rng_demo_emit_one_line(void)
{
  uint8_t        rng[k_rng_demo_bytes_per_line]                                     = {};
  uint8_t        hex[(uint16_t)k_rng_demo_bytes_per_line * k_rng_demo_hex_per_byte] = {};
  const uint32_t hex_len =
    (uint32_t)((uint16_t)k_rng_demo_bytes_per_line * (uint16_t)k_rng_demo_hex_per_byte);

  if (ra8_psa_crypto_random(rng, sizeof(rng)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
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
    (void)ra8_board_uart_console_write(fail_banner, (size_t)(sizeof(fail_banner) - 1U));
    return k_ra8_err_hw_error;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_rng_demo_bytes_per_line; ++i) {
    const size_t hi = (size_t)i * (size_t)k_rng_demo_hex_per_byte;
    hex[hi] =
      internal_rng_demo_nibble_to_hex((uint8_t)(rng[i] >> (uint8_t)k_rng_demo_nibble_shift));
    hex[hi + 1U] = internal_rng_demo_nibble_to_hex(rng[i]);
  }

  if (ra8_board_uart_console_write(s_rng_demo_prefix, (size_t)(sizeof(s_rng_demo_prefix) - 1U)) !=
      k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  if (ra8_board_uart_console_write(hex, (size_t)hex_len) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  if (ra8_board_uart_console_write(s_rng_demo_eol, (size_t)(sizeof(s_rng_demo_eol) - 1U)) !=
      k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  /* Success-only verdict for the HIL scrape; fire-and-forget so it adds
   * no decision to the documented MC/DC vector set above. */
  (void)ra8_board_uart_console_write(s_rng_demo_pass_msg,
                                     (size_t)(sizeof(s_rng_demo_pass_msg) - 1U));
  return k_ra8_ok;
}

void main(void)
{
  internal_rng_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    if (internal_rng_demo_emit_one_line() != k_ra8_ok) {
      break;
    }
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms(k_rng_demo_period_ms);
  }
  internal_rng_demo_panic_halt();
}
