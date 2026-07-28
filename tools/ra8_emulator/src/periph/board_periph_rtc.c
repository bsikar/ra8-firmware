/**
 * @file board_periph_rtc.c
 * @brief Realtime Clock (RTC) calendar peripheral-block model for ra8_emulator
 *
 * @details
 * Models the RA8D2 RTC (ra8_rtc_regs.h, ra8_rtc.c) in 24-hour BCD calendar
 * mode with a real, advancing time base plus the alarm and periodic interrupt
 * sources, so an example such as @c rtc_alarm (waits for the alarm flag) and
 * @c rtc_periodic_demo (waits for the alarm-or-periodic flag) run to their log
 * line and the alarm / periodic ELC events actually fire.
 *
 * Control registers are a faithful shadow so the driver's bring-up poll loops
 * complete deterministically: @c ra8_rtc_init clears RCR2 then waits for CNTMD
 * to read 0, sets RCR2.HR24 and waits for it to read back, then sets RCR2.START
 * and waits for it; it also clears RCR1 and waits for 0. The model honours every
 * such readback. The driver's status surface is unusual -- @c ra8_rtc_get_status
 * returns @c RCR1 & (AIE|CIE|PIE), i.e. the enable bits double as the polled
 * "fired" flags -- so reflecting RCR1 exactly is what lets the demos' poll loops
 * make progress.
 *
 * On top of that faithful shadow the model adds genuine RTC behaviour:
 *
 *  - **Time advance.** While RCR2.START is set, a binary mirror of the
 *    second/minute/hour/day/month/year advances one RTC second every few ticks
 *    (a fixed tick:second ratio keeps a multi-second alarm reachable inside the
 *    headless chunk budget). Calendar-count reads (RSECCNT..RYRCNT) and R64CNT
 *    are produced from that mirror in BCD, so @c ra8_rtc_get reports a clock that
 *    moves -- the time register advances, exactly as the verification asks.
 *  - **Alarm match.** A write to RSECAR / RMINAR / RHRAR with the ENB bit set
 *    arms the alarm; when the running time reaches the armed hh:mm:ss the model
 *    latches the alarm and, if RCR1.AIE is set, raises the RTC alarm ELC event
 *    through the core's ICU -> NVIC path so an interrupt-driven alarm also fires.
 *  - **Periodic.** While RCR1.PIE is set the model raises the RTC periodic ELC
 *    event once per modelled second, the coarsest of the RCR1.PES intervals, so
 *    a periodic-interrupt firmware path is exercised.
 *
 * The RTC is a single register window (@c 0x40202000, 0x80 bytes); the block
 * carries a @c tick that advances the calendar and raises the two events.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief BCD packing + calendar constants for the RTC model. */
typedef enum : uint32_t {
  k_bcd_base    = 10U,   /**< Decimal radix for BCD packing. */
  k_bcd_shift   = 4U,    /**< Tens nibble shift.             */
  k_nibble_mask = 0x0FU, /**< Low BCD nibble.                */
  k_year_mod    = 100U,  /**< Two-digit year wrap.           */
  k_year_base   = 2000U, /**< Calendar epoch base year.      */
} rtc_lit_t;

/**
 * @brief Per-tick order slot for the RTC block.
 *
 * @details
 * Placed above the existing ::board_periph_block_order_t slots (gpio 10,
 * timer 20, sci 30, i2c 40) and above the DMAC block, so the RTC ticks and
 * reports after them. Spacing is deliberate; only the relative order matters and
 * the RTC shares no state with another block, so the absolute value is free. A
 * local enum keeps the value typed without editing the shared header the
 * conflict-free rule forbids touching.
 */
typedef enum : uint32_t {
  k_rtc_block_order = 60U, /**< RTC tick / reset / report order slot. */
} rtc_order_t;

