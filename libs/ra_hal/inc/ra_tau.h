/**
 * @file ra_tau.h
 * @brief Timer Array Unit (TAU) driver -- placeholder
 *
 * @details
 * Mirrors the FSP `r_tau` API shape (open / start / stop / period
 * read / status / close). The TAU is the RL78-derived 16-bit timer
 * array (8 channels per unit) found on RA0 / RA2 parts.
 *
 * @warning The Renesas RA8D2 silicon does not carry a TAU block;
 *          its general-purpose timing is provided by AGT, GPT and
 *          ULPT (HUM Ch 1). This file ships a host-testable
 *          placeholder so portable code keeps compiling.
 *
 * Reference: FSP `r_tau` driver shape.
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
 * @enum ra_tau_channel_t
 * @brief Logical TAU channel identifier.
 */
typedef enum : uint8_t {
  k_ra_tau_channel_0     = 0U,
  k_ra_tau_channel_1     = 1U,
  k_ra_tau_channel_2     = 2U,
  k_ra_tau_channel_3     = 3U,
  k_ra_tau_channel_4     = 4U,
  k_ra_tau_channel_5     = 5U,
  k_ra_tau_channel_6     = 6U,
  k_ra_tau_channel_7     = 7U,
  k_ra_tau_channel_count = 8U,
} ra_tau_channel_t;

/**
 * @struct ra_tau_cfg_t
 * @brief Configuration descriptor for `ra_tau_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_tau_channel_t channel;   /**< Channel index.                  */
  uint16_t         period;    /**< 16-bit period reload value.     */
  uint8_t          prescaler; /**< Prescaler exponent (0..15).  */
} ra_tau_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open a TAU channel.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Channel opened.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `channel` out of range or `period` zero.
 * @retval k_ra_err_exists Channel already opened.
 *
 * @pre Single-threaded init context.
 * @post Channel is in stopped state.
 * @post `ra_tau_close` will succeed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_open(const ra_tau_cfg_t* cfg);

/**
 * @brief Start counting on a previously-opened channel.
 *
 * @param[in] channel Channel id.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Started.
 * @retval k_ra_err_invalid_arg `channel` out of range.
 * @retval k_ra_err_not_initialized Channel was never opened.
 *
 * @pre `ra_tau_open` succeeded.
 * @post Counter is enabled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_start(ra_tau_channel_t channel);

/**
 * @brief Stop counting on a channel.
 *
 * @param[in] channel Channel id.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Stopped.
 * @retval k_ra_err_invalid_arg `channel` out of range.
 * @retval k_ra_err_not_initialized Channel was never opened.
 *
 * @pre `ra_tau_open` succeeded.
 * @post Counter is disabled.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_stop(ra_tau_channel_t channel);

/**
 * @brief Read the current count value.
 *
 * @param[in]  channel Channel id.
 * @param[out] out     Receives the 16-bit count (placeholder writes 0).
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Count copied.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_invalid_arg `channel` out of range.
 * @retval k_ra_err_not_initialized Channel was never opened.
 *
 * @pre `ra_tau_open` succeeded.
 * @post `*out` is defined.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_count_get(ra_tau_channel_t channel, uint16_t* out);

/**
 * @brief Snapshot the open / running flags for a channel.
 *
 * @param[in]  channel     Channel id.
 * @param[out] out_open    Receives 1 if open.
 * @param[out] out_running Receives 1 if counting.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Status copied.
 * @retval k_ra_err_null_ptr Either pointer NULL.
 * @retval k_ra_err_invalid_arg `channel` out of range.
 *
 * @pre Pointers reference writable memory.
 * @post `*out_open` and `*out_running` reflect channel state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_tau_status_get(ra_tau_channel_t channel, uint8_t* out_open, uint8_t* out_running);

/**
 * @brief Close a TAU channel.
 *
 * @param[in] channel Channel id.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_arg `channel` out of range.
 * @retval k_ra_err_invalid_state Channel was never opened.
 *
 * @pre Single-threaded shutdown context.
 * @post Channel returns to closed state.
 * @post Subsequent operations on this channel return `k_ra_err_not_initialized`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tau_close(ra_tau_channel_t channel);

#ifdef __cplusplus
}
#endif
