/**
 * @file examples/ek_ra8d2/timer_capture_demo/main.c
 * @brief GPT free-running timer capture demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + GPT0 in free-run mode.
 * Once a second:
 *
 *   1. Snapshots GPT0 counter via ``ra_gpt_read``.
 *   2. Toggles LED1 (the "event" being measured).
 *   3. ``ra_delay_ms(50)`` to leave a known interval between reads.
 *   4. Snapshots GPT0 again, computes ``delta = stop - start``
 *      (handling wrap), and emits ``"gpt: period=NNNNNNNN\r\n"``
 *      on the J-Link OB CDC channel (115200 8N1).
 *
 * No external pin wiring is needed -- the GPT input-capture mode
 * needs an external GTIOC edge that the bare EK-RA8D2 cannot
 * provide without a shield, so this demo uses the simpler
 * "snapshot the free-run counter twice across a known software
 * delay" pattern. The captured period is dominated by the 50 ms
 * delay so the printed value is reproducible across boots.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
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
#include "ra_gpt.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_timer_demo_baud        = 115200U,
  k_timer_demo_period_ms   = 1000U,
  k_timer_demo_capture_ms  = 50U,
  k_timer_demo_sci_channel = 8U,
  k_timer_demo_gpt_channel = 0U,
  k_timer_demo_gpt_period  = 0xFFFFFFFFUL,
} timer_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_timer_demo_nibble_mask  = 0x0FU,
  k_timer_demo_nibble_shift = 4U,
  k_timer_demo_alpha_thresh = 10U,
  k_timer_demo_hex_per_word = 8U,
} timer_demo_byte_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel. */
static const ra_port_pin_t k_timer_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_timer_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Output line tags. */
static const uint8_t k_timer_demo_prefix[] = "gpt: period=";
static const uint8_t k_timer_demo_eol[]    = "\r\n";

/** @brief Park forever after a fatal init failure. */
static void timer_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Convert the low nibble of ``n`` to its ASCII hex char.
 *
 * @param[in] nibble Lower 4 bits used.
 * @return Printable ASCII byte.
 *
 * @pre None.
 * @post Return value is always printable ASCII.
 *
 * @since 0.1.0
 */
static uint8_t timer_demo_nibble_to_hex(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_timer_demo_nibble_mask);
  if (n < (uint8_t)k_timer_demo_alpha_thresh) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - (uint8_t)k_timer_demo_alpha_thresh));
}

/** @brief Render a 32-bit value as 8 ASCII hex chars (big-endian).
 *
 * @param[in]  v   Value to render.
 * @param[out] dst 8-byte buffer that receives the hex characters.
 *
 * @pre ``dst`` is non-NULL and has at least 8 bytes capacity.
 * @post ``dst[0..7]`` contains printable hex.
 *
 * @since 0.1.0
 */
static void timer_demo_word_to_hex(uint32_t v, uint8_t* dst)
{
  for (uint8_t i = 0U; i < (uint8_t)k_timer_demo_hex_per_word; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_timer_demo_hex_per_word - 1U - i) * (uint8_t)k_timer_demo_nibble_shift);
    const uint8_t nibble = (uint8_t)((v >> shift) & (uint32_t)k_timer_demo_nibble_mask);
    dst[i]               = timer_demo_nibble_to_hex(nibble);
  }
}

/**
 * @brief Compute ``stop - start`` modulo 2^32 (handles wrap).
 *
 * @par MC/DC:
 * Compound decision: ``stop >= start``. One atomic condition x 2
 * vectors -- no-wrap (this) + wrap (covered by host integration test).
 *
 * @param[in] start Earlier counter snapshot.
 * @param[in] stop  Later counter snapshot.
 * @return Number of timer ticks between snapshots.
 *
 * @pre Both inputs are valid GPT counter values.
 * @post Return value is in [0, 2^32).
 *
 * @since 0.1.0
 */
static uint32_t timer_demo_delta(uint32_t start, uint32_t stop)
{
  if (stop >= start) {
    return stop - start;
  }
  return (uint32_t)k_timer_demo_gpt_period - start + stop + 1U;
}

/** @brief Route PD_02 / PD_03 to SCI8 TXD/RXD via PFS.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 * @retval k_ra_ok                       Both pins routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port out of range.
 *
 * @pre IOPORT module reachable.
 * @post On success PD_02 + PD_03 in SCI-async mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t timer_demo_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_timer_demo_pin_txd, k_ra_psel_sci_async, "timer_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_timer_demo_pin_rxd, k_ra_psel_sci_async, "timer_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LED1 + MSTP + GPT0 up. */
static void timer_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (timer_demo_pins_init() != k_ra_ok) {
    timer_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_timer_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_timer_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    timer_demo_panic_halt();
  }
  if (ra_gpt_start_free_run((uint8_t)k_timer_demo_gpt_channel, (uint32_t)k_timer_demo_gpt_period) !=
      k_ra_ok) {
    timer_demo_panic_halt();
  }
}

/**
 * @brief One iteration: capture two counter snapshots and emit the delta.
 *
 * @par MC/DC:
 * Compound decision: ``read_start != ok || read_stop != ok ||
 * sci_writes != ok``. Three atomic conditions x N+1 = 4 vectors;
 * the all-ok runtime path here + each error branch covered by the
 * host integration test.
 *
 * @return Error code from the first failing primitive.
 *
 * @pre timer_demo_setup_or_halt() returned ok.
 * @post On success the period line was transmitted.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t timer_demo_one_capture(void)
{
  uint32_t start = 0U;
  uint32_t stop  = 0U;

  if (ra_gpt_read((uint8_t)k_timer_demo_gpt_channel, &start) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  (void)ra_board_led_toggle(k_ra_board_led1);
  ra_delay_ms((uint32_t)k_timer_demo_capture_ms);
  if (ra_gpt_read((uint8_t)k_timer_demo_gpt_channel, &stop) != k_ra_ok) {
    return k_ra_err_hw_error;
  }

  const uint32_t delta                          = timer_demo_delta(start, stop);
  uint8_t        hex[k_timer_demo_hex_per_word] = {};
  timer_demo_word_to_hex(delta, hex);

  if (ra_sci_write_polling((uint8_t)k_timer_demo_sci_channel,
                           k_timer_demo_prefix,
                           (uint32_t)(sizeof(k_timer_demo_prefix) - 1U)) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_sci_write_polling((uint8_t)k_timer_demo_sci_channel,
                           hex,
                           (uint32_t)k_timer_demo_hex_per_word) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  return ra_sci_write_polling((uint8_t)k_timer_demo_sci_channel,
                              k_timer_demo_eol,
                              (uint32_t)(sizeof(k_timer_demo_eol) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  timer_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (timer_demo_one_capture() != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_timer_demo_period_ms);
  }
  timer_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
