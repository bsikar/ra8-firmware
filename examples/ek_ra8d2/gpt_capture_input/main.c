/**
 * @file examples/ek_ra8d2/gpt_capture_input/main.c
 * @brief GPT input-capture-style demo: measure SW1 press period
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Programs GPT0 as a free-running 32-bit up-counter with prescaler
 * PCLKD/64. On each falling edge of SW1 (active-low) the application
 * latches the current GPT counter value and computes the delta from
 * the previous press: that delta is the measured "input capture
 * period" in GPT counter ticks. The lowest LED is toggled on every
 * captured edge so a human can see live activity.
 *
 * Note: the RA8D2 GPT supports hardware GTIOC pin capture, but the
 * HAL layer exposes free-run + counter-read primitives only at the
 * time of writing. This demo exercises the available surface and
 * approximates input capture in software via SW1 polling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpt.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_capture_period         = 0xFFFFFFFFUL,
  k_gpt_capture_poll_period_ms = 10U,
  k_gpt_capture_min_delta_ms   = 50U,
} gpt_capture_const_t;

/** @brief Channel + LED selection. */
typedef enum : uint8_t {
  k_gpt_capture_channel = 0U,
} gpt_capture_layout_t;

/** @brief Park the CPU after a fatal init failure. */
static void gpt_capture_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LEDs + SW1 + GPT0 up.
 */
static void gpt_capture_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    gpt_capture_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    gpt_capture_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    gpt_capture_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    gpt_capture_panic_halt();
  }
  if (ra_board_sw_init(k_ra_board_sw1) != k_ra_ok) {
    gpt_capture_panic_halt();
  }
  const ra_gpt_cfg_t cfg = {
    .mode       = k_ra_gpt_mode_saw_pwm,
    .prescaler  = k_ra_gpt_ps_div_64,
    .period     = (uint32_t)k_gpt_capture_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  if (ra_gpt_init((uint8_t)k_gpt_capture_channel, &cfg) != k_ra_ok) {
    gpt_capture_panic_halt();
  }
}

/**
 * @brief Read current GPT counter ticks, or return UINT32_MAX on err.
 */
static uint32_t gpt_capture_read(void)
{
  uint32_t val = 0U;
  if (ra_gpt_read((uint8_t)k_gpt_capture_channel, &val) != k_ra_ok) {
    return UINT32_MAX;
  }
  return val;
}

/**
 * @brief Detect a falling edge on SW1.
 *
 * @par MC/DC:
 * Compound decision: ``prev == released && now == pressed``. Two
 * atomic conditions x N+1 = 3 vectors -- both true (edge), prev
 * pressed (no edge), now released (no edge). Companion host test
 * exercises each vector.
 *
 * @param[in,out] prev Last sampled state (updated on return).
 * @return true on the falling edge, false otherwise.
 */
static bool gpt_capture_edge(ra_board_sw_state_t* prev)
{
  ra_board_sw_state_t now = k_ra_board_sw_released;
  if (ra_board_sw_read(k_ra_board_sw1, &now) != k_ra_ok) {
    return false;
  }
  bool fell = (*prev == k_ra_board_sw_released) && (now == k_ra_board_sw_pressed);
  *prev     = now;
  return fell;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpt_capture_setup_or_halt();
  ra_isr_globals_enable();

  ra_board_sw_state_t last_state = k_ra_board_sw_released;
  uint32_t            last_tick  = 0U;

  while (1) {
    if (gpt_capture_edge(&last_state)) {
      const uint32_t now        = gpt_capture_read();
      const uint32_t last_delta = now - last_tick;
      last_tick                 = now;
      (void)last_delta;
      (void)ra_board_led_toggle(k_ra_board_led1);
    }
    ra_delay_ms(k_gpt_capture_poll_period_ms);
  }
  gpt_capture_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
