/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/timer_capture_demo/main.c
 * @brief GPT free-running timer capture demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + GPT0 in free-run mode.
 * Once a second:
 *
 *   1. Snapshots GPT0 counter via ``ra8_gpt_read``.
 *   2. Toggles LED1 (the "event" being measured).
 *   3. ``ra8_delay_ms(50)`` to leave a known interval between reads.
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
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_timer_demo_baud        = 115200U,      /**< Timer demo baud.        */
  k_timer_demo_period_ms   = 1000U,        /**< Timer demo period ms.   */
  k_timer_demo_capture_ms  = 50U,          /**< Timer demo capture ms.  */
  k_timer_demo_gpt_channel = 0U,           /**< Timer demo GPT channel. */
  k_timer_demo_gpt_period  = 0xFFFFFFFFUL, /**< Timer demo GPT period.  */
} timer_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_timer_demo_nibble_mask  = 0x0FU, /**< Timer demo nibble mask.  */
  k_timer_demo_nibble_shift = 4U,    /**< Timer demo nibble shift. */
  k_timer_demo_alpha_thresh = 10U,   /**< Timer demo alpha thresh. */
  k_timer_demo_hex_per_word = 8U,    /**< Timer demo hex per word. */
} timer_demo_byte_t;

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

/** @brief Bring CGC + SysTick + SCI8 + LED1 + MSTP + GPT0 up. */
static void timer_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_timer_demo_baud) != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    timer_demo_panic_halt();
  }
  if (ra8_gpt_start_free_run((uint8_t)k_timer_demo_gpt_channel,
                             (uint32_t)k_timer_demo_gpt_period) != k_ra8_ok) {
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
[[nodiscard]] static ra8_err_t timer_demo_one_capture(void)
{
  uint32_t start = 0U;
  uint32_t stop  = 0U;

  if (ra8_gpt_read((uint8_t)k_timer_demo_gpt_channel, &start) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  ra8_delay_ms((uint32_t)k_timer_demo_capture_ms);
  if (ra8_gpt_read((uint8_t)k_timer_demo_gpt_channel, &stop) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }

  const uint32_t delta                          = timer_demo_delta(start, stop);
  uint8_t        hex[k_timer_demo_hex_per_word] = {};
  timer_demo_word_to_hex(delta, hex);

  if (ra8_board_uart_console_write(k_timer_demo_prefix,
                                   (size_t)(sizeof(k_timer_demo_prefix) - 1U)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  if (ra8_board_uart_console_write(hex, (size_t)k_timer_demo_hex_per_word) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  return ra8_board_uart_console_write(k_timer_demo_eol, (size_t)(sizeof(k_timer_demo_eol) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  timer_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    if (timer_demo_one_capture() != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_timer_demo_period_ms);
  }
  timer_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
