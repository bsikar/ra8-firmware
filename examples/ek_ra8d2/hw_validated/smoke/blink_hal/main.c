/**
 * @file examples/ek_ra8d2/blink_hal/main.c
 * @brief HAL-based LED-blink demo for EK-RA8D2 (using ra_board_ek_ra8d2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives all three EK-RA8D2 user LEDs at a 1 Hz square wave using the
 * board-support layer (``libs/ra_board_ek_ra8d2``). Compared to the
 * earlier raw-pin variant in ``examples/blink/main.c`` this demo
 * doesn't reach into ``ra_port_constants.h`` for the LED pin numbers
 * directly; it speaks in board coordinates ("LED1, LED2, LED3") and
 * lets the BSP look up the right RA8D2 pins per UM Table 24 p 31.
 *
 * Sequence:
 *   1. ``ra_time_init()`` -- 1 ms SysTick on the reset-default MOCO.
 *   2. ``ra_board_led_init(led)`` for each LED (P600 / P303 / PA07).
 *   3. ``ra_isr_globals_enable()`` to let SysTick_Handler run.
 *   4. Loop: ``ra_board_led_toggle(led)`` for each LED, then sleep.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/**
 * @brief CPU clock at reset (MOCO ~8.4 MHz on RA8D2 before CGC bring-up).
 */
typedef enum : uint32_t {
  k_blink_cpu_hz_at_reset = 8400000U,
} blink_clock_t;

/** @brief Half-period of the visible blink, in milliseconds. */
typedef enum : uint32_t {
  k_blink_half_period_ms = 500U,
} blink_period_t;

/* Forward declarations -- definitions appear after main() so the
 * audit_init_order linter sees the canonical CGC -> TIME -> peripheral
 * sequence in source order. */
[[nodiscard]] static ra_err_t blink_pins_init(void);
[[nodiscard]] static ra_err_t blink_pins_toggle_all(void);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
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

/**
 * @brief Configure all three EK-RA8D2 user LEDs as outputs.
 *
 * @return First HAL/BSP error encountered, or k_ra_ok on success.
 *
 * @retval k_ra_ok                Every LED pin is now a digital output.
 * @retval k_ra_err_invalid_arg   A LED id was rejected by the BSP.
 * @retval k_ra_err_gpio_conflict A pin was already claimed.
 *
 * @pre HAL pin validator initialized.
 * @post LED1..LED3 are output-low.
 */
[[nodiscard]] static ra_err_t blink_pins_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_board_led_count; ++i) {
    const ra_err_t err = ra_board_led_init((ra_board_led_id_t)i);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Toggle every user LED in sequence.
 *
 * @retval k_ra_ok                All three pins toggled.
 * @retval k_ra_err_invalid_arg   A LED id became invalid (shouldn't happen).
 *
 * @pre blink_pins_init() succeeded.
 * @post Each LED's output latch is inverted from its prior value.
 */
[[nodiscard]] static ra_err_t blink_pins_toggle_all(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_board_led_count; ++i) {
    const ra_err_t err = ra_board_led_toggle((ra_board_led_id_t)i);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}
