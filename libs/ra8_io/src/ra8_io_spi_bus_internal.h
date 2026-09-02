/**
 * @file ra8_io_spi_bus_internal.h
 * @brief Internal vtable type for the ra8_io SPI-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the SPI-bus dispatcher and the backend
 * translation units include this file. Tests under `tests/` MAY include it
 * to drive MC/DC vectors against internal helpers; application code never
 * should.
 *
 * Defines ::ra8_io_spi_bus_iface -- the function-pointer table each backend
 * fills in and binds into a caller-owned ::ra8_io_spi_bus_t. It is forward
 * declared opaque in `ra8_io_spi_bus.h`.
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
#include "ra8_io_spi_bus.h"
#include "ra8_spi.h"

/**
 * @struct ra8_io_spi_bus_iface
 * @brief One vtable per SPI transport. Each callback receives the backend's
 *        opaque `ctx` (the bound channel) so backends keep no static state.
 *
 * @details
 * All three rows are mandatory: both RA8D2 SPI transports implement the
 * full transfer surface, so the dispatcher never NULL-checks a row beyond
 * the binding check on the handle itself.
 *
 * @invariant `xfer8`, `write_read`, and `set_clock` are non-NULL.
 *
 * @since 0.1.0
 */
struct ra8_io_spi_bus_iface {
  /** @brief Full-duplex single-byte exchange (`rx` may be NULL). */
  ra8_err_t (*xfer8)(void* ctx, uint8_t tx, uint8_t* rx);

  /** @brief Full-duplex multi-frame transfer (either buffer may be NULL). */
  ra8_err_t (
    *write_read)(void* ctx, const void* tx, void* rx, uint32_t len, ra8_spi_bit_width_t width);

  /** @brief Re-program the bit-rate divider from `baud_hz` / `pclk_hz`. */
  ra8_err_t (*set_clock)(void* ctx, uint32_t baud_hz, uint32_t pclk_hz);
};

#ifdef __cplusplus
}
#endif
