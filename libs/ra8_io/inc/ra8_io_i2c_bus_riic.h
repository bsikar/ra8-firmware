/**
 * @file ra8_io_i2c_bus_riic.h
 * @brief RIIC backend binder for the ra8_io I2C-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_i2c_bus_t to a classic RIIC channel via
 * thin trampolines to `ra8_i2c_write` / `ra8_i2c_read` / `ra8_i2c_transfer`
 * (`ra8_i2c.h`, unmodified). The channel must already be initialised
 * through `ra8_i2c_init` -- the facade owns transfers only, never
 * bring-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @brief Bind `bus` to RIIC channel `channel`.
 *
 * @details
 * Installs the RIIC trampoline vtable and records the channel in the
 * handle context. No hardware is touched: initialise the channel with
 * `ra8_i2c_init(channel, ...)` (before or after binding) and route the
 * SCL/SDA pins yourself.
 *
 * @param[in,out] bus     Caller-owned handle to bind (non-NULL).
 * @param[in]     channel RIIC channel index (0, 1 or 2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              `bus` now dispatches to `ra8_i2c_*`.
 * @retval k_ra8_err_null_ptr    `bus` was NULL.
 * @retval k_ra8_err_invalid_arg `channel` is out of range.
 *
 * @pre `bus` points to writable storage.
 * @pre `channel` < 3 (the RA8D2 has IIC0..IIC2).
 * @post On success `bus->iface` is non-NULL and every `ra8_io_i2c_bus_*`
 *       call forwards to RIIC channel `channel`.
 * @post On failure `bus` is left unmodified.
 *
 * @note Thread-safe (writes only the caller's handle).
 *
 * @see ra8_io_i2c_bus_bind_i3c_compat  The I3C I2C-compatibility twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_i2c_bus_bind_riic(ra8_io_i2c_bus_t* bus, uint8_t channel);

#ifdef __cplusplus
}
#endif
