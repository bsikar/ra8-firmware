/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rtc_regs.h
 * @brief Real-Time Clock (RTC) register layout for the Renesas RA8D2
 * @ingroup grp_hal_timers
 *
 * @details
 * RTC block at `0x40202000`. Layout re-derived against FSP
 * `R_RTC_Type` (BSP header `R7KA8D2KF_core0.h`, size 128 / 0x80) and
 * cross-checked against HUM Ch 26 "Realtime Clock (RTC)" pages 1219..1255.
 *
 * Two operating modes:
 *  - **Calendar mode** (BCD): year / month / day / day-of-week /
 *    hour / minute / second registers (RSECCNT..RYRCNT).
 *  - **Binary counter mode**: 32-bit up-counter aliased over the
 *    calendar registers as BCNT0..BCNT3.
 *
 * **Important offset notes vs the older spin of this header:**
 *  - There is **no** RCR3 register on RA8D2. The control registers
 *    are RCR1 (+0x22), RCR2 (+0x24), and RCR4 (+0x28). The slot at
 *    +0x26 is reserved.
 *  - RCR2 bit assignments per HUM Ch 26.2.4 are
 *    `START.0, RESET.1, ADJ30.2, RTCOE.3, AADJE.4, AADJP.5, HR24.6,
 *    CNTMD.7`.
 *
 * All time/date count and alarm registers are BCD-encoded. The RTC
 * clock source is selected in CGC (sub-clock or LOCO) before this
 * driver is brought up.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef enum : uintptr_t {
  k_ra8_rtc_base_addr = 0x40202000UL, /**< RA8 rtc base address. */
} ra8_rtc_addr_t;

/**
 * @struct r_rtc_regs_t
 * @brief RTC register window matching FSP `R_RTC_Type` byte-for-byte.
 *
 * @details
 * Offsets follow HUM Ch 26 "Realtime Clock (RTC)" and FSP
 * `R_RTC_Type` (size 0x80). Gaps between registers are modelled
 * with `_rN` reserved fields so the struct layout matches the
 * hardware byte-for-byte.
 *
 * In binary-count mode the calendar fields RSECCNT..RWKCNT alias as
 * BCNT0..BCNT3 (32-bit counter), and RSECAR..RWKAR alias as
 * BCNT0AR..BCNT3AR. The driver in this tree only exposes calendar
 * mode, so the calendar-mode names are used throughout.
 */
