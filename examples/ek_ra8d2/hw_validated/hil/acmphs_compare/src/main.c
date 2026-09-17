/**
 * @file examples/ek_ra8d2/hw_validated/hil/acmphs_compare/src/main.c
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
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

/**
 * @brief Park the CPU forever after a fatal comparator-demo failure.
 *
 * @details Enters a permanent wait-for-interrupt loop so the comparator and
 *          LED state at the point of failure remain available to a debugger.
 *
 * @return None.
 *
 * @pre The caller has determined the demo cannot safely continue.
 * @pre Any pending LED diagnostic has already been requested.
 * @post The function never returns to its caller.
 * @post No further comparator samples or liveness increments occur.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_acmphs_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LEDs + ACMPHS channel 0 up.
 *
 * @details Initializes the clock and time base, configures all three board LEDs,
 *          enables the comparator block, and binds channel 0 to the demo's
 *          selected input and reference without edge interrupts or filtering.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_acmphs_init != ok || ra8_acmphs_channel_init !=
 * ok``. Two atomic conditions x N+1 = 3 vectors -- both-ok (steady
 * state), each-fail (mock injection in test_app_acmphs_compare).
 *
 * @return Error code from the first failing primitive.
 * @retval k_ra8_ok All clocks, LEDs, and comparator state were initialized.
 * @retval (other)  The first error reported by a board or HAL dependency.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @pre The comparator input and reference selections match the board wiring.
 * @post On success ACMPHS channel 0 is enabled in polling mode.
 * @post On failure no later initialization step is attempted.
 *
 * @note Single-shot boot helper; it is not safe to invoke concurrently.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_acmphs_demo_setup(void)
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
 * @details Polls channel 0 once, toggling LED1 for a high result and LED2 for a
 *          low result while preserving the first HAL or BSP error.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_acmphs_read_output != ok``. One atomic
 * condition x 2 vectors -- success (steady state) + driver-failure
 * (test mock).
 *
 * @return Error code from ra8_acmphs_read_output.
 * @retval k_ra8_ok The sample was read and the selected LED toggled.
 * @retval (other)  The comparator read or selected LED update failed.
 *
 * @pre ::internal_acmphs_demo_setup completed successfully.
 * @pre This demo exclusively owns channel 0 and LED1/LED2 updates.
 * @post On success exactly one of LED1 or LED2 is toggled.
 * @post On a read error neither LED is changed.
 *
 * @note Polling helper; no comparator interrupt state is consumed.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_acmphs_demo_one_iter(void)
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

void main(void)
{
  if (internal_acmphs_demo_setup() != k_ra8_ok) {
    internal_acmphs_demo_panic_halt();
  }
  ra8_isr_globals_enable();

  while (1) {
    if (internal_acmphs_demo_one_iter() != k_ra8_ok) {
      (void)ra8_board_led_on(k_ra8_board_led3);
      break;
    }
    g_acmphs_compare_tick += 1U;
    ra8_delay_ms((uint32_t)k_acmphs_demo_period_ms);
  }
  internal_acmphs_demo_panic_halt();
}
