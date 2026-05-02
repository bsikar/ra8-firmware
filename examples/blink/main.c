/**
 * @file main.c
 * @brief Minimal LED-blink demo for EK-RA8D2 (single LED, BSP-driven)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives the EK-RA8D2 user-LED1 (P600, blue) at 1 Hz using the
 * board-support layer (``libs/ra_board_ek_ra8d2``). This is the
 * smallest possible smoke test that proves the chip booted into
 * Reset_Handler / SystemInit / main and that the BSP + HAL
 * GPIO/SysTick stack is alive.
 *
 * For the multi-LED + HAL-toggle variant see ``examples/blink_hal``.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}`
 * means -- application-layer code that runs in the Secure world.
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
  k_blink_half_period_ms  = 500U, /**< 500 ms on / 500 ms off -> 1 Hz square wave. */
} blink_const_t;

/**
 * @brief Park the CPU forever in WFI -- used as a panic stop.
 */
static void blink_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_time_init(k_blink_cpu_hz_at_reset) != k_ra_ok) {
    blink_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    blink_panic_halt();
  }

  ra_isr_globals_enable();

  while (1) {
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_blink_half_period_ms);
  }

  blink_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