/** @brief RTC register window geometry (ra8_rtc_regs.h). */
typedef enum : uint64_t {
  k_rtc_base = 0x40202000UL, /**< RTC block base.      */
  k_rtc_span = 0x80UL,       /**< FSP R_RTC_Type size. */
} rtc_geom_t;

/** @brief RTC register byte offsets (subset the driver touches). */
typedef enum : uint64_t {
  k_rtc_off_r64cnt  = 0x00UL, /**< 64 Hz counter (read-only).        */
  k_rtc_off_rseccnt = 0x02UL, /**< Second counter (BCD).             */
  k_rtc_off_rmincnt = 0x04UL, /**< Minute counter (BCD).             */
  k_rtc_off_rhrcnt  = 0x06UL, /**< Hour counter (BCD).               */
  k_rtc_off_rwkcnt  = 0x08UL, /**< Day-of-week counter.              */
  k_rtc_off_rdaycnt = 0x0AUL, /**< Day counter (BCD).                */
  k_rtc_off_rmoncnt = 0x0CUL, /**< Month counter (BCD).              */
  k_rtc_off_ryrcnt  = 0x0EUL, /**< Year counter (BCD, 16-bit).       */
  k_rtc_off_rsecar  = 0x10UL, /**< Second alarm (BCD + ENB).         */
  k_rtc_off_rminar  = 0x12UL, /**< Minute alarm (BCD + ENB).         */
  k_rtc_off_rhrar   = 0x14UL, /**< Hour alarm (BCD + ENB).           */
  k_rtc_off_rwkar   = 0x16UL, /**< Day-of-week alarm.                */
  k_rtc_off_rdayar  = 0x18UL, /**< Day alarm.                        */
  k_rtc_off_rmonar  = 0x1AUL, /**< Month alarm.                      */
  k_rtc_off_ryrar   = 0x1CUL, /**< Year alarm.                       */
  k_rtc_off_ryraren = 0x1EUL, /**< Year alarm enable.                */
  k_rtc_off_rcr1    = 0x22UL, /**< Control 1 (AIE/CIE/PIE, PES).     */
  k_rtc_off_rcr2    = 0x24UL, /**< Control 2 (START/RESET/HR24/...). */
  k_rtc_off_rcr4    = 0x28UL, /**< Control 4 (count source / mode).  */
} rtc_off_t;

/** @brief RCR1 / RCR2 bit masks (ra8_rtc_regs.h, ra8_rtc.h). */
typedef enum : uint8_t {
  k_rtc_rcr1_aie   = 0x01U, /**< RCR1.AIE alarm interrupt enable.    */
  k_rtc_rcr1_pie   = 0x04U, /**< RCR1.PIE periodic interrupt enable. */
  k_rtc_rcr2_start = 0x01U, /**< RCR2.START 0=stop, 1=run.           */
  k_rtc_alarm_enb  = 0x80U, /**< RxxAR bit7 alarm-field enable.      */
  k_rtc_alarm_val  = 0x7FU, /**< RxxAR lower BCD match value.        */
} rtc_bit_t;

/** @brief Calendar moduli + the modelled time:tick ratio. */
typedef enum : uint32_t {
  k_rtc_secs_per_min  = 60U, /**< Rtc secs per minimum.                 */
  k_rtc_mins_per_hour = 60U, /**< Rtc mins per hour.                    */
  k_rtc_hours_per_day = 24U, /**< Rtc hours per day.                    */
  k_rtc_days_per_mon  = 28U, /**< Conservative roll-over (model only).  */
  k_rtc_mons_per_year = 12U, /**< Rtc mons per year.                    */
  k_rtc_ticks_per_sec = 8U,  /**< Emulation chunks per modelled second. */
} rtc_calendar_t;

/** @brief 64 Hz counter range (R64CNT is 6 bits). */
typedef enum : uint8_t {
  k_rtc_r64_mask = 0x3FU, /**< R64CNT counts 0..63. */
} rtc_r64_t;

