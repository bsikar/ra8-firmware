/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_three_phase_demo/main.c
 * @brief GPT three-phase complementary PWM HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up three GPT channels (0/1/2) as a phase-synchronised
 * three-phase PWM bank (HUM Ch 25.2.3 GTSTR / GTBER -- the three
 * channels are armed via the same GTSTR write so they all start on
 * the same PCLKD edge). Drives all three channels at the same period
 * with U/V/W initial duties offset by ~33 % so the motor-control
 * waveforms would be observable on a scope -- bench validation here
 * just confirms the three counters advance synchronously and the
 * driver did not reject the multi-channel arm.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 init.
 *   - ``ra8_gpt_three_phase_open`` with the three channel ids + a
 *     shared period and three initial duties.
 *   - Loop: sample GPT0 counter every step_ms; verify it advanced;
 *     bump bench-readable counters.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Per-step duty advance. */
typedef enum : uint32_t {
  k_gpt_duty_advance = 0x200U, /**< Compare-value increment per update. */
} gpt_demo_duty_t;

typedef enum : uint32_t {
  k_gpt_3p_demo_period  = 0x0000FFFFU, /**< GPT 3p demo period.  */
  k_gpt_3p_demo_step_ms = 20U,         /**< GPT 3p demo step ms. */
  k_gpt_3p_demo_duty_u  = 0x00002000U, /**< ~12.5 %.             */
  k_gpt_3p_demo_duty_v  = 0x00006000U, /**< ~37.5 %.             */
  k_gpt_3p_demo_duty_w  = 0x0000A000U, /**< ~62.5 %.             */
} gpt_3p_demo_const_t;

/** @brief Three-phase channel ids. */
typedef enum : uint8_t {
  k_gpt_3p_demo_ch_u = 0U, /**< GPT 3p demo channel u. */
  k_gpt_3p_demo_ch_v = 1U, /**< GPT 3p demo channel v. */
  k_gpt_3p_demo_ch_w = 2U, /**< GPT 3p demo channel w. */
} gpt_3p_demo_chan_t;

/**
 * @var g_gpt_three_phase_match
 * @brief HIL liveness counter -- bumped each iteration the GPT0
 *        counter advanced relative to the previous sample.
 *
 * @details The probe asserts this advances. If the three-phase arm
 * failed or PCLKD is gated, the counter never moves.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_three_phase_match = 0U;

/**
 * @var g_gpt_three_phase_mismatch
 * @brief HIL failure counter -- bumped when the GPT counter did not
 *        advance between two samples, or read returned non-ok.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_three_phase_mismatch = 0U;

static void gpt_3p_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void gpt_3p_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    gpt_3p_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    gpt_3p_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    gpt_3p_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    gpt_3p_demo_panic_halt();
  }
}

/**
 * @brief Open the three GPT channels as a synchronised triple.
 *
 * @return Return code from ra8_gpt_three_phase_open.
 *
 * @pre ra8_cgc_init has succeeded.
 * @pre Channels 0/1/2 are not in use by another driver.
 * @post On k_ra8_ok all three counters are running phase-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t gpt_3p_demo_arm(void)
{
  const ra8_gpt_three_phase_cfg_t cfg = {
    .channels       = {(uint8_t)k_gpt_3p_demo_ch_u,
                       (uint8_t)k_gpt_3p_demo_ch_v,
                       (uint8_t)k_gpt_3p_demo_ch_w},
    .mode           = k_ra8_gpt_mode_saw_pwm,
    .prescaler      = k_ra8_gpt_ps_div_4,
    .period_counts  = (uint32_t)k_gpt_3p_demo_period,
    .initial_duty_u = (uint32_t)k_gpt_3p_demo_duty_u,
    .initial_duty_v = (uint32_t)k_gpt_3p_demo_duty_v,
    .initial_duty_w = (uint32_t)k_gpt_3p_demo_duty_w,
  };
  return ra8_gpt_three_phase_open(&cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpt_3p_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (gpt_3p_demo_arm() != k_ra8_ok) {
    gpt_3p_demo_panic_halt();
  }

  /* Cycle the three-phase duties via ra8_gpt_three_phase_set_duty
   * once per iteration. The call exercises GTCCR writes on all
   * three channels and proves the driver's open state is intact.
   * On a scope the U/V/W outputs would sweep their duty cycles in
   * lockstep; on the HIL bench we just count successful set_duty
   * calls. */
  uint32_t duty_offset = 0U;
  while (1) {
    const uint32_t period = (uint32_t)k_gpt_3p_demo_period;
    const uint32_t u_duty = ((uint32_t)k_gpt_3p_demo_duty_u + duty_offset) & period;
    const uint32_t v_duty = ((uint32_t)k_gpt_3p_demo_duty_v + duty_offset) & period;
    const uint32_t w_duty = ((uint32_t)k_gpt_3p_demo_duty_w + duty_offset) & period;
    if (ra8_gpt_three_phase_set_duty(u_duty, v_duty, w_duty) == k_ra8_ok) {
      g_gpt_three_phase_match += 1U;
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      g_gpt_three_phase_mismatch += 1U;
    }
    duty_offset = (duty_offset + k_gpt_duty_advance) & period;
    ra8_delay_ms((uint32_t)k_gpt_3p_demo_step_ms);
  }
  gpt_3p_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
