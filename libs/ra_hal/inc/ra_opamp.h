/**
 * @file ra_opamp.h
 * @brief Op-amp (AMP) peripheral driver
 *
 * @details
 * Mirrors FSP `r_opamp_api_t` lifecycle (open / close / configure)
 * for the on-chip AMP block. Each call configures a single channel:
 * its gain selector, input-pin selector, and output-pin selector.
 *
 * Reference: FSP `r_opamp` driver shape (BSP `r_opamp.h`). The AMP
 * register layout is modelled in `ra8d2_opamp_regs.h`; HUM placement
 * follows the analog-cluster peripheral list.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_opamp_regs.h"
#include "ra_err.h"

/**
 * @enum ra_opamp_gain_t
 * @brief Per-channel gain selector codes (4 bits, FSP-aligned).
 */
typedef enum : uint8_t {
  k_ra_opamp_gain_unity = 0U, /**< 1x unity-gain follower.    */
  k_ra_opamp_gain_2x    = 1U, /**< 2x non-inverting.          */
  k_ra_opamp_gain_4x    = 2U, /**< 4x non-inverting.          */
  k_ra_opamp_gain_8x    = 3U, /**< 8x non-inverting.          */
  k_ra_opamp_gain_max   = 4U, /**< Range guard ceiling.       */
} ra_opamp_gain_t;

/**
 * @enum ra_opamp_input_pin_t
 * @brief Input-pin selector codes (4 bits, MCU-group specific).
 */
typedef enum : uint8_t {
  k_ra_opamp_input_pin_default = 0U, /**< Default pin per channel. */
  k_ra_opamp_input_pin_alt1    = 1U, /**< Alternate pin 1.         */
  k_ra_opamp_input_pin_alt2    = 2U, /**< Alternate pin 2.         */
  k_ra_opamp_input_pin_max     = 3U, /**< Range guard ceiling.     */
} ra_opamp_input_pin_t;

/**
 * @enum ra_opamp_output_pin_t
 * @brief Output-pin selector codes.
 *
 * @details Per FSP `r_opamp`, the output pin is fixed per channel
 * (AMP0_OUT..AMP3_OUT). Callers carry the selector for symmetry with
 * the input-pin field; the driver rejects bogus values.
 */
typedef enum : uint8_t {
  k_ra_opamp_output_pin_default = 0U, /**< Channel's primary OUT pin. */
  k_ra_opamp_output_pin_alt1    = 1U, /**< Alt OUT routing.           */
  k_ra_opamp_output_pin_max     = 2U, /**< Range guard.               */
} ra_opamp_output_pin_t;

/**
 * @struct ra_opamp_cfg_t
 * @brief Per-channel configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_opamp_channel_t    channel;    /**< Channel id (0..3).           */
  ra_opamp_gain_t       gain;       /**< Gain selector.               */
  ra_opamp_input_pin_t  input_pin;  /**< Input-pin select code.       */
  ra_opamp_output_pin_t output_pin; /**< Output-pin select code.      */
} ra_opamp_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the op-amp driver and reset every channel.
 *
 * @details
 * Clears `AMPC`, `AMPGAIN`, and `AMPINSEL`. Powers down all four
 * channels until the application configures one explicitly. Idempotent
 * across `_close` -> `_open` cycles.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver ready.
 *
 * @pre Single-threaded init context.
 *
 * @post All four channels are disabled.
 * @post `AMPC = 0`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_opamp_open(void);

/**
 * @brief Close the op-amp driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post All four channels disabled, AMPC = 0.
 * @post Subsequent `_configure` calls return `k_ra_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_opamp_close(void);

/**
 * @brief Configure a single op-amp channel.
 *
 * @details
 * Programmes the gain nibble in `AMPGAIN`, the input-pin nibble in
 * `AMPINSEL`, and asserts the channel-enable bit in `AMPC`. The
 * output pin selector is validated and stored in `AMPTRS`'s low
 * nibble.
 *
 * @param[in] cfg Channel configuration.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Channel armed.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `cfg->channel`, `gain`, `input_pin`, or
 *         `output_pin` out of range.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre `ra_opamp_open` succeeded.
 *
 * @post `AMPC` bit `cfg->channel` is set.
 * @post `AMPGAIN` and `AMPINSEL` nibbles for the channel reflect `cfg`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_opamp_configure(const ra_opamp_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