/**
 * @brief RTC alarm / periodic ELC events the model emits (FSP ra8d2 bsp_elc).
 *
 * @details
 * The RA8D2 ELC signal table (HUM Ch 19) places the RTC alarm interrupt
 * (RTC_ALM) at 0x0BB and the RTC periodic interrupt (RTC_PRD) at 0x0BC, matching
 * FSP @c bsp_elc.h. A firmware that routes either through @c ra8_isr_register
 * writes the same number into an IELSR slot, so the ICU model links the raised
 * event to that slot.
 */
typedef enum : uint16_t {
  k_rtc_event_alarm    = 0x0BBU, /**< RTC_ALM alarm-match event. */
  k_rtc_event_periodic = 0x0BCU, /**< RTC_PRD periodic event.    */
} rtc_event_t;

/** @brief The modelled RTC: a control-register shadow + a binary time mirror. */
typedef struct {
  uint8_t  reg[k_rtc_span]; /**< Faithful register shadow (BCD on reads).  */
  uint8_t  sec;             /**< Binary second mirror.                     */
  uint8_t  min;             /**< Binary minute mirror.                     */
  uint8_t  hour;            /**< Binary hour mirror.                       */
  uint8_t  day;             /**< Binary day mirror.                        */
  uint8_t  mon;             /**< Binary month mirror.                      */
  uint16_t year;            /**< Binary year mirror (2000-based BCD low).  */
  uint8_t  r64;             /**< 64 Hz sub-second counter.                 */
  uint32_t tick_frac;       /**< Ticks accumulated toward the next second. */
  uint32_t alarms;          /**< Alarm events raised (report).             */
  uint32_t periodics;       /**< Periodic events raised (report).          */
} rtc_state_t;

static rtc_state_t s_rtc;

/* =============================================================================
 * BCD helpers + the binary <-> BCD calendar reflection.
 * =============================================================================
 */

/** @brief Binary 0..99 -> packed BCD. */
static uint8_t rtc_bin_to_bcd(uint8_t bin)
{
  return (uint8_t)(((uint8_t)(bin / (uint8_t)k_bcd_base) << (uint8_t)k_bcd_shift) |
                   (uint8_t)(bin % (uint8_t)k_bcd_base));
}

/** @brief Packed BCD -> binary 0..99. */
static uint8_t rtc_bcd_to_bin(uint8_t bcd)
{
  return (uint8_t)(((uint8_t)((bcd >> (uint8_t)k_bcd_shift) & (uint8_t)k_nibble_mask) *
                    (uint8_t)k_bcd_base) +
                   (uint8_t)(bcd & (uint8_t)k_nibble_mask));
}

/** @brief Refresh the BCD calendar shadow registers from the binary mirror. */
static void rtc_publish_calendar(void)
{
  s_rtc.reg[(uint64_t)k_rtc_off_r64cnt]  = (uint8_t)(s_rtc.r64 & (uint8_t)k_rtc_r64_mask);
  s_rtc.reg[(uint64_t)k_rtc_off_rseccnt] = rtc_bin_to_bcd(s_rtc.sec);
  s_rtc.reg[(uint64_t)k_rtc_off_rmincnt] = rtc_bin_to_bcd(s_rtc.min);
  s_rtc.reg[(uint64_t)k_rtc_off_rhrcnt]  = rtc_bin_to_bcd(s_rtc.hour);
  s_rtc.reg[(uint64_t)k_rtc_off_rdaycnt] = rtc_bin_to_bcd(s_rtc.day);
  s_rtc.reg[(uint64_t)k_rtc_off_rmoncnt] = rtc_bin_to_bcd(s_rtc.mon);
  s_rtc.reg[(uint64_t)k_rtc_off_ryrcnt] =
    rtc_bin_to_bcd((uint8_t)(s_rtc.year % (uint32_t)k_year_mod));
}

