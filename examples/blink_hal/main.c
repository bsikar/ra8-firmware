/**
 * @file main.c
 * @brief HAL-based LED-blink demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Same blink behaviour as `examples/blink/main.c` but built on the
 * project's HAL libraries instead of raw register pokes. This is the
 * canonical "is the HAL stack alive?" smoke test.
 *
 * Compared to the raw-register `blink` example, this one:
 *   - Uses ``ra_gpio_output_init()`` / ``ra_gpio_toggle()`` from
 *     `libs/ra_hal/src/ra_gpio.c` instead of writing PFS / PCNTR1
 *     directly. The HAL handles the PWPR unlock dance, pin-validator
 *     reservation, and the per-pin PFS field encoding.
 *   - Uses ``ra_time_init()`` + ``ra_delay_ms()`` from
 *     `libs/ra_core/src/ra_time.c` for delays. SysTick is programmed
 *     to fire every 1 ms; ``ra_delay_ms`` is a busy-wait against the
 *     resulting tick counter.
 *
 * What this demo deliberately skips:
 *   - ``ra_cgc_init()`` -- PLL bring-up to 1 GHz. The CGC driver does
 *     PRCR-protected register writes that HardFault on the bare chip
 *     today; we run on the reset-default MOCO ~8.4 MHz.
 *   - ``ra_infrastructure_init()`` -- log + stack canary + pin validator
 *     boot-time setup. The pin validator IS used implicitly via
 *     `ra_gpio_output_init` (it claims each pin); we just don't run
 *     the standalone init.
 *   - TrustZone / SAU programming.
 *
 * @par Architectural ring
 * See `docs/RING_AND_WORLD.md` for what `[Ring 6 / APP] {World: S}`
 * means. Short version: this file is application-layer code that
 * runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"

/**
 * @brief CPU clock at reset (MOCO ~8.4 MHz on RA8D2 before CGC bring-up).
 *
 * @details
 * Empirically measured via DWT.CYCCNT in a single JLinkExe session:
 * 42,068,697 cycles in 5.000 s -> 8,413,739 Hz. After
 * ``ra_cgc_init`` brings up HOCO + PLL this should be replaced with
 * the actual operating clock.
 */
typedef enum : uint32_t {
  k_blink_cpu_hz_at_reset = 8400000U,
} blink_clock_t;

/**
 * @brief Half-period of the visible blink, in milliseconds.
 *
 * @details
 * 500 ms on / 500 ms off gives a 1 Hz square wave -- comfortable for
 * the eye. Adjust to taste; ``ra_delay_ms`` accepts any 32-bit value.
 */
typedef enum : uint32_t {
  k_blink_half_period_ms = 500U,
} blink_period_t;

/**
 * @brief Configure all three EK-RA8D2 user LEDs as outputs.
 *
 * @details
 * The pin assignments come from `libs/ra_core/inc/ra_port_constants.h`.
 * Each pin is initialised driving low so the LEDs start in a known
 * state before the toggle loop begins.
 *
 * @return ``ra_err_t`` error code from the first failing GPIO init,
 *         or ``k_ra_ok`` if all three pins were configured.
 *
 * @retval k_ra_ok               Every LED pin is now a digital output.
 * @retval k_ra_err_invalid_arg  A pin id was rejected by the HAL.
 * @retval k_ra_err_pin_conflict A pin was already claimed.
 *
 * @pre IOPORT module is reachable (true on reset).
 * @pre Caller is single-threaded (init context).
 *
 * @post On success, P6_00, P3_03, and P10_07 are output-low.
 * @post On failure, partial state is acceptable -- the demo will halt.
 *
 * @note Not thread-safe; call once from boot.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t blink_pins_init(void)
{
  ra_err_t err = ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_gpio_output_init(k_ra_pin_led2, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_output_init(k_ra_pin_led3, k_ra_level_low);
}

/**
 * @brief Toggle all three LED pins atomically (one HAL call each).
 *
 * @details
 * `ra_gpio_toggle` is safe to interleave -- each call locks PWPR, flips
 * one PODR bit, and unlocks again. For a 3-pin demo the per-call cost
 * is irrelevant.
 *
 * @return ``ra_err_t`` from the first failing toggle, or k_ra_ok.
 *
 * @retval k_ra_ok               All three pins toggled.
 * @retval k_ra_err_invalid_arg  A pin id became invalid (shouldn't happen).
 *
 * @pre `blink_pins_init()` has succeeded.
 *
 * @post Each LED pin's output latch is inverted from its prior value.
 *
 * @note Not thread-safe; intended for the main blink loop only.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t blink_pins_toggle_all(void)
{
  ra_err_t err = ra_gpio_toggle(k_ra_pin_led1);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_gpio_toggle(k_ra_pin_led2);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_toggle(k_ra_pin_led3);
}

/**
 * @brief Application entry. Sets up GPIO + SysTick, runs the blink.
 *
 * @return Never returns. Type is `int32_t` per the project's
 *         no-bare-`int` rule; gcc's `-Wmain` is silenced via pragma.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry into the blink loop, the function never returns.
 * @post On any HAL init failure, the function halts in `__WFI`.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  /* Driver init runs with PRIMASK set (SystemInit masks IRQs at boot
   * so partial state isn't accidentally driven by an interrupt). */
  if (ra_time_init(k_blink_cpu_hz_at_reset) != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (blink_pins_init() != k_ra_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  /* Drivers are ready -- drop the global mask so SysTick_Handler
   * (and any other ISRs we register later) can dispatch. From here
   * on, IRQs run normally during the main loop. */
  ra_isr_globals_enable();

  while (1) {
    if (blink_pins_toggle_all() != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_blink_half_period_ms);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
