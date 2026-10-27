/**
 * @file fw_if_clock_stub.h
 * @brief Stub chip adapter for the neutral `fw_if_clock` port: the slice-5
 * proof binding.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `fw_if_clock.h` says a request names a neutral module kind and an instance,
 * and that "a binding supplied by the chip and the board resolves it against a
 * clock profile they own between them". This file is that sentence's proof
 * binding: the `if_<chip>` half of epic RA8FW-618's slice 5, standing in for a
 * real chip adapter the way the proof stack ra8_core + ra8_fmt + ra8_xml
 * stands in for a real portable application.
 *
 * @par What it is, and what it is not
 * It is a complete binding of the port: ops that are never null, a profile
 * that resolves three modules, and the same bind contract the RA8 adapter
 * answers to, so the facade's guards run against a binding that is not the
 * RA8 one. It is not a port of any chip. The profile is arbitrary (the core,
 * UART 0 and SPI 0 at fixed rates); gating a carried module succeeds and does
 * nothing; every other module resolves as ::k_ra8_err_not_found, which
 * ::fw_clock_has_module reports as "this binding cannot resolve that
 * instance". A real port (an `if_esp32c6` set, say) replaces this member by
 * member, and nothing above `libs/if` changes.
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

#include "fw_if_clock.h"
#include "ra8_err.h"

/**
 * @struct fw_clock_stub_row_t
 * @brief What the stub profile knows about one module instance.
 *
 * @details
 * A row the profile does not carry comes back zeroed with `has_rate` false
 * and ::k_ra8_err_not_found, never as a rate of zero that looks like success.
 */
typedef struct fw_clock_stub_row_s {
  uint32_t rate_hz;  /**< Stub rate in Hz; zero unless has_rate. */
  bool     has_rate; /**< Whether the profile carries this instance. */
} fw_clock_stub_row_t;

/**
 * @brief Resolve a neutral module against the stub profile.
 *
 * @details
 * Pure table lookup: no register is touched, which is what lets the mapping
 * be tested on the host without a fake peripheral block.
 *
 * @param[in]  module  Neutral module, with @p index as the board instance.
 * @param[out] out_row On success, what the profile carries for it.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output, or
 *         ::k_ra8_err_not_found when the profile carries no such instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_clock_stub_resolve(fw_clock_module_t module, fw_clock_stub_row_t *out_row);

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context, because the stub profile is a singleton:
 * ::fw_clock_bind is called with a NULL `ctx` and the ops ignore it. The
 * pointer outlives any handle bound to it.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_clock_iface_t *fw_clock_stub_iface(void);

/**
 * @brief Bind a handle to the stub adapter.
 *
 * @details
 * Convenience over `fw_clock_bind(clk, fw_clock_stub_iface(), nullptr)`, so a
 * composition root does not have to name the context it is not passing.
 *
 * @param[out] clk Caller-owned handle, contents ignored on entry.
 * @return As ::fw_clock_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_stub_bind(fw_clock_t *clk);

#ifdef __cplusplus
}
#endif
