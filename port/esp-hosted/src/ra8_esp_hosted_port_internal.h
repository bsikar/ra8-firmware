/**
 * @file port/esp-hosted/src/ra8_esp_hosted_port_internal.h
 * @brief Library-private surface of the esp-hosted port bring-up.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * ``ra8_esp_hosted_port.c`` sequences the slices -- RTOS pools, the
 * vtable, the SPI channel, the side-band pins -- and unwinds them in the
 * opposite order. The two decisions that are worth testing on their own,
 * configuration validation and pin-map validation, are promoted out of
 * `static` here so a host test can drive every rejection path without a
 * board.
 *
 * Nothing outside ``port/esp-hosted/`` and ``tests/`` may include this.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"

/**
 * @brief Check a port configuration against its documented contract.
 *
 * @details
 * Every field has a reason it cannot be zero: a zero peripheral clock
 * makes the bit-rate divider undefined, a zero bit rate asks the SCI for
 * a rate it cannot produce, a zero poll period arms a ThreadX timer with
 * a zero tick count -- which ThreadX rejects -- and an out-of-range SCI
 * channel indexes past the peripheral. Rejecting them here, before any
 * hardware is touched, is what lets initialisation fail without having
 * claimed a pin.
 *
 * @param[in] cfg Configuration to validate. May be null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every field is within contract.
 * @retval k_ra8_err_null_ptr ``cfg`` was null.
 * @retval k_ra8_err_invalid_arg A field was zero or out of range.
 *
 * @pre None; the function is total over its input.
 * @pre The caller has not yet touched any hardware.
 * @post No state is modified.
 * @post The answer depends only on ``cfg``.
 *
 * @note Reentrant; a pure predicate.
 *
 * @par MC/DC:
 * Promoted from `static` so each field's rejection can be driven
 * independently -- the decision is a four-condition disjunction, so it
 * takes five vectors to prove each field influences the outcome on its
 * own. Production callers reach it through
 * ``ra8_esp_hosted_port_init``.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_port_cfg_t cfg = { .pclk_hz = 0U };
 * TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_cfg_check(&cfg));
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] ra8_err_t priv_ra8_esp_hosted_port_cfg_check(const ra8_esp_hosted_port_cfg_t* cfg);

/**
 * @brief Check the compiled-in pin map for self-consistency.
 *
 * @details
 * The pin map in ``ra8_esp_hosted_pins.h`` is the one file the owner
 * edits when the harness changes, so it is exactly where a
 * copy-and-paste mistake would land. Two properties are worth proving
 * before anything claims a pin: the chip select and the two side-band
 * nets must be three distinct pins, and each must decode to a legal port
 * and pin index. A duplicated assignment would otherwise show up as an
 * unexplained pin-claim conflict deep in bring-up.
 *
 * The reset pin is deliberately exempt from the distinctness check: it is
 * legitimately ``k_ra8_pin_none``, the harness having no reset wire.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The map is self-consistent.
 * @retval k_ra8_err_invalid_arg Two signals share a pin, or a pin decodes
 *         to an out-of-range port or pin index.
 *
 * @pre The pin map header describes this board.
 * @pre No pin has been claimed yet.
 * @post No state is modified.
 * @post The answer depends only on compile-time constants.
 *
 * @note Reentrant; a pure predicate over compile-time values.
 *
 * @par MC/DC:
 * Promoted from `static` so a test can assert the property holds for the
 * shipped map. The decision's conditions are compile-time constants, so
 * the shipped configuration exercises one vector; the remaining vectors
 * are driven through ``priv_ra8_esp_hosted_port_pins_check_values``.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_pins_check());
 * @endcode
 *
 * @see priv_ra8_esp_hosted_port_pins_check_values
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] ra8_err_t priv_ra8_esp_hosted_port_pins_check(void);

/**
 * @brief Check three candidate pin assignments for self-consistency.
 *
 * @details
 * The body of ::priv_ra8_esp_hosted_port_pins_check, taking its inputs as
 * parameters so every rejection path can be reached. The shipped map is
 * a set of compile-time constants and therefore exercises exactly one
 * path; without this seam the other paths would be unreachable from a
 * test, which is the same as being unverified.
 *
 * @param[in] chip_select Packed chip-select pin code.
 * @param[in] handshake Packed HANDSHAKE pin code.
 * @param[in] data_ready Packed DATA_READY pin code.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The three are distinct and each decodes legally.
 * @retval k_ra8_err_invalid_arg Two share a pin, or one decodes to an
 *         out-of-range port or pin index.
 *
 * @pre The values are packed ``RA8_PIN(port, pin)`` codes.
 * @pre The caller treats ``k_ra8_pin_none`` as an illegal assignment for
 *      all three of these signals, since the link cannot work without any
 *      of them.
 * @post No state is modified.
 * @post The answer depends only on the three arguments.
 *
 * @note Reentrant; a pure predicate.
 *
 * @par MC/DC:
 * Promoted from `static` so the three distinctness comparisons and the
 * range checks can each be varied independently. Five vectors cover the
 * distinctness disjunction; the range checks add one vector per signal.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
 *                priv_ra8_esp_hosted_port_pins_check_values(0x0804U, 0x0804U, 0x0006U));
 * @endcode
 *
 * @see priv_ra8_esp_hosted_port_pins_check
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] ra8_err_t priv_ra8_esp_hosted_port_pins_check_values(uint16_t chip_select,
                                                                   uint16_t handshake,
                                                                   uint16_t data_ready);

/**
 * @brief Keep the first error of a teardown sequence, discarding later ones.
 *
 * @details
 * The unwind path runs every release step regardless of what the previous
 * one reported, because a teardown that stops at the first error strands
 * exactly the resource it was meant to release. It therefore needs the
 * "first error wins" fold in one place: given what has been reported so
 * far and what the next step reported, the earlier failure survives and a
 * clean step never overwrites a failure.
 *
 * @param[in] first What the sequence has reported so far.
 * @param[in] next What the step just run reported.
 *
 * @return ra8_err_t The error the sequence should carry onwards.
 * @retval k_ra8_ok Both arguments were ``k_ra8_ok``.
 * @retval first ``first`` was already an error, whatever ``next`` was.
 * @retval next ``first`` was ``k_ra8_ok`` and ``next`` was not.
 *
 * @pre Both arguments are ``ra8_err_t`` values from release steps.
 * @pre The step producing ``next`` has already run; this fold never
 *      decides whether a step runs.
 * @post No state is modified.
 * @post The answer depends only on the two arguments.
 *
 * @note Reentrant; a pure fold.
 *
 * @par MC/DC:
 * Promoted from `static` because the fold is the only compound decision on
 * the unwind path and the unwind path itself needs a brought-up port -- a
 * board -- to reach. As a pure two-argument function it takes the minimal
 * three vectors: (ok, ok), (error, ok) and (ok, error).
 *
 * @par Example:
 * @code
 * ra8_err_t first = priv_ra8_esp_hosted_port_first_error(k_ra8_ok, close_err);
 * first           = priv_ra8_esp_hosted_port_first_error(first, deinit_err);
 * @endcode
 *
 * @see ra8_esp_hosted_port_deinit
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] ra8_err_t priv_ra8_esp_hosted_port_first_error(ra8_err_t first, ra8_err_t next);
