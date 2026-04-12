/**
 * @file ra_port_utils.h
 * @brief High-level GPIO helpers on top of the PORT + PFS register layer
 *
 * @details
 * Thin convenience API that takes `ra_port_pin_t` values and wraps the
 * PORT and PFS register writes with the correct PWPR unlock / lock
 * sequence. Drivers should prefer these helpers over hand-coding the
 * PFS dance at every callsite.
 *
 * ## Pattern
 *
 * @code{.c}
 *   ra_err_t err = ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low);
 *   RA_ERROR_CHECK(err);
 *   ra_gpio_write(k_ra_pin_led1, k_ra_level_high);
 * @endcode
 *
 * Every helper claims ownership of the pin through
 * `ra_pin_validator_claim()` under the tag `"GPIO"`. If a different
 * driver later tries to claim the same pin, it gets
 * `k_ra_err_gpio_conflict`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"
#include "ra_port_constants.h"

/**
 * @brief Configure a pin as a digital output and drive it to an
 *        initial level.
 *
 * @param[in] pin         Packed port/pin identifier.
 * @param[in] init_level  Initial output level (`k_ra_level_low` or
 *                        `k_ra_level_high`).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                    Pin configured and driven.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin  Pin out of range.
 * @retval k_ra_err_gpio_conflict     Pin already owned.
 *
 * @pre `ra_infrastructure_init()` has run (pin validator ready).
 * @pre The IOPORT module clock is on (IOPORT is one of the "always on"
 *      blocks, so this is satisfied automatically after reset).
 * @post On success, the pin is in GPIO-output mode driving `init_level`.
 * @post On success, the pin is owned by this driver (`"GPIO"` tag).
 *
 * @note Not thread-safe: reads / modifies / writes the PFS register
 *       and touches PWPR. Protect with IRQ masking or run during
 *       single-threaded init.
 */
[[nodiscard]] ra_err_t ra_gpio_output_init(ra_port_pin_t pin, ra_level_t init_level);

/**
 * @brief Configure a pin as a digital input.
 *
 * @param[in] pin  Packed port/pin identifier.
 * @param[in] pull `k_ra_pull_none` / `k_ra_pull_up`.
 *
 * @return `ra_err_t` error code (same set as `ra_gpio_output_init()`).
 */
[[nodiscard]] ra_err_t ra_gpio_input_init(ra_port_pin_t pin, ra_pin_pull_t pull);

/**
 * @brief Drive a previously-configured output to the given level.
 *
 * @param[in] pin   Packed port/pin identifier.
 * @param[in] level Target level.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                    Level driven.
 * @retval k_ra_err_gpio_invalid_port Port out of range.
 * @retval k_ra_err_gpio_invalid_pin  Pin out of range.
 *
 * @note Uses the atomic POSR/PORR register so the write is race-free
 *       against concurrent updates to other pins of the same port.
 */
[[nodiscard]] ra_err_t ra_gpio_write(ra_port_pin_t pin, ra_level_t level);

/**
 * @brief Toggle a previously-configured output.
 *
 * @param[in] pin Packed port/pin identifier.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_gpio_toggle(ra_port_pin_t pin);

/**
 * @brief Read a previously-configured input.
 *
 * @param[in]  pin       Packed port/pin identifier.
 * @param[out] out_level Pointer to receive current level.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_gpio_read(ra_port_pin_t pin, ra_level_t* out_level);

#ifdef __cplusplus
}
#endif
