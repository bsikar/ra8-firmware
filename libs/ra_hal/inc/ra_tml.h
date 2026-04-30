/**
 * @file ra_tml.h
 * @brief Timer Module Library (TML) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_tml` API shape (open / start / stop /
 * count-get / status / close). The TML is a 32-bit interval timer
 * block found on RA0/RA2 parts, used for low-power periodic wake-ups.
 *
 * @warning The Renesas RA8D2 silicon does not carry a TML block;
 *          ULPT, AGT and IWDT cover this functionality. This file
 *          ships a host-testable placeholder so portable code keeps
 *          compiling.
 *
 * Reference: FSP `r_tml` driver shape.
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
 * @struct ra_tml_cfg_t
 * @brief Configuration descriptor for `ra_tml_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;  /**< TML channel index.                */
  uint32_t period;   /**< 32-bit period reload value.       */
  uint8_t  one_shot; /**< Non-zero for one-shot operation.  */
} ra_tml_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the TML driver.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Driver opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `period` zero.
 * @retval k_ra_err_exists Already opened.
 *
 * @pre Single-threaded init context.
 * @post Driver in stopped state.
 * @post `ra_tml_close` will succeed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tml_open(const ra_tml_cfg_t* cfg);

/**
 * @brief Start counting.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Started.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tml_open` succeeded.
 * @post Counter is enabled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tml_start(void);

/**
 * @brief Stop counting.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Stopped.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tml_open` succeeded.
 * @post Counter is disabled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tml_stop(void);

/**
 * @brief Read the current count.
 *
 * @param[out] out Receives the 32-bit count (placeholder writes 0).
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Count copied.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_not_initialized Driver was never opened.
 *
 * @pre `ra_tml_open` succeeded.
 * @post `*out` is defined.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tml_count_get(uint32_t* out);

/**
 * @brief Snapshot the open / running flags.
 *
 * @param[out] out_open    Receives 1 if open.
 * @param[out] out_running Receives 1 if counting.
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
[[nodiscard]] ra_err_t ra_tml_status_get(uint8_t* out_open, uint8_t* out_running);

/**
 * @brief Close the TML driver.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Subsequent operations return `k_ra_err_not_initialized`.
 * @post `ra_tml_open` may be called again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tml_close(void);

#ifdef __cplusplus
}
#endif