/** @brief Load the binary time mirror from a freshly written BCD calendar. */
static void rtc_latch_calendar(void)
{
  s_rtc.sec  = rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_rseccnt]);
  s_rtc.min  = rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_rmincnt]);
  s_rtc.hour = rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_rhrcnt]);
  s_rtc.day  = rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_rdaycnt]);
  s_rtc.mon  = rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_rmoncnt]);
  s_rtc.year = (uint16_t)rtc_bcd_to_bin(s_rtc.reg[(uint64_t)k_rtc_off_ryrcnt]);
}

/* =============================================================================
 * MMIO read / write.
 * =============================================================================
 */

/** @brief Read @p size bytes little-endian from the RTC shadow at @p off. */
static uint64_t rtc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_rtc_base;
  uint64_t       v   = 0U;
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b < (uint64_t)k_rtc_span) {
      v |= (uint64_t)s_rtc.reg[b] << (8U * i);
    }
  }
  return v;
}

/** @brief True iff offset @p off is a calendar-count register (BCD value). */
static bool rtc_is_calendar_off(uint64_t off)
{
  return (off == (uint64_t)k_rtc_off_rseccnt) || (off == (uint64_t)k_rtc_off_rmincnt) ||
         (off == (uint64_t)k_rtc_off_rhrcnt) || (off == (uint64_t)k_rtc_off_rdaycnt) ||
         (off == (uint64_t)k_rtc_off_rmoncnt) || (off == (uint64_t)k_rtc_off_ryrcnt);
}

/** @brief Write @p size bytes little-endian into the RTC shadow at @p off. */
static void rtc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t off         = addr - (uint64_t)k_rtc_base;
  bool           touched_cal = false;
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b >= (uint64_t)k_rtc_span) {
      continue;
    }
    s_rtc.reg[b] = (uint8_t)(value >> (8U * i));
    if (rtc_is_calendar_off(b)) {
      touched_cal = true;
    }
  }
  /* A calendar-count write (ra8_rtc_set, performed with START cleared) reseeds
   * the running time mirror so the clock continues from the new value. */
  if (touched_cal) {
    rtc_latch_calendar();
  }
}

/* =============================================================================
 * Per-tick time advance + alarm / periodic event raising.
 * =============================================================================
 */

/** @brief Advance the binary time mirror by one modelled second. */
static void rtc_advance_one_second(void)
{
  s_rtc.sec++;
  if (s_rtc.sec < (uint8_t)k_rtc_secs_per_min) {
    return;
  }
  s_rtc.sec = 0U;
  s_rtc.min++;
  if (s_rtc.min < (uint8_t)k_rtc_mins_per_hour) {
    return;
  }
  s_rtc.min = 0U;
  s_rtc.hour++;
  if (s_rtc.hour < (uint8_t)k_rtc_hours_per_day) {
    return;
  }
  s_rtc.hour = 0U;
  s_rtc.day++;
  if (s_rtc.day <= (uint8_t)k_rtc_days_per_mon) {
    return;
  }
  s_rtc.day = 1U;
  s_rtc.mon++;
  if (s_rtc.mon <= (uint8_t)k_rtc_mons_per_year) {
    return;
  }
  s_rtc.mon = 1U;
  s_rtc.year++;
}

/** @brief True iff the running time matches the armed (ENB) alarm fields. */
static bool rtc_alarm_matches(void)
{
  const uint8_t sar = s_rtc.reg[(uint64_t)k_rtc_off_rsecar];
  const uint8_t mar = s_rtc.reg[(uint64_t)k_rtc_off_rminar];
  const uint8_t har = s_rtc.reg[(uint64_t)k_rtc_off_rhrar];
  /* Alarm is meaningful only if at least one field has its ENB bit set. */
  if (((sar | mar | har) & (uint8_t)k_rtc_alarm_enb) == 0U) {
    return false;
  }
  const bool sec_ok = ((sar & (uint8_t)k_rtc_alarm_enb) == 0U) ||
                      (rtc_bcd_to_bin((uint8_t)(sar & (uint8_t)k_rtc_alarm_val)) == s_rtc.sec);
  const bool min_ok = ((mar & (uint8_t)k_rtc_alarm_enb) == 0U) ||
                      (rtc_bcd_to_bin((uint8_t)(mar & (uint8_t)k_rtc_alarm_val)) == s_rtc.min);
  const bool hr_ok  = ((har & (uint8_t)k_rtc_alarm_enb) == 0U) ||
                      (rtc_bcd_to_bin((uint8_t)(har & (uint8_t)k_rtc_alarm_val)) == s_rtc.hour);
  return sec_ok && min_ok && hr_ok;
}

