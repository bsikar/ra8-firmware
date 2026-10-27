/**
 * @file fw_if_rng_stub.h
 * @brief Stub chip adapter for the neutral `fw_if_rng` port: the slice-5
 * proof binding.
 * @ingroup grp_fw_rng
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `fw_if_rng.h` says portable code asks for entropy behind one narrow
 * vtable, and that the mechanism is a binding supplied by the chip. This
 * file is that sentence's proof binding: the `if_<chip>` half of epic
 * RA8FW-618's slice 5, standing in for a real chip adapter the way the proof
 * stack ra8_core + ra8_fmt + ra8_xml stands in for a real portable
 * application.
 *
 * @par What it is, and what it is not
 * It is a complete binding of the port: ops that are never null, a source
 * that always reports ready, and the same bind contract the RA8 adapter
 * answers to, so the facade's guards run against a binding that is not the
 * RA8 one. It is not entropy, and it says so in its own name: fill N hands
 * back byte i as (i + N) mod 256, so consecutive fills differ and the port's
 * fill and ready intents run end to end, but these bytes are never key
 * material and the name ::fw_rng_stub_fill is the warning label. A real port
 * (an `if_esp32c6` set, say) replaces this member by member, and nothing
 * above `libs/if` changes.
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

#include "fw_if_rng.h"
#include "ra8_err.h"

/**
 * @brief The stub profile's fill op, drop-in for ::fw_rng_iface_t.
 *
 * @details
 * Deterministic, not entropy: byte @p i of fill @p N is `(i + N) mod 256`,
 * so consecutive fills differ and a test can tell them apart. A zero-length
 * request is satisfied without touching the profile's state, matching the
 * facade's own contract.
 *
 * @param[in]  ctx  Ignored; the stub serves one source.
 * @param[out] out  Buffer to fill; may be NULL only when @p len is zero.
 * @param[in]  len  Bytes to fill.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg for a NULL @p out with a
 *         nonzero @p len.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_rng_stub_fill(void *ctx, uint8_t *out, size_t len);

/**
 * @brief The stub profile's ready op, drop-in for ::fw_rng_iface_t.
 *
 * @details
 * The stub source always serves, so this answers true every time. The
 * port's contract that ready is a report, not a reservation, holds here
 * trivially: there is nothing to contend for.
 *
 * @param[in]  ctx        Ignored; the stub serves one source.
 * @param[out] out_ready  Receives true; never left unset on success.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg for a NULL output.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_rng_stub_ready(void *ctx, bool *out_ready);

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context, because the stub profile is a singleton:
 * ::fw_rng_bind is called with a NULL `ctx` and the ops ignore it. The
 * pointer outlives any handle bound to it.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_rng_iface_t *fw_rng_stub_iface(void);

/**
 * @brief Bind a handle to the stub adapter.
 *
 * @details
 * Convenience over `fw_rng_bind(rng, fw_rng_stub_iface(), nullptr)`, so a
 * composition root does not have to name the context it is not passing.
 *
 * @param[out] rng Caller-owned handle, contents ignored on entry.
 * @return As ::fw_rng_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_rng_stub_bind(fw_rng_t *rng);

#ifdef __cplusplus
}
#endif
