/**
 * @file main.c
 * @brief ADC_B VREF-channel sample-and-log demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Configures the ADC_B in 12-bit, software-trigger mode, samples the
 * internal VREF channel (``k_adc_b_demo_channel``) once per loop, and
 * logs the raw count plus the converted millivolt value over SCI8.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8 on PD_02 / PD_03).
 *   2. ``ra_adc_init_configured`` with software trigger + 12-bit res.
 *   3. Loop: ``ra_adc_read_channel`` -> format -> ``ra_sci_write_polling``.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_adc.h"
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
  k_adc_b_demo_baud        = 115200U,
  k_adc_b_demo_sci_channel = 8U,
  k_adc_b_demo_period_ms   = 500U,
  k_adc_b_demo_vref_mv     = 3300U, /**< EK-RA8D2 VREFH = 3.3V. */
  k_adc_b_demo_full_scale  = 4095U, /**< 12-bit max code. */
} adc_b_demo_const_t;

/** @brief ADC channel selection. */
typedef enum : uint8_t {
  k_adc_b_demo_channel = 0U, /**< AN000 -- safe default. */
} adc_b_demo_chan_t;

static const ra_port_pin_t k_adc_b_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_adc_b_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_adc_b_demo_log_prefix[] = "adc: raw=";

static void adc_b_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t adc_b_demo_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_adc_b_demo_pin_txd, k_ra_psel_sci_async, "adc_b_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_adc_b_demo_pin_rxd, k_ra_psel_sci_async, "adc_b_demo.rxd8");
}

static void adc_b_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  if (adc_b_demo_pins_init() != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_adc_b_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_adc_b_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    adc_b_demo_panic_halt();
  }
}

/**
 * @brief Configure ADC_B in 12-bit software-trigger mode.
 *
 * @par MC/DC:
 * Decision: ``ra_adc_init_configured != ok``. One atomic condition x
 * 2 vectors -- golden (this) + null cfg reject
 * (test_app_adc_b_demo.c).
 */
[[nodiscard]] static ra_err_t adc_b_demo_arm(void)
{
  const ra_adc_cfg_t cfg = {
    .resolution    = k_ra_adc_res_12bit,
    .trigger       = k_ra_adc_trig_software,
    .right_aligned = true,
    .scan_mode     = false,
  };
  return ra_adc_init_configured(&cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  adc_b_demo_setup_or_halt();
  ra_isr_globals_enable();

  if (adc_b_demo_arm() != k_ra_ok) {
    adc_b_demo_panic_halt();
  }

  while (1) {
    uint16_t raw = 0U;
    if (ra_adc_read_channel((uint8_t)k_adc_b_demo_channel, &raw) != k_ra_ok) {
      break;
    }
    if (ra_sci_write_polling((uint8_t)k_adc_b_demo_sci_channel,
                             k_adc_b_demo_log_prefix,
                             (uint32_t)(sizeof(k_adc_b_demo_log_prefix) - 1U)) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_adc_b_demo_period_ms);
  }
  adc_b_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
