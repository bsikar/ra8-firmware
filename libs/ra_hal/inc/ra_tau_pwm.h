/**
 * @file ra_tau_pwm.h
 * @brief Timer Array Unit (TAU) PWM-mode driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_tau_pwm` API shape (open / start / stop /
 * duty-set / period-set / status / close). When the TAU is paired
 * across two channels it generates an edge-aligned PWM signal --
 * one channel sets the period, the other the duty.
 *
 * @warning The Renesas RA8D2 silicon does not carry a TAU block;
 *          PWM is provided by GPT and AGT (HUM Ch 1). This file
 *          ships a host-testable placeholder so portable code keeps
 *          compiling.
 *
 * Reference: FSP `r_tau_pwm` driver shape.
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
 * @struct ra_tau_pwm_cfg_t
 * @brief Configuration descriptor for `ra_tau_pwm_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel; /**< Master TAU channel.               */
  uint16_t period;  /**< 16-bit period.                    */
  uint16_t duty;    /**< 16-bit duty (must be < period).   */
} ra_tau_pwm_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the TAU PWM driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `period` zero or `duty` >= `period`.
 * @retval k_ra_err_exists Already opened.
 *
 * @pre Single-threaded init context.
 * @post Driver in stopped state.
 * @post `ra_tau_pwm_close` will succeed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_open(const ra_tau_pwm_cfg_t* cfg);

/**
 * @brief Start PWM output.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Started.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tau_pwm_open` succeeded.
 * @post Output is toggling.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_start(void);

/**
 * @brief Stop PWM output.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Stopped.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tau_pwm_open` succeeded.
 * @post Output is in idle level.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_stop(void);

/**
 * @brief Update the duty cycle.
 *
 * @param[in] duty New duty value (must be < period).
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Duty latched.
 * @retval k_ra_err_invalid_arg `duty` exceeds period.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tau_pwm_open` succeeded.
 * @post New duty applied at next overflow.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_duty_set(uint16_t duty);

/**
 * @brief Update the period.
 *
 * @param[in] period New period (must be > 0 and > current duty).
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Period latched.
 * @retval k_ra_err_invalid_arg `period` zero or below current duty.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tau_pwm_open` succeeded.
 * @post New period applied at next overflow.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_period_set(uint16_t period);

/**
 * @brief Snapshot the open / running flags.
 *
 * @param[out] out_open    Receives 1 if open.
 * @param[out] out_running Receives 1 if PWM output is active.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either pointer NULL.
 *
 * @pre Pointers reference writable memory.
 * @post `*out_open` and `*out_running` reflect driver state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_status_get(uint8_t* out_open, uint8_t* out_running);

/**
 * @brief Close the TAU PWM driver.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_tau_pwm_open` may be called again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_pwm_close(void);

#ifdef __cplusplus
}
#endif
