/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_deep_standby_3_demo/main.c
 * @brief Deep Software Standby 3 (LPSCR.LPMD = 0xA) entry demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates Deep Software Standby variant 3 (LPSCR.LPMD = 0xA per
 * HUM Ch 11.2.20 p 457) on the EK-RA8D2. Unlike Software Standby
 * (LPMD = 0x5), Deep Software Standby resets the CPU on wake instead
 * of resuming -- so the wake path lands in ``Reset_Handler`` and the
 * banner is re-emitted on every cycle.
 *
 * Variant 3 is the deepest standby state on RA8D2 -- on top of the
 * variant 2 cuts it also stops the LOCO oscillator (HUM Ch 11.1
 * Table 11.3 p 431..432). Only the always-on wake-up detectors
 * driven by the sub-clock survive.
 *
 * Boot flow:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. Emit boot banner ``"lpm_dpsby3: boot\r\n"`` -- HIL gate.
 *   3. RTC init + seed + arm a +5 s alarm + RCR1.AIE.
 *   4. Arm DPSIER2.DRTCAIE so the alarm cancels Deep Standby.
 *   5. Arm WUPEN0.RTCALMWUPEN so the alarm reaches the wake matrix.
 *   6. ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_3)``.
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
  k_lpm_dpsby3_baud = 115200U, /**< Lpm dpsby3 baud. */
} lpm_dpsby3_const_t;

/** @brief Single-byte demo constants. */
typedef enum : uint16_t {
  k_lpm_dpsby3_alarm_offset_s = 5U,    /**< Lpm dpsby3 alarm offset s.              */
  k_lpm_dpsby3_seed_year_lo   = 26U,   /**< Lpm dpsby3 seed year lo.                */
  k_lpm_dpsby3_year_base      = 2000U, /**< Calendar epoch base for the year field. */
  k_lpm_dpsby3_seed_month     = 1U,    /**< Lpm dpsby3 seed month.                  */
  k_lpm_dpsby3_seed_day       = 1U,    /**< Lpm dpsby3 seed day.                    */
  k_lpm_dpsby3_secs_per_min   = 60U,   /**< Lpm dpsby3 secs per minimum.            */
  k_lpm_dpsby3_mins_per_hour  = 60U,   /**< Lpm dpsby3 mins per hour.               */
  k_lpm_dpsby3_hours_per_day  = 24U,   /**< Lpm dpsby3 hours per day.               */
} lpm_dpsby3_byte_t;

/** @brief Boot banner -- this is the HIL gate string. */
static const uint8_t s_lpm_dpsby3_boot_msg[] = "lpm_dpsby3: boot\r\n";

