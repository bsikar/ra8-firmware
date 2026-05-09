/**
 * @file examples/ek_ra8d2/dac_b_demo/main.c
 * @brief DAC_B 12-bit DC sweep demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives DAC_B channel 0 in 12-bit normal-vref mode and walks the
 * output value linearly from 0 to ``k_dac_b_demo_full_scale``
 * (~3.3 V) and back, stepping every ``k_dac_b_demo_step_ms``. This
 * is distinct from the ``dac_waveform`` example which uses a
 * lookup-table sine wave.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + LED1.
 *   2. ``ra_dac_b_init_configured`` -- 12-bit, right-aligned, both
 *      channels disabled initially.
 *   3. ``ra_dac_b_set_output_enable(0, true)``.
 *   4. Loop: ``ra_dac_b_write`` with monotonically increasing /
 *      decreasing 12-bit code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_dac_b.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint16_t {
  k_dac_b_demo_full_scale = 4095U, /**< 12-bit max. */
  k_dac_b_demo_step       = 16U,   /**< Code increment per tick. */
} dac_b_demo_const_t;

typedef enum : uint32_t {
  k_dac_b_demo_step_ms = 20U,
} dac_b_demo_time_t;

/** @brief DAC_B channel under test. */
typedef enum : uint8_t {
  k_dac_b_demo_channel = 0U,
} dac_b_demo_chan_t;

static void dac_b_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void dac_b_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    dac_b_demo_panic_halt();
  }
}

/**
 * @brief Configure DAC_B in 12-bit, normal-vref mode.
 *
 * @par MC/DC:
 * Decision: ``ra_dac_b_init_configured != ok``. One atomic condition
 * x 2 vectors -- golden (this) + null cfg (test_app_dac_b_demo.c).
 */
[[nodiscard]] static ra_err_t dac_b_demo_arm(void)
{
  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_normal,
    .data_format     = k_ra_dac_b_format_right,
    .enable_channel0 = true,
    .enable_channel1 = false,
  };
  return ra_dac_b_init_configured(&cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dac_b_demo_setup_or_halt();
  ra_isr_globals_enable();

  if (dac_b_demo_arm() != k_ra_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra_dac_b_set_output_enable((uint8_t)k_dac_b_demo_channel, true) != k_ra_ok) {
    dac_b_demo_panic_halt();
  }

  uint16_t code = 0U;
  bool     up   = true;
  while (1) {
    if (ra_dac_b_write((uint8_t)k_dac_b_demo_channel, code) != k_ra_ok) {
      break;
    }
    if (up) {
      if ((uint16_t)(code + (uint16_t)k_dac_b_demo_step) >= (uint16_t)k_dac_b_demo_full_scale) {
        code = (uint16_t)k_dac_b_demo_full_scale;
        up   = false;
      } else {
        code = (uint16_t)(code + (uint16_t)k_dac_b_demo_step);
      }
    } else {
      if (code <= (uint16_t)k_dac_b_demo_step) {
        code = 0U;
        up   = true;
      } else {
        code = (uint16_t)(code - (uint16_t)k_dac_b_demo_step);
      }
    }
    ra_delay_ms((uint32_t)k_dac_b_demo_step_ms);
  }
  dac_b_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
