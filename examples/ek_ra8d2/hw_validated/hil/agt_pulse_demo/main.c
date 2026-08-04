/**
 * @file examples/ek_ra8d2/hw_validated/hil/agt_pulse_demo/main.c
 * @brief AGT pulse-output / output-compare demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + UART (SCI8 on PD_02 / PD_03) and arms AGT0
 * in pulse-output mode via the new `ra8_agt_start_pulse_output` HAL
 * surface. The HAL programmes TMOD = 001b in AGTMR1 (HUM Ch 24.3.4
 * "Pulse Output Mode" p 1177), wires AGTCMA as the duty target so the
 * AGTOAn pin toggles on compare-match A, and sets AGTIOC.TOE so the
 * AGTOn pin tracks counter underflows.
 *
 * The demo polls AGTCR.TUNDF in the foreground loop. Each underflow:
 *   1. Stops + re-arms the channel to clear AGTCR.TUNDF.
 *   2. Toggles board LED1 so a scope / human can see the cadence.
 *   3. Emits ``agt_pulse: tick=XXXXXXXX\r\n`` over SCI8.
 *
 * The HIL gate scrapes ``agt_pulse: tick=`` to confirm bring-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_agt.h"
#include "ra8_agt_regs.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_agt_pulse_baud    = 115200U, /**< AGT pulse baud.    */
  k_agt_pulse_poll_ms = 10U,     /**< AGT pulse poll ms. */
} agt_pulse_const_t;

/** @brief AGT channel + pulse-output settings. */
typedef enum : uint16_t {
  k_agt_pulse_channel = 0U,      /**< AGT pulse channel.         */
  k_agt_pulse_period  = 0x7FFFU, /**< ~1 Hz at the fake default. */
  k_agt_pulse_duty    = 0x3FFFU, /**< 50% via compare-match A.   */
} agt_pulse_timer_t;

/** @brief Hex-digit width + mask for the per-tick banner formatter. */
typedef enum : uint32_t {
  k_agt_pulse_hex_digits = 8U,   /**< AGT pulse hex digits. */
  k_agt_pulse_hex_shift  = 4U,   /**< AGT pulse hex shift.  */
  k_agt_pulse_hex_mask   = 0xFU, /**< AGT pulse hex mask.   */
} agt_pulse_fmt_t;

/** @brief Hex print map for the tick counter. */
static const char k_agt_pulse_hex_chars[] = "0123456789ABCDEF";

/** @brief Tick-line prefix and CR/LF tail. */
static const uint8_t k_agt_pulse_msg_head[] = "agt_pulse: tick=";
static const uint8_t k_agt_pulse_msg_tail[] = "\r\n";

/**
 * @brief Spin-halt the core if any init step fails.
 *
 * @details
 * Boot helper. Uses `wfi` so an attached debugger can break in and
 * inspect the failed init.
 *
 * @pre Called only from `main()` / the boot path.
 * @pre No caller relies on the function returning.
 *
 * @post Core is halted in a tight `wfi` loop.
 * @post No registers other than `pc` change after the first `wfi`.
 *
 * @note Not thread-safe; bring-up is single-threaded by construction.
 * @since 0.1.0
 */
static void agt_pulse_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up CGC, SysTick, SCI8 and LED1.
 *
 * @details
 * Sequence: ra8_cgc_init -> ra8_time_init -> ra8_board_uart_console_init
 * -> ra8_board_led_init. Any failure goes straight to panic-halt so the
 * HIL gate sees no `agt_pulse:` banner.
 *
 * @pre Called once from `main()` before `ra8_isr_globals_enable`.
 * @pre Stack is sized for the SCI bring-up call chain.
 *
 * @post All peripherals named above are powered, configured, and ready.
 * @post No mutable global beyond CGC / SCI driver state is touched.
 *
 * @note Not thread-safe (boot path).
 * @since 0.1.0
 */
static void agt_pulse_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    agt_pulse_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    agt_pulse_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    agt_pulse_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_agt_pulse_baud) != k_ra8_ok) {
    agt_pulse_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    agt_pulse_panic_halt();
  }
}

