/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/ra8_esp_hosted_c6link.h
 * @brief Binds the `ra8_c6link` transport seam to this port's OS vtable.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * `libs/ra8_c6link` is deliberately ignorant of hardware: it reaches the
 * co-processor through three function pointers and knows nothing about SCI
 * channels, ICU edges or ThreadX. This header is where those three pointers
 * are filled in with the port's own implementations, which is why it lives
 * here rather than inside the library -- the library must stay compilable on a
 * host with no port, and it is.
 *
 * All three rows are thin trampolines onto the 72-entry `hosted_osi_funcs_t`
 * vtable behind ``g_h``. That is deliberate: the transactions the facade clocks
 * then go through exactly the same code the vendored driver would use, so a
 * bench result from the facade is a bench result about the port.
 *
 * @par Order of operations
 * ::ra8_esp_hosted_port_init must have returned ``k_ra8_ok`` before the seam is
 * bound, because ``g_h.funcs`` is unpopulated until it does and the first
 * transaction would dereference it.
 *
 * @since 0.1.0
 */

#pragma once

#include "ra8_c6link_transport.h"
#include "ra8_err.h"

/**
 * @brief Fill a `ra8_c6link` transport seam with this port's implementations.
 *
 * @details
 * Sets the transfer, handshake and delay rows to trampolines onto ``g_h`` and
 * leaves the context null -- the port is a singleton, so there is nothing
 * per-instance to carry. The seam is validated by ::ra8_c6link_open, which
 * rejects any row this call failed to fill.
 *
 * @param[out] out Seam to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The seam is bound and may be handed to ::ra8_c6link_open.
 * @retval k_ra8_err_null_ptr @p out was null.
 * @retval k_ra8_err_not_initialized The port is not up, so ``g_h.funcs`` would
 *         be unpopulated at the first transaction.
 *
 * @pre ::ra8_esp_hosted_port_init has returned ``k_ra8_ok``.
 * @pre No link is currently clocking transactions through this port.
 * @post On success all three rows of @p out are non-null.
 * @post On failure @p out is cleared rather than left half-filled.
 *
 * @note Not thread-safe against a concurrent port teardown; bind during
 *       bring-up, before any pump starts.
 * @warning The bound seam outlives nothing: tearing the port down while a link
 *          holds the seam leaves that link calling into a closed bus.
 *
 * @par Example:
 * @code
 * ra8_c6link_cfg_t cfg = {};
 * if (ra8_esp_hosted_c6link_bind(&cfg.transport) == k_ra8_ok) {
 *   cfg.arena       = arena;
 *   cfg.arena_bytes = (uint32_t)sizeof arena;
 *   (void)ra8_c6link_open(&link, &cfg);
 * }
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @see ra8_c6link_open
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: two preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_esp_hosted_c6link_bind(ra8_c6link_transport_t* out);
