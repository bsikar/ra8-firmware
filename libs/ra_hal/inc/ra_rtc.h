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
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_rtc_get`` and ``ra_rtc_set`` in
 * ``libs/ra_hal/src/ra_rtc.c``.
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
} ra_rtc_datetime_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Start the RTC in 24-hour calendar mode.
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_init_failed` otherwise.
 *
 * @note Sub-clock oscillator must already be running (configured
 *       via CGC). This driver does not enable it.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_init(void);

/**
 * @brief Write the calendar registers.
 * @param[in] dt Date/time to install (BCD-encoded internally).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_set(const ra_rtc_datetime_t* dt);

/**
 * @brief Read the calendar registers.
 * @param[out] out Receive buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_get(ra_rtc_datetime_t* out);

/**
 * @enum ra_rtc_irq_mask_t
 * @brief RCR1 IRQ enable bits.
 */
typedef enum : uint8_t {
  k_ra_rtc_irq_none     = 0x00U,
  k_ra_rtc_irq_alarm    = 0x01U, /**< RCR1.AIE. */
  k_ra_rtc_irq_carry    = 0x02U, /**< RCR1.CIE. */
  k_ra_rtc_irq_periodic = 0x04U, /**< RCR1.PIE. */
} ra_rtc_irq_mask_t;

/**
 * @typedef ra_rtc_event_fn_t
 * @brief RTC event callback.
 */
typedef void (*ra_rtc_event_fn_t)(void* ctx, uint8_t status_mask);

/**
 * @brief Tear down the RTC (stop counter + disable IRQs).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_deinit(void);

/**
 * @brief Enable one or more RTC IRQ sources via RCR1.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_set_irq_enable(uint8_t mask);

/**
 * @brief Read RCR1 IRQ enable bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_get_status(uint8_t* out_mask);

/**
 * @brief Clear RCR1 IRQ enable bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_clear_status(uint8_t mask);

/**
 * @brief Attach a callback for the RTC alarm / periodic event.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_attach_handler(ra_rtc_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an RTC event -- snapshot RCR1 + fire callback.
 * @since 0.1.0
 */
void ra_rtc_dispatch(void);

/**
 * @brief Stop the RTC counter for low-power mode.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_enter_stop(void);

/**
 * @brief Restart the RTC counter from stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rtc_exit_stop(void);

#ifdef __cplusplus
}
#endif
