/**
 * @file examples/ek_ra8d2/hw_validated/hil/blink_hal/src/main.c
 * @brief HAL-based LED-blink demo for EK-RA8D2 (using ra8_board_ek_ra8d2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives all three EK-RA8D2 user LEDs at a 1 Hz square wave using the
 * board-support layer (``libs/ra8_board_ek_ra8d2``). Compared to the
 * earlier raw-pin variant in ``examples/ek_ra8d2/hw_validated/hil/blink/src/main.c`` this demo
 * doesn't reach into ``ra8_port_constants.h`` for the LED pin numbers
 * directly; it speaks in board coordinates ("LED1, LED2, LED3") and
 * lets the BSP look up the right RA8D2 pins per UM Table 24 p 31.
 *
 * Sequence:
 *   1. ``ra8_time_init()`` -- 1 ms SysTick on the reset-default MOCO.
 *   2. ``ra8_board_led_init(led)`` for each LED (P600 / P303 / PA07).
 *   3. ``ra8_isr_globals_enable()`` to let SysTick_Handler run.
 *   4. Loop: ``ra8_board_led_toggle(led)`` for each LED, then sleep.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/**
 * @brief CPU clock at reset (MOCO ~8.4 MHz on RA8D2 before CGC bring-up).
 */
typedef enum : uint32_t {
  k_blink_cpu_hz_at_reset = 8400000U, /**< Blink CPU Hz at reset. */
} blink_clock_t;

/** @brief Half-period of the visible blink, in milliseconds. */
typedef enum : uint32_t {
  k_blink_half_period_ms = 500U, /**< Blink half period ms. */
} blink_period_t;

/**
 * @var g_blink_hal_tick
 * @brief HIL liveness counter -- incremented each main-loop iteration.
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
volatile uint32_t g_blink_hal_tick = 0U;

/* Forward declarations -- definitions appear after main() so the
 * audit_init_order linter sees the canonical CGC -> TIME -> peripheral
 * sequence in source order. */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_blink_pins_init(void);
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_blink_pins_toggle_all(void);

void main(void)
{
  if (ra8_time_init(k_blink_cpu_hz_at_reset) != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  if (internal_blink_pins_init() != k_ra8_ok) {
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  ra8_isr_globals_enable();

  while (1) {
    if (internal_blink_pins_toggle_all() != k_ra8_ok) {
      break;
    }
    g_blink_hal_tick += 1U;
    ra8_delay_ms(k_blink_half_period_ms);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Configure all three EK-RA8D2 user LEDs as outputs.
 *
 * @details Walks every board LED identifier in declaration order and stops at
 *          the first BSP initialization error.
 *
 * @return First HAL/BSP error encountered, or k_ra8_ok on success.
 *
 * @retval k_ra8_ok                Every LED pin is now a digital output.
 * @retval k_ra8_err_invalid_arg   A LED id was rejected by the BSP.
 * @retval k_ra8_err_gpio_conflict A pin was already claimed.
 *
 * @pre HAL pin validator initialized.
 * @pre The board LED descriptor table is available for all enumerated IDs.
 * @post LED1..LED3 are output-low.
 * @post On failure no LED after the failing identifier is initialized.
 *
 * @note Boot-context helper; concurrent LED initialization is unsupported.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_blink_pins_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    const ra8_err_t err = ra8_board_led_init((ra8_board_led_id_t)i);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Toggle every user LED in sequence.
 *
 * @details Visits each board LED identifier once and returns immediately if a
 *          BSP toggle fails, preserving the first error for the caller.
 *
 * @return First BSP error encountered, or k_ra8_ok after all toggles.
 * @retval k_ra8_ok                All three pins toggled.
 * @retval k_ra8_err_invalid_arg   A LED id became invalid (shouldn't happen).
 *
 * @pre ::internal_blink_pins_init succeeded.
 * @pre The caller owns this demo's LED update sequence.
 * @post Each LED's output latch is inverted from its prior value.
 * @post On failure LEDs after the failing identifier are left unchanged.
 *
 * @note Not atomic across LEDs; an intermediate error can leave a mixed phase.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_blink_pins_toggle_all(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_board_led_count; ++i) {
    const ra8_err_t err = ra8_board_led_toggle((ra8_board_led_id_t)i);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}
