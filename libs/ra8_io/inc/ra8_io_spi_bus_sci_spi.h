/**
 * @file ra8_io_spi_bus_sci_spi.h
 * @brief SCI Simple-SPI backend binder for the ra8_io SPI-bus facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_spi_bus_t to an SCI channel running in
 * Simple-SPI mode via thin trampolines to `ra8_sci_spi_xfer8` /
 * `ra8_sci_spi_xfer` / `ra8_sci_spi_set_clock` (`ra8_sci_spi.h`, unmodified).
 * This is the transport behind the EK-RA8D2 Pmod2 microSD path. The
 * channel must already be initialised through `ra8_sci_spi_init` -- the
 * facade owns transfers only, never bring-up.
 *
 * The SCI block clocks 8-bit frames only, so
 * `ra8_io_spi_bus_write_read` on this backend reports
 * ::k_ra8_err_not_supported for 16/32-bit widths.
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
 * @brief Bind `bus` to SCI Simple-SPI channel `channel`.
 *
 * @details
 * Installs the SCI Simple-SPI trampoline vtable and records the channel
 * in the handle context. No hardware is touched: initialise the channel
 * with `ra8_sci_spi_init(channel, ...)` (before or after binding) and own
 * the chip-select as a GPIO yourself.
 *
 * @param[in,out] bus     Caller-owned handle to bind (non-NULL).
 * @param[in]     channel SCI channel index (0..9).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              `bus` now dispatches to `ra8_sci_spi_*`.
 * @retval k_ra8_err_null_ptr    `bus` was NULL.
 * @retval k_ra8_err_invalid_arg `channel` is out of range.
 *
 * @pre `bus` points to writable storage.
 * @pre `channel` < 10 (the RA8D2 has SCI0..SCI9).
 * @post On success `bus->iface` is non-NULL and every `ra8_io_spi_bus_*`
 *       call forwards to SCI channel `channel` in Simple-SPI mode.
 * @post On failure `bus` is left unmodified.
 *
 * @note Thread-safe (writes only the caller's handle).
 *
 * @see ra8_io_spi_bus_bind_spi_b  The dedicated SPI_B twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_spi_bus_bind_sci_spi(ra8_io_spi_bus_t* bus, uint8_t channel);

#ifdef __cplusplus
}
#endif
