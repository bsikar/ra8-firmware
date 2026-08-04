/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/blink/main.c
 * @brief Minimal LED-blink demo for EK-RA8D2 (single LED, BSP-driven)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives the EK-RA8D2 user-LED1 (P600, blue) at 1 Hz using the
 * board-support layer (``libs/ra8_board_ek_ra8d2``). This is the
 * smallest possible HIL test that proves the chip booted into
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
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/**
 * @brief CPU clock at reset (MOCO ~8.4 MHz on RA8D2 before CGC bring-up).
 */
typedef enum : uint32_t {
  k_blink_cpu_hz_at_reset = 8400000U, /**< Blink CPU Hz at reset.                      */
  k_blink_half_period_ms  = 500U,     /**< 500 ms on / 500 ms off -> 1 Hz square wave. */
} blink_const_t;

/**
 * @var g_blink_tick
 * @brief HIL liveness counter -- incremented each time the LED toggles.
 *
 * @details
 * scripts/hil/jlink_memprobe.sh halts the chip via SWD, reads this
 * counter, lets the chip run for N seconds, halts again, and asserts
 * the counter advanced by >= HIL_PROBE_MIN_ADVANCE. This is a much
 * stronger gate than HIL_MODE=alive (which only checks PC is in MRAM
 * and CycleCnt advances): a chip stuck in a busy-wait inside
 * ra8_delay_ms would pass alive-mode but fail memprobe because the
 * main loop never iterated.
 *
 * `volatile` keeps the increment out of the optimiser's hands (it
 * would otherwise hoist or eliminate the dead store, since nothing in
 * the firmware reads the counter). The symbol is also non-static so
 * the linker keeps it visible to arm-none-eabi-nm without
 * @c -Wl,--gc-sections culling it.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_blink_tick = 0U;

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
  if (ra8_time_init(k_blink_cpu_hz_at_reset) != k_ra8_ok) {
    blink_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    blink_panic_halt();
  }

  ra8_isr_globals_enable();

  while (1) {
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    g_blink_tick += 1U;
    ra8_delay_ms(k_blink_half_period_ms);
  }

  blink_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
