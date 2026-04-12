/**
 * @file ra_rtc.h
 * @brief Real-Time Clock driver (BCD calendar mode)
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
 * @struct ra_rtc_datetime_t
 * @brief Calendar date + time decoded from BCD.
 */
typedef struct {
  uint16_t year;    /**< 2000..2099.        */
  uint8_t  month;   /**< 1..12.             */
  uint8_t  day;     /**< 1..31.             */
  uint8_t  weekday; /**< 0 (Sun) .. 6 (Sat).*/
  uint8_t  hour;    /**< 0..23.             */
  uint8_t  minute;  /**< 0..59.             */
  uint8_t  second;  /**< 0..59.             */
} ra_rtc_datetime_t;

/**
 * @brief Start the RTC in 24-hour calendar mode.
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_init_failed` otherwise.
 *
 * @note Sub-clock oscillator must already be running (configured
 *       via CGC). This driver does not enable it.
 */
[[nodiscard]] ra_err_t ra_rtc_init(void);

/**
 * @brief Write the calendar registers.
 * @param[in] dt Date/time to install (BCD-encoded internally).
 */
[[nodiscard]] ra_err_t ra_rtc_set(const ra_rtc_datetime_t* dt);

/**
 * @brief Read the calendar registers.
 * @param[out] out Receive buffer.
 */
[[nodiscard]] ra_err_t ra_rtc_get(ra_rtc_datetime_t* out);

#ifdef __cplusplus
}
#endif
