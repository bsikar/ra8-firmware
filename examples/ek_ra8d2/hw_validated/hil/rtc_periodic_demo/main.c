/**
 * @file examples/ek_ra8d2/rtc_alarm/main.c
 * @brief RTC alarm + UART log demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Sets the on-chip RTC to a known seed time (2026-01-01 00:00:00),
 * schedules an alarm 5 seconds in the future via the new
 * ``ra_rtc_set_alarm`` API, enables RCR1.AIE, and polls
 * ``ra_rtc_get_status`` for the alarm-fired flag (no NVIC needed in
 * this minimal demo). When the alarm fires, the demo logs the wall
 * time over SCI8 (115200 8N1, on the J-Link OB CDC port), clears the
 * status bit, advances the seed by ten seconds, and re-arms.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8 on PD_02 / PD_03) bring-up.
 *   2. ``ra_rtc_init()`` -- start the RTC in 24-hour calendar mode.
 *   3. ``ra_rtc_set()`` to the seed datetime.
 *   4. Loop: programme the +5 s alarm, poll for the alarm flag,
 *      log "alarm fired", advance the seed by ten seconds.
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
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_rtc.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_rtc_demo_baud         = 115200U,
  k_rtc_demo_sci_channel  = 8U,
  k_rtc_demo_poll_ms      = 100U,
  k_rtc_demo_advance_secs = 10U,
  k_rtc_demo_ms_per_sec   = 1000U,
} rtc_demo_const_t;

/** @brief Calendar-arithmetic moduli for the +5 s alarm calc. */
typedef enum : uint8_t {
  k_rtc_demo_secs_per_min  = 60U,
  k_rtc_demo_mins_per_hour = 60U,
  k_rtc_demo_hours_per_day = 24U,
} rtc_demo_calendar_t;

/** @brief Alarm offset from the current RTC reading. */
typedef enum : uint16_t {
  k_rtc_demo_alarm_offset_s = 5U,
  k_rtc_demo_seed_year_lo   = 26U,   /**< 2026 - 2000. */
  k_rtc_demo_year_base      = 2000U, /**< Calendar epoch base for the year field. */
  k_rtc_demo_seed_month     = 1U,
  k_rtc_demo_seed_day       = 1U,
} rtc_demo_seed_t;

/** @brief SCI8 pin map -- same as uart_hello. */
static const ra_port_pin_t k_rtc_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_rtc_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_rtc_demo_log_msg[]  = "rtc_per: tick\r\n";
static const uint8_t k_rtc_demo_boot_msg[] = "rtc_per: boot\r\n";

static void rtc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t rtc_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_rtc_demo_pin_txd, k_ra_psel_sci_async, "rtc_alarm.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_rtc_demo_pin_rxd, k_ra_psel_sci_async, "rtc_alarm.rxd8");
}

static void rtc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  if (rtc_demo_pins_init() != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_rtc_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_rtc_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  if (ra_rtc_init() != k_ra_ok) {
    rtc_demo_panic_halt();
  }
  const ra_rtc_datetime_t seed = {
    .year    = (uint16_t)(k_rtc_demo_year_base + k_rtc_demo_seed_year_lo),
    .month   = (uint8_t)k_rtc_demo_seed_month,
    .day     = (uint8_t)k_rtc_demo_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  if (ra_rtc_set(&seed) != k_ra_ok) {
    rtc_demo_panic_halt();
  }
}

/**
 * @brief Program a +5 s alarm relative to ``now`` and enable AIE.
 *
 * @par MC/DC:
 * Compound decision: ``ra_rtc_set_alarm != ok ||
 * ra_rtc_set_irq_enable != ok``. Two atomic conditions x N+1 = 3
 * vectors -- both ok (steady state) plus each branch fails
 * (covered in test_app_rtc_alarm.c).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t rtc_demo_arm_alarm(const ra_rtc_datetime_t* now)
{
  ra_rtc_datetime_t a = *now;
  uint16_t          s = (uint16_t)a.second + (uint16_t)k_rtc_demo_alarm_offset_s;
  a.second            = (uint8_t)(s % (uint16_t)k_rtc_demo_secs_per_min);
  uint16_t add_min    = (uint16_t)(s / (uint16_t)k_rtc_demo_secs_per_min);
  uint16_t m          = (uint16_t)a.minute + add_min;
  a.minute            = (uint8_t)(m % (uint16_t)k_rtc_demo_mins_per_hour);
  uint16_t add_hr     = (uint16_t)(m / (uint16_t)k_rtc_demo_mins_per_hour);
  uint16_t h          = (uint16_t)a.hour + add_hr;
  a.hour              = (uint8_t)(h % (uint16_t)k_rtc_demo_hours_per_day);
  ra_err_t err        = ra_rtc_set_alarm(&a);
  if (err != k_ra_ok) {
    return err;
  }
  /* Enable BOTH alarm and periodic IRQ flags so the demo gates on
   * whichever fires first. The periodic interval is selected by
   * RCR1.PES inside ra_rtc_init's default config; the bench cares
   * only that ra_rtc_get_status reports a periodic-or-alarm event. */
  return ra_rtc_set_irq_enable(
    (uint8_t)((uint8_t)k_ra_rtc_irq_alarm | (uint8_t)k_ra_rtc_irq_periodic));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  rtc_demo_setup_or_halt();
  ra_isr_globals_enable();

  /* Boot banner -- emit before the RTC poll loop so the HIL host can
   * confirm the firmware booted even when the sub-clock crystal is not
   * running and the alarm-fired path never reaches its own write. */
  (void)ra_sci_write_polling((uint8_t)k_rtc_demo_sci_channel,
                             k_rtc_demo_boot_msg,
                             (uint32_t)(sizeof(k_rtc_demo_boot_msg) - 1U));

  while (1) {
    ra_rtc_datetime_t now = {};
    if (ra_rtc_get(&now) != k_ra_ok) {
      break;
    }
    if (rtc_demo_arm_alarm(&now) != k_ra_ok) {
      break;
    }

    /* Poll for the periodic flag (or alarm as a fallback). On real
     * silicon RCR1.PIE drives an NVIC line, but for this demo we
     * just spin against the status bit. */
    uint8_t       status   = 0U;
    const uint8_t any_mask = (uint8_t)k_ra_rtc_irq_alarm | (uint8_t)k_ra_rtc_irq_periodic;
    do {
      ra_delay_ms(k_rtc_demo_poll_ms);
      if (ra_rtc_get_status(&status) != k_ra_ok) {
        rtc_demo_panic_halt();
      }
    } while ((status & any_mask) == 0U);

    if (ra_sci_write_polling((uint8_t)k_rtc_demo_sci_channel,
                             k_rtc_demo_log_msg,
                             (uint32_t)(sizeof(k_rtc_demo_log_msg) - 1U)) != k_ra_ok) {
      break;
    }
    if (ra_rtc_clear_status(
          (uint8_t)((uint8_t)k_ra_rtc_irq_alarm | (uint8_t)k_ra_rtc_irq_periodic)) != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_rtc_demo_advance_secs * (uint32_t)k_rtc_demo_ms_per_sec);
  }
  rtc_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