/** @brief Raise alarm / periodic ELC events for one elapsed modelled second. */
static void rtc_raise_events(uc_engine* uc)
{
  const uint8_t rcr1 = s_rtc.reg[(uint64_t)k_rtc_off_rcr1];
  if (rtc_alarm_matches()) {
    s_rtc.alarms++;
    if ((rcr1 & (uint8_t)k_rtc_rcr1_aie) != 0U) {
      board_periph_icu_raise_event(uc, (uint16_t)k_rtc_event_alarm);
    }
    if (board_periph_trace()) {
      (void)fprintf(stderr,
                    "  RTC           : alarm match %02u:%02u:%02u\n",
                    s_rtc.hour,
                    s_rtc.min,
                    s_rtc.sec);
    }
  }
  if ((rcr1 & (uint8_t)k_rtc_rcr1_pie) != 0U) {
    s_rtc.periodics++;
    board_periph_icu_raise_event(uc, (uint16_t)k_rtc_event_periodic);
  }
}

/** @brief Advance the RTC each chunk while running; raise its events. */
static void rtc_tick(uc_engine* uc)
{
  if ((s_rtc.reg[(uint64_t)k_rtc_off_rcr2] & (uint8_t)k_rtc_rcr2_start) == 0U) {
    return; /* stopped: time holds */
  }
  s_rtc.r64 = (uint8_t)(s_rtc.r64 + 1U);
  s_rtc.tick_frac++;
  if (s_rtc.tick_frac < (uint32_t)k_rtc_ticks_per_sec) {
    rtc_publish_calendar();
    return;
  }
  s_rtc.tick_frac = 0U;
  rtc_advance_one_second();
  rtc_publish_calendar();
  rtc_raise_events(uc);
}

/* =============================================================================
 * Reset / report.
 * =============================================================================
 */

/** @brief Clear the RTC register shadow and the time mirror. */
static void rtc_reset(void)
{
  s_rtc = (rtc_state_t){};
}

/** @brief Print the RTC's final time and its alarm / periodic event totals. */
static void rtc_report(void)
{
  if ((s_rtc.alarms == 0U) && (s_rtc.periodics == 0U)) {
    return;
  }
  (void)fprintf(stderr,
                "  RTC           : %04u-%02u-%02u %02u:%02u:%02u alarms=%u periodics=%u\n",
                (unsigned)((uint32_t)k_year_base + s_rtc.year),
                s_rtc.mon,
                s_rtc.day,
                s_rtc.hour,
                s_rtc.min,
                s_rtc.sec,
                s_rtc.alarms,
                s_rtc.periodics);
}

/** @brief RTC register window + the calendar tick / reset / report. */
static const board_periph_block_t k_rtc_block = {
  .base   = (uint64_t)k_rtc_base,
  .span   = (uint64_t)k_rtc_span,
  .order  = (uint32_t)k_rtc_block_order,
  .read   = rtc_read,
  .write  = rtc_write,
  .tick   = rtc_tick,
  .reset  = rtc_reset,
  .report = rtc_report,
  .name   = "RTC",
};

/** @brief Self-register the RTC window before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_rtc_register(void)
{
  board_periph_register_block(&k_rtc_block);
}
