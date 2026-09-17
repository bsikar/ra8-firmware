/**
 * @file ra8_io_i2c_bus_internal.h
 * @brief Internal vtable type for the ra8_io I2C-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the I2C-bus dispatcher and the backend
 * translation units include this file. Tests under `tests/` MAY include it
 * to drive MC/DC vectors against internal helpers; application code never
 * should.
 *
 * Defines ::ra8_io_i2c_bus_iface -- the function-pointer table each backend
 * fills in and binds into a caller-owned ::ra8_io_i2c_bus_t. It is forward
 * declared opaque in `ra8_io_i2c_bus.h`.
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
 * @struct ra8_io_i2c_bus_iface
 * @brief One vtable per I2C transport. Each callback receives the backend's
 *        opaque `ctx` (the bound channel) so backends keep no static state.
 *
 * @details
 * All three rows are mandatory: both RA8D2 I2C transports implement the
 * full controller transfer surface, so the dispatcher never NULL-checks a
 * row beyond the binding check on the handle itself.
 *
 * @invariant `write`, `read`, and `transfer` are non-NULL.
 *
 * @since 0.1.0
 */
struct ra8_io_i2c_bus_iface {
  /** @brief Controller write; `send_stop` false holds the bus. */
  ra8_err_t (*write)(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop);

  /** @brief Controller read; always ends with STOP. */
  ra8_err_t (*read)(void* ctx, uint8_t addr, uint8_t* data, uint32_t len);

  /** @brief Write-then-RESTART-then-read combined transaction. */
  ra8_err_t (*transfer)(void*          ctx,
                        uint8_t        addr,
                        const uint8_t* wr,
                        uint32_t       wr_len,
                        uint8_t*       rd,
                        uint32_t       rd_len);
};

#ifdef __cplusplus
}
#endif
