/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_i2c_bus_i3c_compat.h
 * @brief I3C I2C-compatibility backend binder for the ra8_io I2C-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_i2c_bus_t to the I3C block running in its
 * legacy I2C-compatibility controller mode via thin trampolines to
 * `ra8_i3c_write` / `ra8_i3c_read` / `ra8_i3c_transfer` (`ra8_i3c.h`,
 * unmodified). The channel must already be initialised through
 * `ra8_i3c_init` with ::k_ra8_i3c_mode_i2c -- the facade owns transfers
 * only, never bring-up.
 *
 * The facade's `send_stop` flag is adapted to the I3C driver's inverted
 * `restart` flag inside the trampolines, so both backends present the
 * identical `ra8_io_i2c_bus_*` semantics.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_i2c_bus.h"

/**
 * @brief Bind `bus` to I3C channel `channel` in I2C-compatibility mode.
 *
 * @details
 * Installs the I3C-compat trampoline vtable and records the channel in
 * the handle context. No hardware is touched: initialise the channel with
 * `ra8_i3c_init(channel, ...)` using ::k_ra8_i3c_mode_i2c (before or after
 * binding) and route the SCL/SDA pins yourself.
 *
 * @param[in,out] bus     Caller-owned handle to bind (non-NULL).
 * @param[in]     channel I3C channel index (only 0 on RA8D2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              `bus` now dispatches to `ra8_i3c_*`.
 * @retval k_ra8_err_null_ptr    `bus` was NULL.
 * @retval k_ra8_err_invalid_arg `channel` is out of range.
 *
 * @pre `bus` points to writable storage.
 * @pre `channel` < 1 (the RA8D2 has a single I3C instance).
 * @post On success `bus->iface` is non-NULL and every `ra8_io_i2c_bus_*`
 *       call forwards to I3C channel `channel` in I2C-compat mode.
 * @post On failure `bus` is left unmodified.
 *
 * @note Thread-safe (writes only the caller's handle).
 *
 * @see ra8_io_i2c_bus_bind_riic  The classic RIIC twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_i2c_bus_bind_i3c_compat(ra8_io_i2c_bus_t* bus, uint8_t channel);

#ifdef __cplusplus
}
#endif
