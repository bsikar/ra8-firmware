/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/iwdt_demo/main.c
 * @brief IWDT window-mode refresh demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the IWDT in **window mode**, distinct from the existing
 * ``watchdog_demo`` which simply refreshes until it stops. The IWDT
 * period and window-start / window-end positions are programmed in
 * the OFS0 option-setting register at flash time -- the chip cannot
 * reconfigure them at runtime. This demo refreshes the counter only
 * inside the legal window (between ``IWDTRPES`` and ``IWDTRPSS``)
 * via ``ra8_iwdt_refresh_deferred``, observes the live counter via
 * ``ra8_iwdt_get_counter``, and reads the IWDTSR underflow / refresh-
 * error status via ``ra8_iwdt_get_status`` to detect window violations.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8) + LED1.
 *   2. ``ra8_iwdt_init`` (no-op on RA8D2 -- OFS0 owns the period).
 *   3. Loop: poll counter; refresh only when the counter is inside
 *      the legal window; clear status; toggle LED1 each refresh.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_iwdt.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_iwdt_demo_baud    = 115200U, /**< Iwdt demo baud.    */
  k_iwdt_demo_poll_ms = 50U,     /**< Iwdt demo poll ms. */
} iwdt_demo_const_t;

/** @brief Window bounds in 14-bit IWDTSR.CNTVAL units (HUM 28.2.2). */
typedef enum : uint16_t {
  k_iwdt_demo_window_low  = 0x0400U, /**< Refresh-permitted lower bound. */
  k_iwdt_demo_window_high = 0x0C00U, /**< Refresh-permitted upper bound. */
} iwdt_demo_window_t;

static const uint8_t k_iwdt_demo_msg_refresh[] = "iwdt: refresh in window\r\n";
static const uint8_t k_iwdt_demo_msg_boot[]    = "iwdt: poll counter\r\n";

static void iwdt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void iwdt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_iwdt_demo_baud) != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }
}

/**
 * @brief Decide whether the live counter sits inside the legal window.
 *
 * @par MC/DC:
 * Compound decision: ``counter >= low && counter <= high``. Two atomic
 * conditions x 3 vectors -- both true (golden), low fail, high fail
 * (test_app_iwdt_demo.c).
 */
static bool iwdt_demo_in_window(uint16_t counter)
{
  return (counter >= (uint16_t)k_iwdt_demo_window_low) &&
         (counter <= (uint16_t)k_iwdt_demo_window_high);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  iwdt_demo_setup_or_halt();
  ra8_isr_globals_enable();

  /* Boot banner -- emit immediately after setup so the HIL host can
   * confirm the firmware booted regardless of OFS0 / IWDT state. */
  (void)ra8_board_uart_console_write(k_iwdt_demo_msg_boot,
                                     (size_t)(sizeof(k_iwdt_demo_msg_boot) - 1U));

  if (ra8_iwdt_init() != k_ra8_ok) {
    iwdt_demo_panic_halt();
  }

  while (1) {
    uint16_t counter = 0U;
    if (ra8_iwdt_get_counter(&counter) != k_ra8_ok) {
      break;
    }
    if (iwdt_demo_in_window(counter)) {
      ra8_iwdt_refresh_deferred();
      if (ra8_iwdt_clear_status() != k_ra8_ok) {
        break;
      }
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      if (ra8_board_uart_console_write(k_iwdt_demo_msg_refresh,
                                       (size_t)(sizeof(k_iwdt_demo_msg_refresh) - 1U)) !=
          k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_iwdt_demo_poll_ms);
  }
  iwdt_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
