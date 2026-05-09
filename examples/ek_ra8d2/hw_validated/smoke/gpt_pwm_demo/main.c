/**
 * @file examples/ek_ra8d2/gpt_pwm_demo/main.c
 * @brief GPT PWM duty-cycle LED demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives an LED off GPT channel ``k_gpt_pwm_demo_channel`` in saw-wave
 * PWM mode. Each main-loop iteration:
 *
 *   1. Programmes a new GTCCRA duty value via ``ra_gpt_set_duty``.
 *   2. Walks the duty cycle linearly from 0 to ``period`` and back so
 *      the LED visibly breathes at roughly 1 Hz.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 init.
 *   - ``ra_gpt_init`` with ``mode = saw_pwm`` + ``auto_start = true``.
 *   - Loop forever stepping ``ra_gpt_set_duty`` at
 *     ``k_gpt_pwm_demo_step_ms``.
 *
 * Bare EK-RA8D2; no expansion board.
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
  k_gpt_pwm_demo_period   = 0x0000FFFFU, /**< 16-bit period (~61 Hz at 4 MHz / 4). */
  k_gpt_pwm_demo_step_ms  = 20U,         /**< Per-step delay -> ~1.3 s sweep. */
  k_gpt_pwm_demo_step_pct = 1024U,       /**< Duty step (>= 64 to stay in 16-bit range). */
} gpt_pwm_demo_const_t;

/** @brief GPT channel. */
typedef enum : uint8_t {
  k_gpt_pwm_demo_channel = 0U,
} gpt_pwm_demo_chan_t;

static void gpt_pwm_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void gpt_pwm_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    gpt_pwm_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    gpt_pwm_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    gpt_pwm_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    gpt_pwm_demo_panic_halt();
  }
}

/**
 * @brief Configure GPT0 for saw-wave PWM with ``auto_start = true``.
 *
 * @par MC/DC:
 * Decision: ``ra_gpt_init != ok``. One atomic condition x 2 vectors --
 * golden (this) + null-cfg reject (covered in test_app_gpt_pwm_demo.c).
 */
[[nodiscard]] static ra_err_t gpt_pwm_demo_arm(void)
{
  const ra_gpt_cfg_t cfg = {
    .mode       = k_ra_gpt_mode_saw_pwm,
    .prescaler  = k_ra_gpt_ps_div_4,
    .period     = (uint32_t)k_gpt_pwm_demo_period,
    .duty_a     = (uint32_t)0U,
    .duty_b     = (uint32_t)0U,
    .auto_start = true,
  };
  return ra_gpt_init((uint8_t)k_gpt_pwm_demo_channel, &cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpt_pwm_demo_setup_or_halt();
  ra_isr_globals_enable();

  if (gpt_pwm_demo_arm() != k_ra_ok) {
    gpt_pwm_demo_panic_halt();
  }

  uint32_t duty = 0U;
  bool     up   = true;
  while (1) {
    if (ra_gpt_set_duty((uint8_t)k_gpt_pwm_demo_channel, k_ra_gpt_ccr_a, duty) != k_ra_ok) {
      break;
    }
    if (up) {
      if (duty + (uint32_t)k_gpt_pwm_demo_step_pct >= (uint32_t)k_gpt_pwm_demo_period) {
        duty = (uint32_t)k_gpt_pwm_demo_period;
        up   = false;
      } else {
        duty += (uint32_t)k_gpt_pwm_demo_step_pct;
      }
    } else {
      if (duty <= (uint32_t)k_gpt_pwm_demo_step_pct) {
        duty = 0U;
        up   = true;
      } else {
        duty -= (uint32_t)k_gpt_pwm_demo_step_pct;
      }
    }
    ra_delay_ms((uint32_t)k_gpt_pwm_demo_step_ms);
  }
  gpt_pwm_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
