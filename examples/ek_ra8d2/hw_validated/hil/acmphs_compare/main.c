/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/acmphs_compare/main.c
 * @brief High-Speed Analog Comparator (ACMPHS) channel-0 polling demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the on-chip ACMPHS unit, configures channel 0 with the
 * default plus-input / internal-Vref minus-input pair (no edge IRQ,
 * polling only), and reads ``CMPMON`` once per second via
 * ``ra8_acmphs_read_output``. LED1 toggles each time the comparator
 * output is HIGH; LED2 toggles each time the output is LOW. LED3
 * latches if a HAL error is ever returned.
 *
 * Bare EK-RA8D2 only -- no expansion board. The default plus-input
 * pin is left floating; the comparator simply tracks whatever rail
 * the analogue front-end happens to settle at, so the goal is to
 * exercise the bring-up + readback path, not to validate a known
 * threshold.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_acmphs_demo_period_ms = 1000U, /**< Acmphs demo period ms. */
} acmphs_demo_const_t;

/** @brief Channel + register-field selectors. */
typedef enum : uint8_t {
  k_acmphs_demo_channel  = 0U, /**< Acmphs demo channel.                     */
  k_acmphs_demo_ivpsel   = 0U, /**< Default IVCMP selector (CMPSEL0).        */
  k_acmphs_demo_ivrefsel = 0U, /**< Default IVREF selector (CMPSEL1, IVREF). */
} acmphs_demo_chan_t;

/**
 * @var g_acmphs_compare_tick
 * @brief HIL liveness counter -- incremented each comparator read iteration.
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
volatile uint32_t g_acmphs_compare_tick = 0U;

/** @brief Park the CPU forever after fatal init failure. */
static void acmphs_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LEDs + ACMPHS channel 0 up.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_acmphs_init != ok || ra8_acmphs_channel_init !=
 * ok``. Two atomic conditions x N+1 = 3 vectors -- both-ok (steady
 * state), each-fail (mock injection in test_app_acmphs_compare).
 *
 * @return Error code from the first failing primitive.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @post On success ACMPHS channel 0 is enabled in polling mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t acmphs_demo_setup(void)
{
  uint32_t  cpuclk0_hz = 0U;
  ra8_err_t err        = ra8_cgc_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_time_init(cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_led_init(k_ra8_board_led1);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_led_init(k_ra8_board_led2);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_led_init(k_ra8_board_led3);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_acmphs_init();
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_acmphs_cfg_t cfg = {
    .ivpsel     = (uint8_t)k_acmphs_demo_ivpsel,
    .ivrefsel   = (uint8_t)k_acmphs_demo_ivrefsel,
    .edge       = k_ra8_acmphs_edge_none,
    .filter_en  = false,
    .invert_out = false,
  };
  return ra8_acmphs_channel_init((uint8_t)k_acmphs_demo_channel, &cfg);
}

/**
 * @brief Read the comparator output and route to the matching LED.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_acmphs_read_output != ok``. One atomic
 * condition x 2 vectors -- success (steady state) + driver-failure
 * (test mock).
 *
 * @return Error code from ra8_acmphs_read_output.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t acmphs_demo_one_iter(void)
{
  ra8_level_t     lv  = k_ra8_level_low;
  const ra8_err_t err = ra8_acmphs_read_output((uint8_t)k_acmphs_demo_channel, &lv);
  if (err != k_ra8_ok) {
    return err;
  }
  if (lv == k_ra8_level_high) {
    return ra8_board_led_toggle(k_ra8_board_led1);
  }
  return ra8_board_led_toggle(k_ra8_board_led2);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (acmphs_demo_setup() != k_ra8_ok) {
    acmphs_demo_panic_halt();
  }
  ra8_isr_globals_enable();

  while (1) {
    if (acmphs_demo_one_iter() != k_ra8_ok) {
      (void)ra8_board_led_on(k_ra8_board_led3);
      break;
    }
    g_acmphs_compare_tick += 1U;
    ra8_delay_ms((uint32_t)k_acmphs_demo_period_ms);
  }
  acmphs_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
