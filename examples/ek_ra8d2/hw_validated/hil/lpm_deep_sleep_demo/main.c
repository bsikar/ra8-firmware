/**
 * @file examples/ek_ra8d2/lpm_deep_sleep_demo/main.c
 * @brief Deep-Sleep mode (LPMD=0, SCR.SLEEPDEEP=1) wake-count demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates Cortex-M85 Deep Sleep (the second-shallowest LPM
 * state) on the EK-RA8D2. LPSCR.LPMD stays at 0 (System Active) but
 * SCR.SLEEPDEEP is asserted before WFI so the core power-gates more
 * aggressively than plain Sleep. SysTick continues to run -- it is
 * one of the few peripherals that survives SLEEPDEEP -- and serves
 * as the wake source on a millisecond cadence.
 *
 * Sequence:
 *   1. CGC + SysTick + LPM bring-up (no UART -- see "SCI wedge" note
 *      below).
 *   2. LED1 init.
 *   3. Main loop:
 *      a. ``ra_lpm_enter_sleep(k_ra_sleep_mode_deep_sleep)`` -- WFI
 *         with SLEEPDEEP=1.
 *      b. ``ra_delay_ms(100)`` accumulates ~100 SysTick wakes.
 *      c. Increment ``g_lpm_deep_wake_count`` (volatile, externally
 *         readable via SWD).
 *      d. Toggle LED1 so the operator sees activity.
 *
 * @par SCI wedge note
 * An earlier prototype of this demo printed
 * ``"lpm: wake_count=NNNNNNNN\r\n"`` on SCI8 after every wake.
 * That output never appeared on the bench -- the SCI8 module clock
 * (PCLKA) is gated by the default ``ra_lpm_init`` config
 * (``opa_bus_keep=true``, ``io_port_keep=false``) when SLEEPDEEP is
 * asserted, so the TDR write either drops on the floor or the first
 * post-wake byte mis-frames the UART. Rather than fight the gating,
 * this demo drops SCI entirely and the HIL harness gates on the
 * ``g_lpm_deep_wake_count`` symbol via ``jlink_memprobe`` -- a
 * stronger signal than a UART scrape because it directly proves the
 * Deep-Sleep -> wake -> main-loop path completed at least N times.
 *
 * No external hardware required -- bare EK-RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_lpm_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_lpm.h"
#include "ra_time.h"

/** @brief Compile-time tunables for the deep-sleep demo. */
typedef enum : uint32_t {
  k_lpm_deep_period_ms = 100U,
} lpm_deep_config_t;

/**
 * @var g_lpm_deep_wake_count
 * @brief HIL liveness counter -- incremented after every Deep-Sleep wake.
 *
 * @details
 * Read externally by scripts/hil_jlink_memprobe.sh: the script halts
 * the chip, samples this counter, lets the chip run for N seconds,
 * halts again, and asserts the delta is at least
 * ``HIL_PROBE_MIN_ADVANCE``. With a 100 ms sleep period the symbol
 * should advance ~10 times per second, so a 3 s window easily clears
 * the gate threshold even if the first SWD halt freezes mid-WFI.
 *
 * `volatile` keeps the increment alive under -Os; the global (non-
 * static) keeps the symbol linker-visible without --gc-sections
 * culling it.
 *
 * @note Read externally by J-Link only; firmware never reads it back.
 * @since 0.1.0
 */
volatile uint32_t g_lpm_deep_wake_count = 0U;

/** @brief Park forever after a fatal init failure. */
static void lpm_deep_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LED1 + LPM up.
 *
 * @details
 * No SCI bring-up -- see the "SCI wedge" note in the file header.
 * The LPM block is configured with ``opa_bus_keep=true`` so the bus
 * fabric retains state across Deep Sleep entries (otherwise the
 * post-wake recovery costs an extra ~100 cycles of bus re-arming).
 *
 * @pre IRQs disabled (Reset_Handler default).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success the four sub-systems are armed; on any failure
 *       the function never returns (panic-halts).
 * @post LPM block has LPSCR.LPMD cleared to 0 so the first WFI is a
 *       plain CPU sleep until ``ra_lpm_enter_sleep`` is called.
 *
 * @since 0.1.0
 */
static void lpm_deep_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    lpm_deep_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lpm_deep_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lpm_deep_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    lpm_deep_panic_halt();
  }
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  if (ra_lpm_init(&lpm_cfg) != k_ra_ok) {
    lpm_deep_panic_halt();
  }
}

/**
 * @brief Enter Deep Sleep once and post-process the wake.
 *
 * @par MC/DC:
 * Compound decision: ``enter_sleep != ok`` is the only non-trivial
 * predicate. One atomic condition x 2 vectors -- valid mode (runtime
 * happy path) + invalid mode (covered by host test
 * test_app_lpm_deep_sleep.c).
 *
 * @return Error code from ``ra_lpm_enter_sleep``.
 *
 * @pre ``lpm_deep_setup_or_halt`` returned ok.
 * @pre At least one wake source (SysTick) is active.
 *
 * @post On success ``g_lpm_deep_wake_count`` has been incremented
 *       exactly once.
 * @post LED1 has been toggled exactly once on success.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lpm_deep_one_wake(void)
{
  if (ra_lpm_enter_sleep(k_ra_sleep_mode_deep_sleep) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  ra_delay_ms((uint32_t)k_lpm_deep_period_ms);
  g_lpm_deep_wake_count++;
  return ra_board_led_toggle(k_ra_board_led1);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lpm_deep_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (lpm_deep_one_wake() != k_ra_ok) {
      break;
    }
  }
  lpm_deep_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