typedef struct {
  volatile uint8_t  R64CNT;  /**< +0x00 64 Hz Counter (read-only).    */
  volatile uint8_t  _r0;     /**< +0x01 reserved.                     */
  volatile uint8_t  RSECCNT; /**< +0x02 Second Counter (BCD) / BCNT0. */
  volatile uint8_t  _r1;     /**< +0x03 reserved.                     */
  volatile uint8_t  RMINCNT; /**< +0x04 Minute Counter (BCD) / BCNT1. */
  volatile uint8_t  _r2;     /**< +0x05 reserved.                     */
  volatile uint8_t  RHRCNT;  /**< +0x06 Hour Counter (BCD)   / BCNT2. */
  volatile uint8_t  _r3;     /**< +0x07 reserved.                     */
  volatile uint8_t  RWKCNT;  /**< +0x08 Day-of-Week Counter / BCNT3.  */
  volatile uint8_t  _r4;     /**< +0x09 reserved.                     */
  volatile uint8_t  RDAYCNT; /**< +0x0A Day Counter (BCD).            */
  volatile uint8_t  _r5;     /**< +0x0B reserved.                     */
  volatile uint8_t  RMONCNT; /**< +0x0C Month Counter (BCD).          */
  volatile uint8_t  _r6;     /**< +0x0D reserved.                     */
  volatile uint16_t RYRCNT;  /**< +0x0E Year Counter (BCD, 16-bit).   */

  volatile uint8_t  RSECAR;  /**< +0x10 Second Alarm (BCD) / BCNT0AR.  */
  volatile uint8_t  _r7;     /**< +0x11 reserved.                      */
  volatile uint8_t  RMINAR;  /**< +0x12 Minute Alarm (BCD) / BCNT1AR.  */
  volatile uint8_t  _r8;     /**< +0x13 reserved.                      */
  volatile uint8_t  RHRAR;   /**< +0x14 Hour   Alarm (BCD) / BCNT2AR.  */
  volatile uint8_t  _r9;     /**< +0x15 reserved.                      */
  volatile uint8_t  RWKAR;   /**< +0x16 Day-of-Week Alarm  / BCNT3AR.  */
  volatile uint8_t  _ra;     /**< +0x17 reserved.                      */
  volatile uint8_t  RDAYAR;  /**< +0x18 Day  Alarm (BCD)   / BCNT0AER. */
  volatile uint8_t  _rb;     /**< +0x19 reserved.                      */
  volatile uint8_t  RMONAR;  /**< +0x1A Month Alarm (BCD)  / BCNT1AER. */
  volatile uint8_t  _rc;     /**< +0x1B reserved.                      */
  volatile uint16_t RYRAR;   /**< +0x1C Year  Alarm (BCD)  / BCNT2AER. */
  volatile uint8_t  RYRAREN; /**< +0x1E Year  Alarm Enable / BCNT3AER. */
  volatile uint8_t  _rd;     /**< +0x1F reserved.                      */

  volatile uint8_t  _r10[2]; /**< +0x20..+0x21 reserved.                  */
  volatile uint8_t  RCR1;    /**< +0x22 Control 1 (IRQ enables, PES).     */
  volatile uint8_t  _r11;    /**< +0x23 reserved.                         */
  volatile uint8_t  RCR2;    /**< +0x24 Control 2 (START/RESET/HR24/...). */
  volatile uint8_t  _r12[3]; /**< +0x25..+0x27 reserved.                  */
  volatile uint8_t  RCR4;    /**< +0x28 Control 4 (count source / mode).  */
  volatile uint8_t  _r13;    /**< +0x29 reserved.                         */
  volatile uint16_t RFRH;    /**< +0x2A Frequency Register H (RFC16).     */
  volatile uint16_t RFRL;    /**< +0x2C Frequency Register L (RFC[15:0]). */
  volatile uint8_t  RADJ;    /**< +0x2E Time Error Adjustment.            */
  volatile uint8_t  _r14;    /**< +0x2F reserved.                         */
  volatile uint16_t RADJ2;   /**< +0x30 Time Error Adjustment 2 (FADJ).   */

  /* +0x32..+0x3F reserved (7 x uint16_t in FSP). */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 7 = (0x40 - 0x32) / 2 reserved halfwords. */
  volatile uint16_t _r15[7]; /**< Reserved. */

  /**
   * +0x40..+0x4F: RTCCR[3] -- Time Capture Control Register (3 ch x
   * 0x04 stride). Modelled here as a plain byte array to keep the
   * struct size correct without pulling the bit-field union in.
   */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 12 = 3 channels x 0x04 stride, raw bytes. */
  volatile uint8_t RTCCR[12];
  /* +0x4C..+0x4F reserved (5 halfwords minus RTCCR overlap = 2 hw). */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 2 reserved halfwords at +0x4C..+0x4F. */
  volatile uint16_t _r16[2]; /**< Reserved. */

  /**
   * +0x50..+0x7F: CP[3] -- Capture registers (sec, min, hour, day,
   * month). Each channel is 0x10 bytes; modelled as raw bytes.
   */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 48 = 3 channels x 0x10 capture bytes. */
  volatile uint8_t CP[48];
} r_rtc_regs_t;

/**
 * @enum ra8_rtc_layout_t
 * @brief Authoritative struct offsets used by the layout asserts below.
 *
 * @details
 * Mirrors FSP `R_RTC_Type` (BSP `R7KA8D2KF_core0.h`, total size 0x80).
 * Kept as a typed enum so the static_assert lines below do not contain
 * magic numbers (clang-tidy `readability-magic-numbers`).
 */
typedef enum : uint8_t {
  k_ra8_rtc_layout_size      = 0x80U, /**< Block size in bytes. */
  k_ra8_rtc_layout_off_rcr1  = 0x22U, /**< RCR1 offset.         */
  k_ra8_rtc_layout_off_rcr2  = 0x24U, /**< RCR2 offset.         */
  k_ra8_rtc_layout_off_rcr4  = 0x28U, /**< RCR4 offset.         */
  k_ra8_rtc_layout_off_rfrh  = 0x2AU, /**< RFRH offset.         */
  k_ra8_rtc_layout_off_radj  = 0x2EU, /**< RADJ offset.         */
  k_ra8_rtc_layout_off_rtccr = 0x40U, /**< RTCCR[0] offset.     */
  k_ra8_rtc_layout_off_cp    = 0x50U, /**< CP[0] offset.        */
} ra8_rtc_layout_t;

