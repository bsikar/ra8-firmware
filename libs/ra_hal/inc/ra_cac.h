/**
 * @file ra_cac.h
 * @brief Clock Accuracy Check driver
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
 * @brief Programme CAC with upper + lower bounds on measured cycles.
 *
 * @param[in] upper Upper limit count.
 * @param[in] lower Lower limit count.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_init(uint16_t upper, uint16_t lower);

/**
 * @brief Kick off a CAC measurement and wait for completion.
 *
 * @param[out] out_count Counter value captured on completion.
 * @return `k_ra_ok` if the measurement completed inside the
 *         window, `k_ra_err_hw_timeout` if CAC did not finish.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_measure(uint16_t* out_count);

/**
 * @enum ra_cac_status_mask_t
 * @brief CASTR status bits.
 */
typedef enum : uint8_t {
  k_ra_cac_status_none  = 0x00U,
  k_ra_cac_status_mendf = 0x02U, /**< Measurement end flag.      */
  k_ra_cac_status_ovff  = 0x04U, /**< Counter overflow.           */
  k_ra_cac_status_ferrf = 0x08U, /**< Frequency error.            */
} ra_cac_status_mask_t;

/**
 * @typedef ra_cac_event_fn_t
 * @brief CAC event callback.
 */
typedef void (*ra_cac_event_fn_t)(void* ctx, uint8_t status_mask);

/**
 * @brief Tear down the CAC (disable + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_deinit(void);

/**
 * @brief Read the CASTR flag bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_get_status(uint8_t* out_mask);

/**
 * @brief Clear the CASTR flag bits via CAICR.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_clear_status(uint8_t mask);

/**
 * @brief Attach a callback for CAC events.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_attach_handler(ra_cac_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch CAC events -- snapshot + fire callback.
 * @since 0.1.0
 */
void ra_cac_dispatch(void);

/**
 * @brief Put CAC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cac_exit_stop(void);

#ifdef __cplusplus
}
#endif