/**
 * @brief Arm AGT0 in pulse-output / output-compare mode.
 *
 * @par MC/DC:
 * Single atomic decision: ``ra8_agt_start_pulse_output != ok``. Two
 * vectors -- happy path (HIL bench) and the bad-channel reject path
 * covered in `tests/test_ra8_agt.c`.
 *
 * @return ``ra8_err_t`` from the HAL call.
 *
 * @pre CGC + SCI8 + LED bring-up has finished.
 * @pre IRQs are masked or the demo is single-threaded.
 *
 * @post AGT0 is running in pulse-output mode (HUM Ch 24.3.4 p 1177).
 * @post AGTCR.TSTART reads 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t agt_pulse_arm(void)
{
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = (uint16_t)k_agt_pulse_period,
    .duty     = (uint16_t)k_agt_pulse_duty,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = k_ra8_agt_output_polarity_active_high,
    .compare  = k_ra8_agt_pulse_compare_a,
  };
  return ra8_agt_start_pulse_output((uint8_t)k_agt_pulse_channel, &cfg);
}

/**
 * @brief Render the 32-bit tick counter into an 8-byte ASCII-hex buf.
 *
 * @details
 * Big-endian hex render: digits[0] = most-significant nibble. Caller
 * passes a buffer >= 8 bytes long.
 *
 * @param[in]  tick    Tick counter value to render.
 * @param[out] digits  Destination buffer (>= 8 bytes).
 *
 * @pre ``digits`` is non-NULL and >= 8 bytes.
 * @pre The caller will not interpret ``digits`` as NUL-terminated.
 *
 * @post ``digits`` contains the 8 ASCII hex digits of ``tick``.
 * @post No global state is touched.
 *
 * @note Not thread-safe (caller owns the buffer).
 * @since 0.1.0
 */
static void agt_pulse_format_tick(uint32_t tick, uint8_t* digits)
{
  for (uint32_t i = 0U; i < (uint32_t)k_agt_pulse_hex_digits; ++i) {
    const uint32_t shift =
      ((uint32_t)k_agt_pulse_hex_digits - 1U - i) * (uint32_t)k_agt_pulse_hex_shift;
    const uint32_t nib = (tick >> shift) & (uint32_t)k_agt_pulse_hex_mask;
    digits[i]          = (uint8_t)k_agt_pulse_hex_chars[nib];
  }
}

/**
 * @brief Emit one tick banner over SCI8.
 *
 * @details
 * Format: ``agt_pulse: tick=XXXXXXXX\r\n``. The HIL probe matches on
 * the ``agt_pulse: tick=`` prefix so any non-zero tick passes.
 *
 * @param[in] tick Current tick counter value.
 *
 * @return ``ra8_err_t`` from the SCI driver (first failure wins).
 *
 * @pre SCI8 has been initialised.
 * @pre ``tick`` fits in 32 bits.
 *
 * @post Up to 26 bytes have been pushed into the SCI8 TX path.
 * @post No mutable global is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t agt_pulse_emit_tick(uint32_t tick)
{
  uint8_t digits[k_agt_pulse_hex_digits] = {};
  agt_pulse_format_tick(tick, digits);
  ra8_err_t err =
    ra8_board_uart_console_write(k_agt_pulse_msg_head, (size_t)(sizeof(k_agt_pulse_msg_head) - 1U));
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_uart_console_write(digits, (size_t)sizeof(digits));
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_board_uart_console_write(k_agt_pulse_msg_tail,
                                      (size_t)(sizeof(k_agt_pulse_msg_tail) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  agt_pulse_setup_or_halt();
  ra8_isr_globals_enable();

  if (agt_pulse_arm() != k_ra8_ok) {
    agt_pulse_panic_halt();
  }

  uint32_t tick = 0U;
  while (1) {
    uint8_t status = 0U;
    if (ra8_agt_get_status((uint8_t)k_agt_pulse_channel, &status) != k_ra8_ok) {
      break;
    }
    if ((status & (uint8_t)k_ra8_agt_agtcr_tundf_msk) != 0U) {
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      ++tick;
      if (agt_pulse_emit_tick(tick) != k_ra8_ok) {
        break;
      }
      /* Stop + re-arm to clear AGTCR.TUNDF on real silicon. */
      if (ra8_agt_stop((uint8_t)k_agt_pulse_channel) != k_ra8_ok) {
        break;
      }
      if (agt_pulse_arm() != k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_agt_pulse_poll_ms);
  }
  agt_pulse_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
