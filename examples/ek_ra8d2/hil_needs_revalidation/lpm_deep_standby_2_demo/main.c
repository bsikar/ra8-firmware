/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_deep_standby_2_demo/main.c
 * @brief Deep Software Standby 2 (LPSCR.LPMD = 0x9) entry demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates Deep Software Standby variant 2 (LPSCR.LPMD = 0x9 per
 * HUM Ch 11.2.20 p 457) on the EK-RA8D2. Unlike Software Standby
 * (LPMD = 0x5), Deep Software Standby resets the CPU on wake instead
 * of resuming -- so the wake path lands in ``Reset_Handler`` and the
 * banner is re-emitted on every cycle.
 *
 * Variant 2 stops the voltage-monitor and sub-clock-detection
 * domains that variant 1 leaves running (HUM Ch 11.1 Table 11.3
 * p 431..432). Variant 3 stops the LOCO on top of that.
 *
 * Boot flow:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. Emit boot banner ``"lpm_dpsby2: boot\r\n"`` -- HIL gate.
 *   3. RTC init + seed + arm a +5 s alarm + RCR1.AIE.
 *   4. Arm DPSIER2.DRTCAIE so the alarm cancels Deep Standby.
 *   5. Arm WUPEN0.RTCALMWUPEN so the alarm reaches the wake matrix.
 *   6. ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2)``.
 *
 * The chip resets on wake -- the next boot lands back in step 1.
 *
 * @par SOSC caveat
 * Same SOSC dependency as ``lpm_software_standby_demo`` (see that
 * file's header). If the sub-clock crystal is silent the wake never
 * happens; the HIL gate is boot-banner-only so the firmware build
 * + bring-up path is still exercised.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_rtc.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_lpm_dpsby2_baud = 115200U, /**< Lpm dpsby2 baud. */
} lpm_dpsby2_const_t;

/** @brief Single-byte demo constants. */
typedef enum : uint16_t {
  k_lpm_dpsby2_alarm_offset_s = 5U,    /**< Lpm dpsby2 alarm offset s.              */
  k_lpm_dpsby2_seed_year_lo   = 26U,   /**< Lpm dpsby2 seed year lo.                */
  k_lpm_dpsby2_year_base      = 2000U, /**< Calendar epoch base for the year field. */
  k_lpm_dpsby2_seed_month     = 1U,    /**< Lpm dpsby2 seed month.                  */
  k_lpm_dpsby2_seed_day       = 1U,    /**< Lpm dpsby2 seed day.                    */
  k_lpm_dpsby2_secs_per_min   = 60U,   /**< Lpm dpsby2 secs per minimum.            */
  k_lpm_dpsby2_mins_per_hour  = 60U,   /**< Lpm dpsby2 mins per hour.               */
  k_lpm_dpsby2_hours_per_day  = 24U,   /**< Lpm dpsby2 hours per day.               */
} lpm_dpsby2_byte_t;

/** @brief Boot banner -- this is the HIL gate string. */
static const uint8_t k_lpm_dpsby2_boot_msg[] = "lpm_dpsby2: boot\r\n";

static void lpm_dpsby2_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + UART console + RTC + LPM block up.
 *
 * @pre IRQs disabled.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post RTC seeded to 2026-01-01 00:00:00.
 *
 * @since 0.1.0
 */
static void lpm_dpsby2_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_dpsby2_baud) != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  if (ra8_rtc_init() != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  const ra8_rtc_datetime_t seed = {
    .year    = (uint16_t)(k_lpm_dpsby2_year_base + (uint16_t)k_lpm_dpsby2_seed_year_lo),
    .month   = (uint8_t)k_lpm_dpsby2_seed_month,
    .day     = (uint8_t)k_lpm_dpsby2_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  if (ra8_rtc_set(&seed) != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    lpm_dpsby2_panic_halt();
  }
}

/**
 * @brief Compute @p now + 5 s wrapping across seconds/minutes/hours.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``now->second < 60``, ``now->minute < 60``, ``now->hour < 24``.
 * @post ``out`` reflects ``now`` advanced by 5 seconds.
 *
 * @since 0.1.0
 */
static void lpm_dpsby2_add_offset(const ra8_rtc_datetime_t* now, ra8_rtc_datetime_t* out)
{
  *out                = *now;
  const uint16_t s    = (uint16_t)now->second + (uint16_t)k_lpm_dpsby2_alarm_offset_s;
  out->second         = (uint8_t)(s % (uint16_t)k_lpm_dpsby2_secs_per_min);
  const uint16_t carm = (uint16_t)(s / (uint16_t)k_lpm_dpsby2_secs_per_min);
  const uint16_t m    = (uint16_t)now->minute + carm;
  out->minute         = (uint8_t)(m % (uint16_t)k_lpm_dpsby2_mins_per_hour);
  const uint16_t carh = (uint16_t)(m / (uint16_t)k_lpm_dpsby2_mins_per_hour);
  const uint16_t h    = (uint16_t)now->hour + carh;
  out->hour           = (uint8_t)(h % (uint16_t)k_lpm_dpsby2_hours_per_day);
}

/**
 * @brief Arm the +5 s RTC alarm, DPSIER2.DRTCAIE, and WUPEN0.RTCALM.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_rtc_set_alarm != ok ||
 * ra8_rtc_set_irq_enable != ok || ra8_lpm_arm_dpsier != ok ||
 * ra8_lpm_arm_wupen0_bits != ok``. Four atomic conditions x N+1 = 5
 * vectors covered in the host unit test.
 *
 * @return Error code from the first failing primitive.
 *
 * @pre RTC initialised and seeded.
 * @pre PRC1 unlocked by ra8_lpm_init.
 *
 * @post On success the RTC alarm fires in 5 s and is wired both into
 *       the deep-standby cancel matrix (DPSIER2) and the wake-up
 *       enable matrix (WUPEN0).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpm_dpsby2_arm_wake(const ra8_rtc_datetime_t* now)
{
  ra8_rtc_datetime_t a = {};
  lpm_dpsby2_add_offset(now, &a);
  ra8_err_t err = ra8_rtc_set_alarm(&a);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_rtc_set_irq_enable((uint8_t)k_ra8_rtc_irq_alarm);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_lpm_arm_dpsier(k_ra8_lpm_dpsier_idx_2, (uint8_t)k_ra8_lpm_dpsier2_drtcaie_mask);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lpm_dpsby2_setup_or_halt();
  ra8_isr_globals_enable();

  /* Emit boot banner before the deep-standby entry so the HIL gate
   * confirms the build + bring-up path even if the sub-clock crystal
   * is silent (see SOSC caveat in the file header). */
  (void)ra8_board_uart_console_write(k_lpm_dpsby2_boot_msg,
                                     (size_t)(sizeof(k_lpm_dpsby2_boot_msg) - 1U));

  while (1) {
    ra8_rtc_datetime_t now = {};
    if (ra8_rtc_get(&now) != k_ra8_ok) {
      break;
    }
    if (lpm_dpsby2_arm_wake(&now) != k_ra8_ok) {
      break;
    }
    /* On hardware this never returns -- Deep Software Standby resets
     * the CPU on wake, so control re-enters Reset_Handler. On the
     * host (RA8_OFF_TARGET) WFI is a no-op so the loop simply
     * iterates. */
    if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_2) != k_ra8_ok) {
      break;
    }
  }
  lpm_dpsby2_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
