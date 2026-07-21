/**
 * @file examples/ek_ra8d2/hw_validated/hil/dac_b_demo/main.c
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
 *   2. ``ra8_dac_b_init_configured`` -- 12-bit, right-aligned, both
 *      channels disabled initially.
 *   3. ``ra8_dac_b_set_output_enable(0, true)``.
 *   4. Loop: ``ra8_dac_b_write`` with monotonically increasing /
 *      decreasing 12-bit code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_dac_b.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint16_t {
  k_dac_b_demo_full_scale = 4095U, /**< 12-bit max.              */
  k_dac_b_demo_step       = 16U,   /**< Code increment per tick. */
} dac_b_demo_const_t;

typedef enum : uint32_t {
  k_dac_b_demo_step_ms = 20U, /**< DAC b demo step ms. */
} dac_b_demo_time_t;

/** @brief DAC_B channel under test. */
typedef enum : uint8_t {
  k_dac_b_demo_channel = 0U, /**< DAC b demo channel. */
} dac_b_demo_chan_t;

/**
 * @var g_dac_b_demo_tick
 * @brief HIL liveness counter -- incremented each DAC write iteration.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD; the script
 * halts the chip, samples this value, lets the chip run for N seconds,
 * halts again, and asserts the delta >= HIL_PROBE_MIN_ADVANCE. Catches
 * the "PC is in MRAM but main loop never iterated" failure mode that
 * the plain HIL_MODE=alive check misses.
 *
 * `volatile` keeps the increment alive under optimization; the global
 * (non-static) keeps the symbol linker-visible without --gc-sections
 * culling.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_dac_b_demo_tick = 0U;

static void dac_b_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void dac_b_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }
}

/**
 * @brief Configure DAC_B in 12-bit, normal-vref mode.
 *
 * @par MC/DC:
 * Decision: ``ra8_dac_b_init_configured != ok``. One atomic condition
 * x 2 vectors -- golden (this) + null cfg (test_app_dac_b_demo.c).
 */
[[nodiscard]] static ra8_err_t dac_b_demo_arm(void)
{
  const ra8_dac_b_cfg_t cfg = {
    .vref            = k_ra8_dac_b_vref_normal,
    .data_format     = k_ra8_dac_b_format_right,
    .enable_channel0 = true,
    .enable_channel1 = false,
  };
  return ra8_dac_b_init_configured(&cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dac_b_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (dac_b_demo_arm() != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }
  if (ra8_dac_b_set_output_enable((uint8_t)k_dac_b_demo_channel, true) != k_ra8_ok) {
    dac_b_demo_panic_halt();
  }

  uint16_t code = 0U;
  bool     up   = true;
  while (1) {
    if (ra8_dac_b_write((uint8_t)k_dac_b_demo_channel, code) != k_ra8_ok) {
      break;
    }
    g_dac_b_demo_tick += 1U;
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
    ra8_delay_ms((uint32_t)k_dac_b_demo_step_ms);
  }
  dac_b_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
