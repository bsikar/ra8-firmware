/**
 * @file ra8_pin_validator.h
 * @brief Runtime Pin-Ownership Validator
 * @ingroup grp_core
 *
 * @details
 * Centralised bookkeeping that prevents two drivers from accidentally
 * claiming the same physical pin. Every driver that touches IOPORT or
 * PFS (Pin Function Select) must call `ra8_pin_validator_claim()`
 * before configuring the pin. The call returns `k_ra8_err_gpio_conflict`
 * if anyone else already owns the pin.
 *
 * ## How it works
 *
 * - One bit per (port, pin) pair in a static bitmap: 15 ports x 16 pins
 *   = 240 bits = 30 bytes.
 * - `ra8_pin_validator_claim(pin, owner_tag)` atomically sets the bit
 *   and records the owner tag.
 * - `ra8_pin_validator_release(pin)` clears the bit.
 * - `ra8_pin_validator_is_claimed(pin)` is a read-only query.
 *
 * ## When to call
 *
 * - At driver init time, after validating arguments, BEFORE touching
 *   any hardware registers. If the claim fails, the driver must return
 *   `k_ra8_err_gpio_conflict` without leaving the peripheral in a
 *   half-initialized state.
 * - At driver de-init time, release all owned pins so the same pin
 *   can be re-used by another peripheral later.
 *
 * ## Thread safety
 *
 * The bitmap is guarded by an IRQ-masked critical section. Safe to call
 * from any context (init, task body, ISR).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"

/**
 * @brief Claim a pin for a driver.
 *
 * @param[in] pin      Packed port/pin identifier.
 * @param[in] owner    Short owner tag (e.g. `"SCI0"`, `"LED1"`). Must
 *                     point to a static string; the validator stores
 *                     the pointer, not the contents.
 *
 * @return `k_ra8_err_t` error code.
 * @retval k_ra8_ok                    Pin successfully claimed.
 * @retval k_ra8_err_gpio_invalid_port Port out of range (0..14).
 * @retval k_ra8_err_gpio_invalid_pin  Pin out of range within the port.
 * @retval k_ra8_err_gpio_conflict     Pin already claimed by another owner.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @post On success, the pin is marked as owned by `owner`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pin_validator_claim(ra8_port_pin_t pin, const char* owner);

/**
 * @brief Release a pin previously claimed by a driver.
 *
 * @param[in] pin Packed port/pin identifier.
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_*` on invalid arguments.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pin_validator_release(ra8_port_pin_t pin);

/**
 * @brief Test whether a pin is currently claimed.
 *
 * @details Reads the bitmap maintained by claim/release. Out-of-range
 *          identifiers report `false` rather than an error.
 *
 * @param[in] pin Packed port/pin identifier.
 * @return `true` if claimed, `false` if free or if `pin` is out of range.
 * @retval true   The pin is currently owned by some driver.
 * @retval false  The pin is free OR the identifier is out of range.
 *
 * @pre `ra8_infrastructure_init()` has run.
 * @pre `pin` is the result of `RA8_PIN_PACK(port, pin)` or 0.
 * @post No internal state modified.
 * @post Result reflects the bitmap at the moment of the call.
 *
 * @note Best-effort read; result may be stale on return. Thread-safe
 *       vs other readers; not safe vs concurrent claim/release.
 *
 * @since 0.1.0
 */
bool ra8_pin_validator_is_claimed(ra8_port_pin_t pin);

/**
 * @brief Reset the validator (release every pin).
 *
 * @details Called exactly once during `ra8_infrastructure_init()`.
 *          Clears the claim bitmap and parallel owner-tag array.
 *
 * @pre Called exactly once during early infrastructure bring-up.
 * @pre All driver state referencing pin claims has been torn down.
 * @post Every pin reads as free.
 * @post Every owner tag pointer is `nullptr`.
 *
 * @note Not thread-safe; one-shot init only.
 *
 * @since 0.1.0
 */
void ra8_pin_validator_reset(void);

#ifdef __cplusplus
}
#endif
