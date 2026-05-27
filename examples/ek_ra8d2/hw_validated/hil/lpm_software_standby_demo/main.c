/**
 * @file examples/ek_ra8d2/lpm_software_standby_demo/main.c
 * @brief Software Standby (LPSCR.LPMD = 0x5) + RTC periodic wake demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates Software Standby mode on the EK-RA8D2. Software
 * Standby gates almost every clock domain except the sub-clock
 * oscillator (SOSC) and a small set of always-on wake-up
 * detectors -- including the RTC. The demo wires the RTC alarm onto
 * WUPEN0.RTCALMWUPEN so a +5 s RTC alarm wakes the CPU out of
 * LPMD = 0x5.
 *
 * Boot flow:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. Emit boot banner ``"lpm_swstd: boot\r\n"`` -- this is what
 *      the HIL harness gates on (see ``hil.conf``).
 *   3. RTC init + seed time + arm alarm + enable RCR1.AIE.
 *   4. Arm WUPEN0.RTCALMWUPEN so the alarm cancels Software Standby.
 *   5. ``ra_lpm_enter_sleep(k_ra_sleep_mode_software_std)``.
 *   6. On wake, re-arm the alarm +5 s in the future and loop.
 *
 * @par SOSC caveat
 * The sub-clock crystal on the bare EK-RA8D2 has been observed to be
 * intermittent (see ``memory/project_eth_*`` notes for the same SOSC
 * failure mode in other apps). If SOSC is not ticking, the RTC alarm
 * never fires and the WFI hangs indefinitely. The HIL gate is
 * deliberately boot-banner-only so the bench reports "this firmware
 * built and ran main() to the standby entry point" even when the
 * crystal is silent. The wake path itself is verified by direct
 * benchwork (debugger / scope), not by the automated HIL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_lpm_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_lpm.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_rtc.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_lpm_swstd_baud        = 115200U,
  k_lpm_swstd_sci_channel = 8U,
} lpm_swstd_const_t;

/** @brief Single-byte demo constants. */
typedef enum : uint8_t {
  k_lpm_swstd_alarm_offset_s = 5U,
  k_lpm_swstd_seed_year_lo   = 26U,
  k_lpm_swstd_seed_month     = 1U,
  k_lpm_swstd_seed_day       = 1U,
  k_lpm_swstd_secs_per_min   = 60U,
  k_lpm_swstd_mins_per_hour  = 60U,
  k_lpm_swstd_hours_per_day  = 24U,
} lpm_swstd_byte_t;

/** @brief SCI8 pinout on the EK-RA8D2 J-Link CDC channel. */
static const ra_port_pin_t k_lpm_swstd_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_lpm_swstd_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Boot banner -- this is the HIL gate string. */
static const uint8_t k_lpm_swstd_boot_msg[] = "lpm_swstd: boot\r\n";

/**
 * @var g_lpm_swstd_wake_count
 * @brief Liveness counter -- bumped after each Software-Standby wake.
 *
 * @details
 * Exposed for ad-hoc J-Link debugging when the SOSC is actually
 * ticking; not gated on by the HIL config (see file header).
 *
 * @since 0.1.0
 */
volatile uint32_t g_lpm_swstd_wake_count = 0U;

static void lpm_swstd_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t lpm_swstd_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_lpm_swstd_pin_txd, k_ra_psel_sci_async, "lpm_swstd.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_lpm_swstd_pin_rxd, k_ra_psel_sci_async, "lpm_swstd.rxd8");
}

/**
 * @brief Bring CGC + SysTick + SCI8 + RTC + LPM block up.
 *
 * @details
 * Same shape as ``lpm_idle_demo`` but adds RTC init + alarm seed.
 * Returns the PCLKA clock rate via @p out_pclka_hz so the SCI8 baud
 * generator can be programmed against it.
 *
 * @param[out] out_pclka_hz Receives the PCLKA frequency in Hz.
 *
 * @pre IRQs disabled (Reset_Handler default).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post LPM block has LPSCR.LPMD = 0 (System Active) so the first
 *       WFI is a plain CPU sleep until ``ra_lpm_enter_sleep`` is
 *       called.
 *
 * @since 0.1.0
 */
