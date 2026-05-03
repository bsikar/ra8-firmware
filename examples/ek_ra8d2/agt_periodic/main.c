/**
 * @file main.c
 * @brief AGT 1 Hz tick + LED1 blink demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + UART (SCI8 on PD_02 / PD_03), starts AGT0
 * in free-running mode with a reload value chosen to underflow at
 * roughly 1 Hz, then sits in the main loop polling AGTCR for the
 * underflow flag. Each underflow toggles board LED1 (BLUE) and emits a
 * one-line tick log over the J-Link OB CDC console.
 *
 * The demo deliberately does **not** wire AGT0 into NVIC / ra_isr -- it
 * polls the AGTCR status bits via ``ra_agt_get_status`` to keep the
 * code path identical between the EK-RA8D2 target build and the host
 * unit-test build (``RA_SIMULATOR_MODE``).
 *
 * Sequence:
 *   1. CGC + SysTick + UART bring-up (panic-halt on any error).
 *   2. ``ra_board_led_init(LED1)``.
 *   3. ``ra_agt_start_free_run(0, k_agt_periodic_reload)``.
 *   4. Loop: poll AGTCR, on underflow: clear, toggle LED1, log tick.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_agt.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_agt_periodic_baud        = 115200U,
  k_agt_periodic_sci_channel = 8U,
  k_agt_periodic_poll_ms     = 10U,
} agt_periodic_const_t;

/** @brief AGT channel + reload (16-bit, ~1 Hz at PCLKB / 8192 div). */
typedef enum : uint16_t {
  k_agt_periodic_channel = 0U,
  /* Reload tuned for the simulator -- on real silicon AGT prescaler
   * setup happens inside ra_agt_start_free_run. */
  k_agt_periodic_reload = 0x7FFFU,
} agt_periodic_timer_t;

/** @brief AGTCR underflow status bit (HUM Ch 24, AGTCR.TUNDF). */
typedef enum : uint8_t {
  k_agt_periodic_undf_bit = 0x20U,
} agt_periodic_status_t;

/** @brief SCI8 pin map -- same as uart_hello / rtc_alarm. */
static const ra_port_pin_t k_agt_periodic_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_agt_periodic_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_agt_periodic_log_msg[] = "agt: tick\r\n";

static void agt_periodic_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t agt_periodic_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_agt_periodic_pin_txd, k_ra_psel_sci_async, "agt_periodic.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_agt_periodic_pin_rxd, k_ra_psel_sci_async, "agt_periodic.rxd8");
}

static void agt_periodic_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  if (agt_periodic_pins_init() != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_agt_periodic_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_agt_periodic_sci_channel, &sci_cfg) != k_ra_ok) {
    agt_periodic_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    agt_periodic_panic_halt();
  }
}

/**
 * @brief Arm AGT0 in free-running mode.
 *
 * @par MC/DC:
 * Compound decision: ``ra_agt_start_free_run != ok``. One atomic
 * condition x 2 vectors -- ok (golden) and bad-channel reject
 * (covered in test_app_agt_periodic.c).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t agt_periodic_arm(void)
{
  return ra_agt_start_free_run((uint8_t)k_agt_periodic_channel, (uint16_t)k_agt_periodic_reload);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  agt_periodic_setup_or_halt();
  ra_isr_globals_enable();

  if (agt_periodic_arm() != k_ra_ok) {
    agt_periodic_panic_halt();
  }

  while (1) {
    uint8_t status = 0U;
    if (ra_agt_get_status((uint8_t)k_agt_periodic_channel, &status) != k_ra_ok) {
      break;
    }
    if ((status & (uint8_t)k_agt_periodic_undf_bit) != 0U) {
      if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
        break;
      }
      if (ra_sci_write_polling((uint8_t)k_agt_periodic_sci_channel,
                               k_agt_periodic_log_msg,
                               (uint32_t)(sizeof(k_agt_periodic_log_msg) - 1U)) != k_ra_ok) {
        break;
      }
      /* Re-arm: stop + start clears AGTCR.TUNDF on real silicon. */
      if (ra_agt_stop((uint8_t)k_agt_periodic_channel) != k_ra_ok) {
        break;
      }
      if (agt_periodic_arm() != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms((uint32_t)k_agt_periodic_poll_ms);
  }
  agt_periodic_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
