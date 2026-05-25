/**
 * @file examples/ek_ra8d2/hw_validated/hil/agt_pulse_demo/main.c
 * @brief AGT pulse-output / output-compare demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + UART (SCI8 on PD_02 / PD_03) and arms AGT0
 * in pulse-output mode via the new `ra_agt_start_pulse_output` HAL
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

#include "ra8d2_agt_regs.h"
#include "ra_agt.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_agt_pulse_baud        = 115200U,
  k_agt_pulse_sci_channel = 8U,
  k_agt_pulse_poll_ms     = 10U,
} agt_pulse_const_t;

/** @brief AGT channel + pulse-output settings. */
typedef enum : uint16_t {
  k_agt_pulse_channel = 0U,
  k_agt_pulse_period  = 0x7FFFU, /**< ~1 Hz at the simulator default. */
  k_agt_pulse_duty    = 0x3FFFU, /**< 50% via compare-match A.        */
} agt_pulse_timer_t;

/** @brief Hex-digit width + mask for the per-tick banner formatter. */
typedef enum : uint32_t {
  k_agt_pulse_hex_digits = 8U,
  k_agt_pulse_hex_shift  = 4U,
  k_agt_pulse_hex_mask   = 0xFU,
} agt_pulse_fmt_t;

/** @brief SCI8 pin map -- mirrors agt_periodic and uart_hello. */
static const ra_port_pin_t k_agt_pulse_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_agt_pulse_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

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
 * @brief Route SCI8 TXD / RXD onto P13_02 / P13_03.
 *
 * @details
 * Wraps `ra_pfs_route_peripheral` for the two SCI pins.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Both pins routed.
 * @retval k_ra_err_invalid_arg PFS rejected the pin map.
 *
 * @pre IOPORT power gate is open (CGC has run).
 * @pre PFS PWPR has been unlocked by the caller.
 *
 * @post P13_02 / P13_03 are owned by SCI8.
 * @post PFS PWPR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t agt_pulse_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_agt_pulse_pin_txd, k_ra_psel_sci_async, "agt_pulse.txd");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_agt_pulse_pin_rxd, k_ra_psel_sci_async, "agt_pulse.rxd");
}

/**
 * @brief Bring up CGC, SysTick, SCI8 and LED1.
 *
 * @details
 * Sequence: ra_cgc_init -> ra_time_init -> ra_pfs_route -> ra_sci_init
 * -> ra_board_led_init. Any failure goes straight to panic-halt so the
 * HIL gate sees no `agt_pulse:` banner.
 *
 * @pre Called once from `main()` before `ra_isr_globals_enable`.
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
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  if (agt_pulse_pins_init() != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_agt_pulse_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_agt_pulse_sci_channel, &sci_cfg) != k_ra_ok) {
    agt_pulse_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    agt_pulse_panic_halt();
  }
}

/**
 * @brief Arm AGT0 in pulse-output / output-compare mode.
 *
 * @par MC/DC:
 * Single atomic decision: ``ra_agt_start_pulse_output != ok``. Two
 * vectors -- happy path (HIL bench) and the bad-channel reject path
 * covered in `tests/test_ra_agt.c`.
 *
 * @return ``ra_err_t`` from the HAL call.
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
[[nodiscard]] static ra_err_t agt_pulse_arm(void)
{
  const ra_agt_pulse_cfg_t cfg = {
    .period   = (uint16_t)k_agt_pulse_period,
    .duty     = (uint16_t)k_agt_pulse_duty,
    .mode     = k_ra_agt_pulse_mode_continuous,
    .polarity = k_ra_agt_output_polarity_active_high,
    .compare  = k_ra_agt_pulse_compare_a,
  };
  return ra_agt_start_pulse_output((uint8_t)k_agt_pulse_channel, &cfg);
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
 * @return ``ra_err_t`` from the SCI driver (first failure wins).
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
[[nodiscard]] static ra_err_t agt_pulse_emit_tick(uint32_t tick)
{
  uint8_t digits[k_agt_pulse_hex_digits] = {};
  agt_pulse_format_tick(tick, digits);
  ra_err_t err = ra_sci_write_polling((uint8_t)k_agt_pulse_sci_channel,
                                      k_agt_pulse_msg_head,
                                      (uint32_t)(sizeof(k_agt_pulse_msg_head) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_sci_write_polling((uint8_t)k_agt_pulse_sci_channel, digits, sizeof(digits));
  if (err != k_ra_ok) {
    return err;
  }
  return ra_sci_write_polling((uint8_t)k_agt_pulse_sci_channel,
                              k_agt_pulse_msg_tail,
                              (uint32_t)(sizeof(k_agt_pulse_msg_tail) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  agt_pulse_setup_or_halt();
  ra_isr_globals_enable();

  if (agt_pulse_arm() != k_ra_ok) {
    agt_pulse_panic_halt();
  }

  uint32_t tick = 0U;
  while (1) {
    uint8_t status = 0U;
    if (ra_agt_get_status((uint8_t)k_agt_pulse_channel, &status) != k_ra_ok) {
      break;
    }
    if ((status & (uint8_t)k_ra_agt_agtcr_tundf_msk) != 0U) {
      if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
        break;
      }
      ++tick;
      if (agt_pulse_emit_tick(tick) != k_ra_ok) {
        break;
      }
      /* Stop + re-arm to clear AGTCR.TUNDF on real silicon. */
      if (ra_agt_stop((uint8_t)k_agt_pulse_channel) != k_ra_ok) {
        break;
      }
      if (agt_pulse_arm() != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms((uint32_t)k_agt_pulse_poll_ms);
  }
  agt_pulse_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