/* Compile-time check that the layout matches FSP R_RTC_Type. */
static_assert(sizeof(r_rtc_regs_t) == (size_t)k_ra8_rtc_layout_size,
              "r_rtc_regs_t must be 128 bytes");
static_assert(offsetof(r_rtc_regs_t, RCR1) == (size_t)k_ra8_rtc_layout_off_rcr1, "RCR1 at +0x22");
static_assert(offsetof(r_rtc_regs_t, RCR2) == (size_t)k_ra8_rtc_layout_off_rcr2, "RCR2 at +0x24");
static_assert(offsetof(r_rtc_regs_t, RCR4) == (size_t)k_ra8_rtc_layout_off_rcr4, "RCR4 at +0x28");
static_assert(offsetof(r_rtc_regs_t, RFRH) == (size_t)k_ra8_rtc_layout_off_rfrh, "RFRH at +0x2A");
static_assert(offsetof(r_rtc_regs_t, RADJ) == (size_t)k_ra8_rtc_layout_off_radj, "RADJ at +0x2E");
static_assert(offsetof(r_rtc_regs_t, RTCCR) == (size_t)k_ra8_rtc_layout_off_rtccr,
              "RTCCR at +0x40");
static_assert(offsetof(r_rtc_regs_t, CP) == (size_t)k_ra8_rtc_layout_off_cp, "CP at +0x50");

/** @brief Get pointer to the RTC block. */
static inline volatile r_rtc_regs_t* ra8_rtc(void)
{
  return (volatile r_rtc_regs_t*)k_ra8_rtc_base_addr;
}

/**
 * @enum ra8_rcr1_bit_t
 * @brief Bit positions in RCR1 (HUM Ch 26.2.3 p 1229).
 */
typedef enum : uint8_t {
  k_ra8_rcr1_bit_aie    = 0U, /**< Alarm interrupt enable.            */
  k_ra8_rcr1_bit_cie    = 1U, /**< Carry  interrupt enable.           */
  k_ra8_rcr1_bit_pie    = 2U, /**< Periodic interrupt enable.         */
  k_ra8_rcr1_bit_rtcos  = 3U, /**< RTCOUT output select.              */
  k_ra8_rcr1_bit_pes_lo = 4U, /**< PES[3:0] periodic select (b4..b7). */
} ra8_rcr1_bit_t;

/**
 * @enum ra8_rcr2_bit_t
 * @brief Bit positions in RCR2 (HUM Ch 26.2.4 p 1230).
 *
 * @details
 * Re-derived against FSP `R_RTC_Type::RCR2_b`:
 *  START.0, RESET.1, ADJ30.2, RTCOE.3, AADJE.4, AADJP.5, HR24.6, CNTMD.7.
 * The previous spin of this enum had `aadje` at bit 5; that was wrong --
 * bit 4 is AADJE, bit 5 is AADJP.
 */
typedef enum : uint8_t {
  k_ra8_rcr2_bit_start = 0U, /**< 0=stop, 1=run.                        */
  k_ra8_rcr2_bit_reset = 1U, /**< Software reset (write 1, auto-clear). */
  k_ra8_rcr2_bit_adj30 = 2U, /**< 30-second adjustment.                 */
  k_ra8_rcr2_bit_rtcoe = 3U, /**< RTCOUT output enable.                 */
  k_ra8_rcr2_bit_aadje = 4U, /**< Auto-adjust enable.                   */
  k_ra8_rcr2_bit_aadjp = 5U, /**< Auto-adjust period select.            */
  k_ra8_rcr2_bit_hr24  = 6U, /**< 1=24h, 0=12h.                         */
  k_ra8_rcr2_bit_cntmd = 7U, /**< 0=calendar, 1=binary.                 */
} ra8_rcr2_bit_t;

/**
 * @enum ra8_rcr4_bit_t
 * @brief Bit positions in RCR4 (HUM Ch 26.2.6 p 1232 area).
 */
typedef enum : uint8_t {
  k_ra8_rcr4_bit_rcksel = 0U, /**< Count source select.       */
  k_ra8_rcr4_bit_ropsel = 7U, /**< RTC operation mode select. */
} ra8_rcr4_bit_t;

#ifdef __cplusplus
}
#endif