/**
 * @brief Park the processor after a fatal setup or standby-path failure.
 *
 * @details Executes wait-for-interrupt indefinitely so a failed deep-standby
 * sequence cannot continue into application code with partial state.
 *
 * @pre The caller has determined that the demo cannot continue safely.
 * @pre No foreground recovery operation remains capable of restoring state.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop.
 * @note The terminal loop preserves the failure state for a debugger probe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_dpsby3_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + SCI8 + RTC + LPM block up.
 *
 * @details Initializes the clock and timing path, console, RTC seed, and LPM
 * configuration in dependency order, halting at the first required failure.
 *
 * @pre IRQs disabled.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post RTC seeded to 2026-01-01 00:00:00.
 * @note The helper applies a fail-closed startup policy before IRQs are enabled.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_dpsby3_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_dpsby3_baud) != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  if (ra8_rtc_init() != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  const ra8_rtc_datetime_t seed = {
    .year    = (uint16_t)(k_lpm_dpsby3_year_base + (uint16_t)k_lpm_dpsby3_seed_year_lo),
    .month   = (uint8_t)k_lpm_dpsby3_seed_month,
    .day     = (uint8_t)k_lpm_dpsby3_seed_day,
    .weekday = 0U,
    .hour    = 0U,
    .minute  = 0U,
    .second  = 0U,
  };
  if (ra8_rtc_set(&seed) != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    internal_lpm_dpsby3_panic_halt();
  }
}

/**
 * @brief Compute @p now + 5 s wrapping across seconds/minutes/hours.
 *
 * @details Copies the complete calendar value, then advances only the time-of-day
 * fields with explicit carry propagation through seconds, minutes, and hours.
 *
 * @param[in] now Current valid RTC calendar value.
 * @param[out] out Receives the copied calendar and advanced time-of-day fields.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``now->second < 60``, ``now->minute < 60``, ``now->hour < 24``.
 * @post ``out`` reflects ``now`` advanced by 5 seconds.
 * @post Calendar fields other than hour, minute, and second mirror @p now.
 * @note Day rollover wraps the hour to zero; date advancement is out of scope.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lpm_dpsby3_add_offset(const ra8_rtc_datetime_t* now,
                                                        ra8_rtc_datetime_t*       out)
{
  *out                = *now;
  const uint16_t s    = (uint16_t)now->second + (uint16_t)k_lpm_dpsby3_alarm_offset_s;
  out->second         = (uint8_t)(s % (uint16_t)k_lpm_dpsby3_secs_per_min);
  const uint16_t carm = (uint16_t)(s / (uint16_t)k_lpm_dpsby3_secs_per_min);
  const uint16_t m    = (uint16_t)now->minute + carm;
  out->minute         = (uint8_t)(m % (uint16_t)k_lpm_dpsby3_mins_per_hour);
  const uint16_t carh = (uint16_t)(m / (uint16_t)k_lpm_dpsby3_mins_per_hour);
  const uint16_t h    = (uint16_t)now->hour + carh;
  out->hour           = (uint8_t)(h % (uint16_t)k_lpm_dpsby3_hours_per_day);
}

/**
 * @brief Arm the +5 s RTC alarm, DPSIER2.DRTCAIE, and WUPEN0.RTCALM.
 *
 * @details Builds the future alarm, enables its RTC interrupt, connects it to
 * the deep-standby cancel matrix, and finally enables the wake source.
 *
 * @param[in] now Current RTC calendar value used as the alarm base.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_rtc_set_alarm != ok ||
 * ra8_rtc_set_irq_enable != ok || ra8_lpm_arm_dpsier != ok ||
 * ra8_lpm_arm_wupen0_bits != ok``. Four atomic conditions x N+1 = 5
 * vectors covered in the host unit test.
 *
 * @return Error code from the first failing primitive.
 * @retval k_ra8_ok The alarm and both wake-routing matrices were armed.
 *
 * @pre RTC initialised and seeded.
 * @pre PRC1 unlocked by ra8_lpm_init.
 *
 * @post On success the RTC alarm fires in 5 s and is wired both into
 *       the deep-standby cancel matrix (DPSIER2) and the wake-up
 *       enable matrix (WUPEN0).
 * @post On failure, later wake-routing operations are not attempted.
 * @note The alarm target uses the bounded offset helper and does not change @p now.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t
internal_lpm_dpsby3_arm_wake(const ra8_rtc_datetime_t* now)
{
  ra8_rtc_datetime_t a = {};
  internal_lpm_dpsby3_add_offset(now, &a);
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

void main(void)
{
  internal_lpm_dpsby3_setup_or_halt();
  ra8_isr_globals_enable();

  /* Emit boot banner before the deep-standby entry so the HIL gate
   * confirms the build + bring-up path even if the sub-clock crystal
   * is silent (see SOSC caveat in the file header). */
  (void)ra8_board_uart_console_write(s_lpm_dpsby3_boot_msg,
                                     (size_t)(sizeof(s_lpm_dpsby3_boot_msg) - 1U));

  while (1) {
    ra8_rtc_datetime_t now = {};
    if (ra8_rtc_get(&now) != k_ra8_ok) {
      break;
    }
    if (internal_lpm_dpsby3_arm_wake(&now) != k_ra8_ok) {
      break;
    }
    /* On hardware this never returns -- Deep Software Standby resets
     * the CPU on wake, so control re-enters Reset_Handler. On the
     * host (RA8_OFF_TARGET) WFI is a no-op so the loop simply
     * iterates. */
    if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_deep_standby_3) != k_ra8_ok) {
      break;
    }
  }
  internal_lpm_dpsby3_panic_halt();
}
