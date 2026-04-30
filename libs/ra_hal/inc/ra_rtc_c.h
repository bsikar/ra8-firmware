/**
 * @file ra_rtc_c.h
 * @brief RTC_C calendar-mode sub-driver
 *
 * @details
 * Thin "calendar" sub-driver layered on the same RA8D2 RTC block as
 * `ra_rtc.c`. Mirrors the FSP `r_rtc_c` interface shape (open / close /
 * calendar set / calendar get / alarm set), which on FSP-supported MCU
 * groups exposes the calendar-mode features as a separate
 * `r_rtc_c_api_t` instance distinct from the binary-counter
 * `r_rtc_api_t`.
 *
 * On RA8D2 the underlying hardware is a single RTC block (HUM Ch 26),
 * but the FSP-shaped `_open`/`_close` lifecycle and explicit alarm
 * descriptor make this driver convenient to layer behind glue that
 * speaks the FSP RTC_C API. The block is reset and re-armed on every
 * `_open`, so callers should treat ra_rtc_c as the sole owner of the
 * RTC peripheral while it is open.
 *
 * Reference: HUM Ch 26 "Realtime Clock (RTC)" pages 1219..1255.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @struct ra_rtc_c_calendar_t
 * @brief Decoded calendar date + time (BCD-converted internally).
 *
 * @details Field ranges match HUM Ch 26.2.5..26.2.11. Years are
 * stored in the absolute 4-digit form to keep the API symmetric with
 * `ra_rtc_datetime_t`; on the wire the year register is BCD over a
 * base of 2000.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t year;    /**< 2000..2099.        */
  uint8_t  month;   /**< 1..12.             */
  uint8_t  day;     /**< 1..31.             */
  uint8_t  weekday; /**< 0 (Sun) .. 6 (Sat).*/
  uint8_t  hour;    /**< 0..23.             */
  uint8_t  minute;  /**< 0..59.             */
  uint8_t  second;  /**< 0..59.             */
} ra_rtc_c_calendar_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_rtc_c_alarm_t
 * @brief Alarm match descriptor (HUM Ch 26.2.13..26.2.20).
 *
 * @details Each `*_match` flag selects whether the corresponding
 * alarm-register byte is compared against the count register on each
 * tick. Disabled fields are written zero so they never match.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_rtc_c_calendar_t time;          /**< Alarm date / time.        */
  bool                second_match;  /**< Match RSECAR.             */
  bool                minute_match;  /**< Match RMINAR.             */
  bool                hour_match;    /**< Match RHRAR.              */
  bool                day_match;     /**< Match RDAYAR.             */
  bool                month_match;   /**< Match RMONAR.             */
  bool                weekday_match; /**< Match RWKAR.              */
  bool                year_match;    /**< Match RYRAR (RYRAREN=1).  */
} ra_rtc_c_alarm_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the RTC_C calendar-mode sub-driver.
 *
 * @details
 * Resets the RTC control registers to a known state (CNTMD = 0,
 * HR24 = 1, START = 1) and clears every alarm-enable byte. Idempotent
 * across `_close` -> `_open` cycles.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok RTC ready in calendar mode.
 *
 * @pre Sub-clock oscillator already running.
 * @pre Single-threaded init context.
 *
 * @post `ra_rtc_c_get_calendar` returns a valid timestamp.
 * @post Alarm-enable bytes cleared (no spurious alarm IRQ).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_c_open(void);

/**
 * @brief Close the RTC_C sub-driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Counter halted (RCR2.START = 0).
 * @post Subsequent calendar / alarm calls return `k_ra_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_c_close(void);

/**
 * @brief Write the calendar registers.
 *
 * @param[in] dt Date/time to install (BCD-encoded internally).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Installed.
 * @retval k_ra_err_null_ptr `dt` was NULL.
 * @retval k_ra_err_invalid_arg `dt->year` < 2000.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre `ra_rtc_c_open` succeeded.
 *
 * @post Counter advances from the new timestamp on the next sub-clock tick.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_c_set_calendar(const ra_rtc_c_calendar_t* dt);

/**
 * @brief Read the calendar registers.
 *
 * @param[out] out Receive buffer.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Snapshot copied.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre `ra_rtc_c_open` succeeded.
 *
 * @post `*out` holds a coherent snapshot of the count registers.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_c_get_calendar(ra_rtc_c_calendar_t* out);

/**
 * @brief Programme the alarm match registers.
 *
 * @details
 * Writes RSECAR..RYRAR with the BCD-encoded alarm time, sets each
 * alarm-byte high-bit (`AENB`) for the match flags requested in
 * `alarm`, and arms `RYRAREN` if `year_match` is true. Enables AIE in
 * RCR1 so the RTC alarm IRQ fires on the next match.
 *
 * @param[in] alarm Alarm descriptor.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Alarm armed.
 * @retval k_ra_err_null_ptr `alarm` was NULL.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre `ra_rtc_c_open` succeeded.
 *
 * @post RCR1.AIE = 1; alarm registers reflect `alarm->time`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_c_set_alarm(const ra_rtc_c_alarm_t* alarm);

#ifdef __cplusplus
}
#endif
