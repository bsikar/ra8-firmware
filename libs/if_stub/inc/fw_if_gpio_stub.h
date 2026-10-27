/**
 * @file fw_if_gpio_stub.h
 * @brief Stub chip adapter for the neutral `fw_if_gpio` port: the slice-5
 * proof binding.
 * @ingroup grp_fw_gpio
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `fw_if_gpio.h` says a pin is named in board numbering, and that its
 * direction and pull are what a chip-and-board binding owns between them.
 * This file is that sentence's proof binding: the `if_<chip>` half of epic
 * RA8FW-618's slice 5, standing in for a real chip adapter the way the proof
 * stack ra8_core + ra8_fmt + ra8_xml stands in for a real portable
 * application.
 *
 * @par What it is, and what it is not
 * It is a complete binding of the port: ops that are never null, a board
 * that carries four pins in dense numbering, and the same bind contract the
 * RA8 adapter answers to, so the facade's guards run against a binding that
 * is not the RA8 one. It is not a port of any chip. The profile is arbitrary:
 * an output pin's level is what a write left there, an input pin samples the
 * board's tied level, which for a stub board is low on every pin, and every
 * pin past index 3 answers ::k_ra8_err_not_found. A real port (an
 * `if_esp32c6` set, say) replaces this member by member, and nothing above
 * `libs/if` changes.
 *
 * @par World
 * The stub reads no hardware and names no vendor header, so unlike the RA8
 * adapters this one carries no {World: S} boundary: it builds for every
 * freestanding target the portability gate runs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_gpio.h"
#include "ra8_err.h"

/**
 * @brief The stub profile's configure op, drop-in for ::fw_gpio_iface_t.
 *
 * @details
 * Pure state change: no register is touched, which is what lets the profile
 * be tested on the host without a fake peripheral block. A direction of
 * none is not a direction, so it is rejected, and so is a pull outside the
 * enum; a pin the board does not carry answers ::k_ra8_err_not_found.
 *
 * @param[in] ctx  Ignored; the stub serves one port.
 * @param[in] pin  Pin in board numbering.
 * @param[in] cfg  Direction and pull.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a malformed config, or
 *         ::k_ra8_err_not_found when the board has no such pin.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_stub_configure(void *ctx, uint16_t pin, fw_gpio_pin_cfg_t cfg);

/**
 * @brief The stub profile's write op, drop-in for ::fw_gpio_iface_t.
 *
 * @details
 * Drives one output pin. An input pin answers ::k_ra8_err_invalid_state,
 * the port's own contract that a write to an input is refused.
 *
 * @param[in] ctx   Ignored; the stub serves one port.
 * @param[in] pin   Pin in board numbering.
 * @param[in] value Level to drive.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_state when the pin is not an
 *         output, or ::k_ra8_err_not_found when the board has no such pin.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_stub_write(void *ctx, uint16_t pin, bool value);

/**
 * @brief The stub profile's read op, drop-in for ::fw_gpio_iface_t.
 *
 * @details
 * Samples one pin's level: what an output drives, or the board's tied level
 * (low) for an input. The board is a stub, so the level asserts nothing
 * about real silicon.
 *
 * @param[in]  ctx       Ignored; the stub serves one port.
 * @param[in]  pin       Pin in board numbering.
 * @param[out] out_value Receives the sampled level.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output, or
 *         ::k_ra8_err_not_found when the board has no such pin.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_stub_read(void *ctx, uint16_t pin, bool *out_value);

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context, because the stub profile is a singleton:
 * ::fw_gpio_bind is called with a NULL `ctx` and the ops ignore it. The
 * pointer outlives any handle bound to it.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_gpio_iface_t *fw_gpio_stub_iface(void);

/**
 * @brief Bind a handle to the stub adapter.
 *
 * @details
 * Convenience over `fw_gpio_bind(gpio, fw_gpio_stub_iface(), nullptr)`, so a
 * composition root does not have to name the context it is not passing.
 *
 * @param[out] gpio Caller-owned handle, contents ignored on entry.
 * @return As ::fw_gpio_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_gpio_stub_bind(fw_gpio_t *gpio);

#ifdef __cplusplus
}
#endif
