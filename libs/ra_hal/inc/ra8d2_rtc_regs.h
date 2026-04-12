/**
 * @file ra8d2_rtc_regs.h
 * @brief Real-Time Clock (RTC) register layout for the Renesas RA8D2
 *
 * @details
 * RTC block at `0x40202000`. Supports two operating modes:
 *
 *  - **Calendar mode** (BCD): year / month / day / day-of-week /
 *    hour / minute / second / sub-second registers.
 *  - **Binary counter mode**: a straight 32-bit up-counter clocked
 *    from the sub-clock or LOCO.
 *
 * This header covers the most commonly used calendar-mode subset.
 * Alarm registers, capture registers, and the RWP write-protect
 * key are included; time-capture (`RTCTCR0..2`) and RPS are omitted
 * until a driver needs them.
 *
 * All time/date registers are BCD-encoded. The RTC clock source is
 * selected in SYSC (not here) and requires the sub-clock oscillator
 * or LOCO to be running first.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_rtc_base_addr = 0x40202000UL,
} ra_rtc_addr_t;

/**
 * @struct r_rtc_regs_t
 * @brief RTC register window (calendar-mode subset).
 *
 * @details
 * Offsets follow RA8D2 HUM section "Realtime Clock (RTC)". Gaps
 * between registers are modelled with `_rN` reserved byte fields so
 * the struct layout matches the hardware byte-for-byte.
 */
typedef struct {
  volatile uint8_t  R64CNT; /**< +0x00 64 Hz Counter.                   */
  volatile uint8_t  _r0;
  volatile uint8_t  RSECCNT; /**< +0x02 Second Counter (BCD).            */
  volatile uint8_t  _r1;
  volatile uint8_t  RMINCNT; /**< +0x04 Minute Counter (BCD).            */
  volatile uint8_t  _r2;
  volatile uint8_t  RHRCNT; /**< +0x06 Hour Counter (BCD).              */
  volatile uint8_t  _r3;
  volatile uint8_t  RWKCNT; /**< +0x08 Day-of-Week Counter.             */
  volatile uint8_t  _r4;
  volatile uint8_t  RDAYCNT; /**< +0x0A Day Counter (BCD).               */
  volatile uint8_t  _r5;
  volatile uint8_t  RMONCNT; /**< +0x0C Month Counter (BCD).             */
  volatile uint8_t  _r6;
  volatile uint16_t RYRCNT; /**< +0x0E Year Counter (BCD).              */
  volatile uint8_t  RSECAR; /**< +0x10 Second Alarm (BCD).              */
  volatile uint8_t  _r7;
  volatile uint8_t  RMINAR; /**< +0x12 Minute Alarm (BCD).              */
  volatile uint8_t  _r8;
  volatile uint8_t  RHRAR; /**< +0x14 Hour   Alarm (BCD).              */
  volatile uint8_t  _r9;
  volatile uint8_t  RWKAR; /**< +0x16 Day-of-Week Alarm.               */
  volatile uint8_t  _ra;
  volatile uint8_t  RDAYAR; /**< +0x18 Day  Alarm (BCD).                */
  volatile uint8_t  _rb;
  volatile uint8_t  RMONAR; /**< +0x1A Month Alarm (BCD).               */
  volatile uint8_t  _rc;
  volatile uint16_t RYRAR;   /**< +0x1C Year Alarm (BCD).                */
  volatile uint8_t  RYRAREN; /**< +0x1E Year Alarm Enable.               */
  volatile uint8_t  _rd;
  volatile uint8_t  RCR1; /**< +0x20 Control 1 (IRQ enables).         */
  volatile uint8_t  _re;
  volatile uint8_t  RCR2; /**< +0x22 Control 2 (START/RESET/24-hour). */
  volatile uint8_t  _rf;
  volatile uint8_t  RCR3; /**< +0x24 Control 3 (sub-clock + RTC EN).  */
  volatile uint8_t  _r10;
  volatile uint8_t  RCR4; /**< +0x26 Control 4 (source select).       */
} r_rtc_regs_t;

/** @brief Get pointer to the RTC block. */
static inline volatile r_rtc_regs_t* ra_rtc(void)
{
  return (volatile r_rtc_regs_t*)k_ra_rtc_base_addr;
}

/**
 * @enum ra_rcr1_bit_t
 * @brief Bit positions in RCR1.
 */
typedef enum : uint8_t {
  k_ra_rcr1_bit_aie = 0U, /**< Alarm interrupt enable.        */
  k_ra_rcr1_bit_cie = 1U, /**< Carry  interrupt enable.       */
  k_ra_rcr1_bit_pie = 2U, /**< Periodic interrupt enable.     */
} ra_rcr1_bit_t;

/**
 * @enum ra_rcr2_bit_t
 * @brief Bit positions in RCR2.
 */
typedef enum : uint8_t {
  k_ra_rcr2_bit_start = 0U, /**< 0=stop, 1=run.                      */
  k_ra_rcr2_bit_reset = 1U, /**< Software reset (write 1, auto-clear). */
  k_ra_rcr2_bit_adj30 = 2U, /**< 30-second adjustment.                */
  k_ra_rcr2_bit_aadje = 5U, /**< Auto-adjust enable.                  */
  k_ra_rcr2_bit_hr24  = 6U, /**< 1=24h, 0=12h.                        */
  k_ra_rcr2_bit_cntmd = 7U, /**< 1=count mode (binary), 0=calendar.   */
} ra_rcr2_bit_t;

#ifdef __cplusplus
}
#endif
