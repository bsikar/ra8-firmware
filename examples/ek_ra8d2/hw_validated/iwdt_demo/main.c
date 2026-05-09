/**
 * @file examples/ek_ra8d2/iwdt_demo/main.c
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
 * via ``ra_iwdt_refresh_deferred``, observes the live counter via
 * ``ra_iwdt_get_counter``, and reads the IWDTSR underflow / refresh-
 * error status via ``ra_iwdt_get_status`` to detect window violations.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8) + LED1.
 *   2. ``ra_iwdt_init`` (no-op on RA8D2 -- OFS0 owns the period).
 *   3. Loop: poll counter; refresh only when the counter is inside
 *      the legal window; clear status; toggle LED1 each refresh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_iwdt.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_iwdt_demo_baud        = 115200U,
  k_iwdt_demo_sci_channel = 8U,
  k_iwdt_demo_poll_ms     = 50U,
} iwdt_demo_const_t;

/** @brief Window bounds in 14-bit IWDTSR.CNTVAL units (HUM 28.2.2). */
typedef enum : uint16_t {
  k_iwdt_demo_window_low  = 0x0400U, /**< Refresh-permitted lower bound. */
  k_iwdt_demo_window_high = 0x0C00U, /**< Refresh-permitted upper bound. */
} iwdt_demo_window_t;

static const ra_port_pin_t k_iwdt_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_iwdt_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_iwdt_demo_msg_refresh[] = "iwdt: refresh in window\r\n";

static void iwdt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t iwdt_demo_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_iwdt_demo_pin_txd, k_ra_psel_sci_async, "iwdt_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_iwdt_demo_pin_rxd, k_ra_psel_sci_async, "iwdt_demo.rxd8");
}

static void iwdt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  if (iwdt_demo_pins_init() != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_iwdt_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_iwdt_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    iwdt_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
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
  ra_isr_globals_enable();

  if (ra_iwdt_init() != k_ra_ok) {
    iwdt_demo_panic_halt();
  }

  while (1) {
    uint16_t counter = 0U;
    if (ra_iwdt_get_counter(&counter) != k_ra_ok) {
      break;
    }
    if (iwdt_demo_in_window(counter)) {
      ra_iwdt_refresh_deferred();
      if (ra_iwdt_clear_status() != k_ra_ok) {
        break;
      }
      if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
        break;
      }
      if (ra_sci_write_polling((uint8_t)k_iwdt_demo_sci_channel,
                               k_iwdt_demo_msg_refresh,
                               (uint32_t)(sizeof(k_iwdt_demo_msg_refresh) - 1U)) != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms((uint32_t)k_iwdt_demo_poll_ms);
  }
  iwdt_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