static void lpm_swstd_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  if (lpm_swstd_pins_init() != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_lpm_swstd_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = *out_pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_lpm_swstd_sci_channel, &sci_cfg) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  if (ra_rtc_init() != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  const ra_rtc_datetime_t seed = {
    .year    = (uint16_t)(2000U + (uint16_t)k_lpm_swstd_seed_year_lo),
    .month   = (uint8_t)k_lpm_swstd_seed_month,
    .day     = (uint8_t)k_lpm_swstd_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  if (ra_rtc_set(&seed) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  if (ra_lpm_init(&lpm_cfg) != k_ra_ok) {
    lpm_swstd_panic_halt();
  }
}

/**
 * @brief Compute @p now + 5 s wrapping over seconds/minutes/hours.
 *
 * @param[in]  now Current datetime.
 * @param[out] out Receives the +5 s alarm datetime.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``now->second < 60``, ``now->minute < 60``, ``now->hour < 24``.
 *
 * @post ``out->second/minute/hour`` reflect the wall-clock advance.
 * @post ``out->year/month/day/weekday`` mirror ``now``.
 *
 * @since 0.1.0
 */
static void lpm_swstd_add_offset(const ra_rtc_datetime_t* now, ra_rtc_datetime_t* out)
{
  *out                = *now;
  const uint16_t s    = (uint16_t)now->second + (uint16_t)k_lpm_swstd_alarm_offset_s;
  out->second         = (uint8_t)(s % (uint16_t)k_lpm_swstd_secs_per_min);
  const uint16_t carm = (uint16_t)(s / (uint16_t)k_lpm_swstd_secs_per_min);
  const uint16_t m    = (uint16_t)now->minute + carm;
  out->minute         = (uint8_t)(m % (uint16_t)k_lpm_swstd_mins_per_hour);
  const uint16_t carh = (uint16_t)(m / (uint16_t)k_lpm_swstd_mins_per_hour);
  const uint16_t h    = (uint16_t)now->hour + carh;
  out->hour           = (uint8_t)(h % (uint16_t)k_lpm_swstd_hours_per_day);
}

/**
 * @brief Arm the +5 s RTC alarm and the WUPEN0.RTCALM wake source.
 *
 * @par MC/DC:
 * Compound decision: ``ra_rtc_set_alarm != ok ||
 * ra_rtc_set_irq_enable != ok || ra_lpm_arm_wupen0_bits != ok``.
 * Three atomic conditions x N+1 = 4 vectors -- all-ok runtime path
 * + each branch error path covered in the host unit test.
 *
 * @return Error code from the first failing primitive.
 *
 * @pre RTC has been initialised and seeded.
 * @pre LPM block has been initialised with PRC1 unlocked.
 *
 * @post On success the RTC alarm is set 5 s in the future and
 *       WUPEN0.RTCALMWUPEN is asserted.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lpm_swstd_arm_wake(const ra_rtc_datetime_t* now)
{
  ra_rtc_datetime_t a = {};
  lpm_swstd_add_offset(now, &a);
  ra_err_t err = ra_rtc_set_alarm(&a);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_rtc_set_irq_enable((uint8_t)k_ra_rtc_irq_alarm);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_lpm_arm_wupen0_bits((uint32_t)k_ra_lpm_wupen0_rtcalm);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  lpm_swstd_setup_or_halt(&pclka_hz);
  (void)pclka_hz;
  ra_isr_globals_enable();

  /* Emit the boot banner *before* the standby entry so the HIL gate
   * confirms the firmware booted even when the sub-clock crystal is
   * silent (see the SOSC caveat in the file header). */
  (void)ra_sci_write_polling((uint8_t)k_lpm_swstd_sci_channel,
                             k_lpm_swstd_boot_msg,
                             (uint32_t)(sizeof(k_lpm_swstd_boot_msg) - 1U));

  while (1) {
    ra_rtc_datetime_t now = {};
    if (ra_rtc_get(&now) != k_ra_ok) {
      break;
    }
    if (lpm_swstd_arm_wake(&now) != k_ra_ok) {
      break;
    }
    if (ra_lpm_enter_sleep(k_ra_sleep_mode_software_std) != k_ra_ok) {
      break;
    }
    g_lpm_swstd_wake_count++;
    /* Clear the alarm flag so the next cycle starts from a known
     * state. Errors here are non-fatal -- the loop body will simply
     * re-arm the alarm. */
    (void)ra_rtc_clear_status((uint8_t)k_ra_rtc_irq_alarm);
  }
  lpm_swstd_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
