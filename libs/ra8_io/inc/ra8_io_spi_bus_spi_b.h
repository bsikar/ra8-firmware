/**
 * @file ra8_io_spi_bus_spi_b.h
 * @brief SPI_B backend binder for the ra8_io SPI-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_spi_bus_t to the dedicated SPI_B block via
 * thin trampolines to `ra8_spi_xfer8` / `ra8_spi_write_read` /
 * `ra8_spi_set_clock` (`ra8_spi.h`, unmodified). The channel must already be
 * initialised through `ra8_spi_init` -- the facade owns transfers only,
 * never bring-up, so the caller keeps full control of mode, bit-rate, and
 * chip-select routing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_spi_bus.h"

/**
 * @brief Bind `bus` to SPI_B channel `channel`.
 *
 * @details
 * Installs the SPI_B trampoline vtable and records the channel in the
 * handle context. No hardware is touched: initialise the channel with
 * `ra8_spi_init(channel, ...)` (before or after binding) and route the
 * pins yourself.
 *
 * @param[in,out] bus     Caller-owned handle to bind (non-NULL).
 * @param[in]     channel SPI_B channel index (0 or 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              `bus` now dispatches to `ra8_spi_*`.
 * @retval k_ra8_err_null_ptr    `bus` was NULL.
 * @retval k_ra8_err_invalid_arg `channel` is out of range.
 *
 * @pre `bus` points to writable storage.
 * @pre `channel` < 2 (the RA8D2 has SPI0 and SPI1).
 * @post On success `bus->iface` is non-NULL and every `ra8_io_spi_bus_*`
 *       call forwards to SPI_B channel `channel`.
 * @post On failure `bus` is left unmodified.
 *
 * @note Thread-safe (writes only the caller's handle).
 *
 * @see ra8_io_spi_bus_bind_sci_spi  The SCI Simple-SPI twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_spi_bus_bind_spi_b(ra8_io_spi_bus_t* bus, uint8_t channel);

#ifdef __cplusplus
}
#endif
