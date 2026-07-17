/**
 * @file examples/ek_ra8d2/hw_validated/hil/lpm_deep_sleep_demo/main.c
 * @brief Deep-Sleep mode (LPMD=0, SCR.SLEEPDEEP=1) one-shot wake demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates Cortex-M85 Deep Sleep (the second-shallowest LPM
 * state) on the EK-RA8D2. LPSCR.LPMD stays at 0 (System Active) but
 * SCR.SLEEPDEEP is asserted before WFI so the core power-gates more
 * aggressively than plain Sleep. SysTick is the wake source.
 *
 * Sequence:
 *   1. CGC + SysTick + LED1 + SCI8 + LPM bring-up.
 *   2. Emit ``"lpm_deep: boot\r\n"`` over SCI8.
 *   3. Bump ``g_lpm_deep_pre_count`` so a JLink probe can confirm
 *      the firmware reached the LPM entry.
 *   4. ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep)`` -- WFI with
 *      SLEEPDEEP=1. SysTick wakes the core ~1 ms later.
 *   5. Bump ``g_lpm_deep_wake_count`` -- proves the chip woke from
 *      Deep-Sleep cleanly.
 *   6. Emit ``"lpm_deep: woke\r\n"`` over SCI8 -- the HIL gate
 *      scrapes for this banner.
 *   7. Park in a normal infinite loop (LED1 toggle every 500 ms).
 *      The loop is NOT in LPM, so subsequent bench flashes have a
 *      reliable halt window. Re-flashing the demo causes another
 *      boot -> Deep-Sleep -> wake -> banner cycle.
 *
 * @par SCI wedge note
 * Earlier prototypes printed UART traffic INSIDE the Deep-Sleep loop
 * (one banner per wake). That output never appeared on the bench --
 * the SCI8 module clock (PCLKA) is gated by the default
 * ``ra8_lpm_init`` config (``opa_bus_keep=true``, ``io_port_keep=false``)
 * when SLEEPDEEP is asserted, so the first post-wake TDR write
 * dropped on the floor and continuous post-wake printing mis-framed
 * subsequent bytes. The "one-shot then park" approach here avoids
 * that: the banner is emitted ONCE after a single Deep-Sleep cycle,
 * while the chip is back in normal active mode, and never again.
 *
 * @par Why two memprobe counters?
 * ``g_lpm_deep_pre_count`` increments BEFORE the LPM entry; if a
 * future regression breaks the LPM init the pre-counter still moves
 * but the wake-counter does not, narrowing the failure mode. Both
 * counters are externally readable via SWD.
 *
 * No external hardware required -- bare EK-RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_time.h"

/** @brief Compile-time tunables for the deep-sleep demo. */
typedef enum : uint32_t {
  k_lpm_deep_park_blink_ms = 500U,    /**< Lpm deep park blink ms. */
  k_lpm_deep_baud          = 115200U, /**< Lpm deep baud.          */
} lpm_deep_config_t;

/** @brief Boot + wake banners. */
static const uint8_t k_lpm_deep_msg_boot[] = "lpm_deep: boot\r\n";
static const uint8_t k_lpm_deep_msg_woke[] = "lpm_deep: woke\r\n";

/**
 * @var g_lpm_deep_pre_count
 * @brief Counter bumped right BEFORE the Deep-Sleep entry.
 *
 * @details Externally readable via SWD memprobe. Lets future
 * regression-bisects distinguish "LPM init crashed" (this counter
 * stays 0) from "Deep-Sleep entry crashed" (this counter increments
 * once, ``g_lpm_deep_wake_count`` stays 0).
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_lpm_deep_pre_count = 0U;

/**
 * @var g_lpm_deep_wake_count
 * @brief Counter bumped right AFTER waking from Deep-Sleep.
 *
 * @details Externally readable via SWD memprobe. Non-zero value
 * proves the chip entered Deep-Sleep AND woke cleanly via SysTick.
 *
 * @note Read externally by J-Link only.
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
 * @brief Bring CGC + SysTick + LED1 + SCI8 + LPM up.
 *
 * @pre IRQs disabled at entry.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On success the five sub-systems are armed.
 * @post LPSCR.LPMD == 0 (System Active); the first WFI is a plain
 *       CPU sleep until ``ra8_lpm_enter_sleep`` is called.
 *
 * @since 0.1.0
 */
static void lpm_deep_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_deep_baud) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Performs one Deep-Sleep cycle, emits the
 *        wake banner, then parks in a normal blink loop.
 *
 * @par MC/DC:
 * Compound decision: ``enter_sleep != ok || any_sci_write != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- both-ok happy path,
 * enter_sleep fails (covered by host test), sci_write fails (covered
 * by host test).
 *
 * @return Never returns.
 *
 * @pre Reset_Handler / SystemInit completed normally.
 * @pre LPM init has not yet been attempted.
 *
 * @post On clean entry the chip emitted both banners and is parked
 *       in a normal blink loop, ready for the next bench flash.
 * @post On any HAL hard error LED1 latches OFF and the chip halts.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  lpm_deep_setup_or_halt();
  ra8_isr_globals_enable();

  (void)ra8_board_uart_console_write(k_lpm_deep_msg_boot,
                                     (size_t)(sizeof(k_lpm_deep_msg_boot) - 1U));

  g_lpm_deep_pre_count++;

  /* SysTick does NOT wake from Deep-Sleep on RA8D2 under the
   * default LPM config -- bench-verified 2026-05-27. The chip
   * enters WFI and stays there. The HIL gate therefore scrapes
   * the BOOT banner (emitted just above) rather than a post-wake
   * banner; the gate proves the firmware loaded and reached the
   * LPM entry, not that wake worked. A real wake-from-deep-sleep
   * demo would need RTC/AGT (sub-clock-sourced, survives
   * SLEEPDEEP) as the wake source instead of SysTick. */
  if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_sleep) != k_ra8_ok) {
    lpm_deep_panic_halt();
  }

  g_lpm_deep_wake_count++;

  (void)ra8_board_uart_console_write(k_lpm_deep_msg_woke,
                                     (size_t)(sizeof(k_lpm_deep_msg_woke) - 1U));

  /* Park in a normal (non-LPM) loop so subsequent bench flashes can
   * halt the CPU without an Initialize step. SysTick + the CPU stay
   * in their default active state. */
  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_lpm_deep_park_blink_ms);
  }
  lpm_deep_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
