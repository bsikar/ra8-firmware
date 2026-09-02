/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_software_standby_demo/src/main.c
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
 *   5. ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std)``.
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

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_rtc.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_lpm_swstd_baud = 115200U, /**< Lpm swstd baud. */
} lpm_swstd_const_t;

/** @brief Single-byte demo constants. */
typedef enum : uint16_t {
  k_lpm_swstd_alarm_offset_s = 5U,    /**< Lpm swstd alarm offset s.               */
  k_lpm_swstd_seed_year_lo   = 26U,   /**< Lpm swstd seed year lo.                 */
  k_lpm_swstd_year_base      = 2000U, /**< Calendar epoch base for the year field. */
  k_lpm_swstd_seed_month     = 1U,    /**< Lpm swstd seed month.                   */
  k_lpm_swstd_seed_day       = 1U,    /**< Lpm swstd seed day.                     */
  k_lpm_swstd_secs_per_min   = 60U,   /**< Lpm swstd secs per minimum.             */
  k_lpm_swstd_mins_per_hour  = 60U,   /**< Lpm swstd mins per hour.                */
  k_lpm_swstd_hours_per_day  = 24U,   /**< Lpm swstd hours per day.                */
} lpm_swstd_byte_t;

/** @brief Boot banner -- this is the HIL gate string. */
static const uint8_t s_lpm_swstd_boot_msg[] = "lpm_swstd: boot\r\n";

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

/**
 * @brief Park the processor after a fatal setup or standby-path failure.
 *
 * @details Executes wait-for-interrupt indefinitely so a failed software-
 * standby sequence cannot continue into application code with partial state.
 *
 * @pre The caller has determined that the demo cannot continue safely.
 * @pre No foreground recovery operation remains capable of restoring state.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop.
 * @note The terminal loop preserves the failure state for a debugger probe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_swstd_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + SCI8 + RTC + LPM block up.
 *
 * @details
 * Same shape as ``lpm_idle_demo`` but adds RTC init + alarm seed.
 * The J-Link console (SCI8 + PD02/PD03 routing + baud) is brought up
 * via the EK-RA8D2 board-support package.
 *
 * @pre IRQs disabled (Reset_Handler default).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post LPM block has LPSCR.LPMD = 0 (System Active) so the first
 *       WFI is a plain CPU sleep until ``ra8_lpm_enter_sleep`` is
 *       called.
 * @note The helper applies a fail-closed startup policy before IRQs are enabled.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_swstd_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_swstd_baud) != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  if (ra8_rtc_init() != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  const ra8_rtc_datetime_t seed = {
    .year    = (uint16_t)(k_lpm_swstd_year_base + (uint16_t)k_lpm_swstd_seed_year_lo),
    .month   = (uint8_t)k_lpm_swstd_seed_month,
    .day     = (uint8_t)k_lpm_swstd_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  if (ra8_rtc_set(&seed) != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    internal_lpm_swstd_panic_halt();
  }
}

/**
 * @brief Compute @p now + 5 s wrapping over seconds/minutes/hours.
 *
 * @details Copies the complete calendar value, then advances the time-of-day
 * fields with explicit carry propagation through seconds, minutes, and hours.
 *
 * @param[in]  now Current datetime.
 * @param[out] out Receives the +5 s alarm datetime.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``now->second < 60``, ``now->minute < 60``, ``now->hour < 24``.
 *
 * @post ``out->second/minute/hour`` reflect the wall-clock advance.
 * @post ``out->year/month/day/weekday`` mirror ``now``.
 * @note Day rollover wraps the hour to zero; date advancement is out of scope.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_swstd_add_offset(const ra8_rtc_datetime_t* now,
                                                       ra8_rtc_datetime_t*       out)
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
 * @details Builds the future alarm, enables the RTC alarm interrupt, and then
 * enables the corresponding software-standby wake bit.
 *
 * @param[in] now Current RTC calendar value used as the alarm base.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_rtc_set_alarm != ok ||
 * ra8_rtc_set_irq_enable != ok || ra8_lpm_arm_wupen0_bits != ok``.
 * Three atomic conditions x N+1 = 4 vectors -- all-ok runtime path
 * + each branch error path covered in the host unit test.
 *
 * @return Error code from the first failing primitive.
 * @retval k_ra8_ok The alarm and wake-source bit were armed.
 *
 * @pre RTC has been initialised and seeded.
 * @pre LPM block has been initialised with PRC1 unlocked.
 *
 * @post On success the RTC alarm is set 5 s in the future and
 *       WUPEN0.RTCALMWUPEN is asserted.
 * @post On failure, later alarm-routing operations are not attempted.
 * @note The alarm target uses the bounded offset helper and does not change @p now.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t
internal_lpm_swstd_arm_wake(const ra8_rtc_datetime_t* now)
{
  ra8_rtc_datetime_t a = {};
  internal_lpm_swstd_add_offset(now, &a);
  ra8_err_t err = ra8_rtc_set_alarm(&a);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_rtc_set_irq_enable((uint8_t)k_ra8_rtc_irq_alarm);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm);
}

void main(void)
{
  internal_lpm_swstd_setup_or_halt();
  ra8_isr_globals_enable();

  /* Emit the boot banner *before* the standby entry so the HIL gate
   * confirms the firmware booted even when the sub-clock crystal is
   * silent (see the SOSC caveat in the file header). */
  (void)ra8_board_uart_console_write(s_lpm_swstd_boot_msg,
                                     (size_t)(sizeof(s_lpm_swstd_boot_msg) - 1U));

  while (1) {
    ra8_rtc_datetime_t now = {};
    if (ra8_rtc_get(&now) != k_ra8_ok) {
      break;
    }
    if (internal_lpm_swstd_arm_wake(&now) != k_ra8_ok) {
      break;
    }
    if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std) != k_ra8_ok) {
      break;
    }
    g_lpm_swstd_wake_count++;
    /* Clear the alarm flag so the next cycle starts from a known
     * state. Errors here are non-fatal -- the loop body will simply
     * re-arm the alarm. */
    (void)ra8_rtc_clear_status((uint8_t)k_ra8_rtc_irq_alarm);
  }
  internal_lpm_swstd_panic_halt();
}
