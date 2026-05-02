/**
 * @file main.c
 * @brief Clock bring-up smoke test for EK-RA8D2 (CGC: HOCO + PLL)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip from reset-default MOCO (~8.4 MHz) up to its rated
 * operating speed via the CGC driver, then blinks the user LEDs at a
 * stopwatch-checkable 1 Hz so we can confirm SysTick timing matches
 * the new clock rate.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- HOCO on, PLL1 locked, CPUCLK0 + bus
 *      clocks live. This is the first time the CGC driver runs on
 *      real silicon end-to-end.
 *   2. ``ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz)`` -- read
 *      back the configured rate so SysTick can use the actual value
 *      rather than a hardcoded constant.
 *   3. ``ra_time_init(hz)`` -- programme SysTick for a 1 ms tick at
 *      the new clock rate.
 *   4. ``ra_gpio_output_init()`` for each LED, then a 1 Hz toggle
 *      loop using ``ra_delay_ms(500)``.
 *
 * Verification: the LEDs should toggle at exactly 1 Hz on a stopwatch
 * (within whatever PLL accuracy the chip guarantees -- typically
 * tens of ppm). If the period is off by orders of magnitude, the
 * reported CPUCLK0 value disagrees with the actual silicon clock and
 * something in the CGC bring-up went wrong.
 *
 * Compared to ``examples/blink_hal``: that demo runs on MOCO ~8.4 MHz
 * and ``ra_delay_ms(500)`` is therefore ~500 ms. This demo runs on
 * the real CPUCLK0 (e.g. ~480 MHz post-PLL on Cortex-M85), so any
 * SysTick-arithmetic bug in ``ra_time.c`` will show up immediately
 * as a wrong-by-100x blink rate.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"

/** @brief Half-period of the visible blink, in milliseconds. */
typedef enum : uint32_t {
  k_clock_check_half_period_ms = 500U,
} clock_check_period_t;

/**
 * @brief Configure all three EK-RA8D2 user LEDs as outputs (low).
 *
 * @return Error code from the first failing GPIO init or k_ra_ok.
 *
 * @retval k_ra_ok               Every LED pin is now a digital output.
 * @retval k_ra_err_invalid_arg  A pin id was rejected by the HAL.
 * @retval k_ra_err_gpio_conflict A pin was already claimed.
 *
 * @pre IOPORT module is reachable (true on reset).
 * @pre Caller is single-threaded init context.
 *
 * @post On success P6_00, P3_03, P10_07 are output-low.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t clock_check_pins_init(void)
{
  ra_err_t err = ra_board_led_init(k_ra_board_led1);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_board_led_init(k_ra_board_led2);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_board_led_init(k_ra_board_led3);
}

/**
 * @brief Toggle all three LED pins (one HAL call each).
 *
 * @return Error code from the first failing toggle or k_ra_ok.
 *
 * @retval k_ra_ok               All three pins toggled.
 * @retval k_ra_err_invalid_arg  A pin id became invalid (shouldn't happen).
 *
 * @pre `clock_check_pins_init()` has succeeded.
 *
 * @post Each LED's output latch is inverted from its prior value.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t clock_check_pins_toggle_all(void)
{
  ra_err_t err = ra_board_led_toggle(k_ra_board_led1);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_board_led_toggle(k_ra_board_led2);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_board_led_toggle(k_ra_board_led3);
}

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void clock_check_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Application entry. Lights HOCO + PLL, then runs a 1 Hz blink.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the toggle loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_cgc_init() != k_ra_ok) {
    clock_check_panic_halt();
  }

  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    clock_check_panic_halt();
  }

  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    clock_check_panic_halt();
  }

  if (clock_check_pins_init() != k_ra_ok) {
    clock_check_panic_halt();
  }

  ra_isr_globals_enable();

  while (1) {
    if (clock_check_pins_toggle_all() != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_clock_check_half_period_ms);
  }

  clock_check_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
