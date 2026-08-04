/**
 * @file ra8_spi_bus_ops.h
 * @brief Injected SPI-bus seam consumed by Ring-3 SPI device drivers.
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 implements SPI twice: the dedicated SPI_B block (`ra8_spi.h`)
 * and SCI in Simple-SPI mode (`ra8_sci_spi.h`). A device driver at
 * [Ring 3 / HAL] (e.g. the IT8951 e-paper driver in `ra8_epaper.h`) must
 * not care which physical peripheral its bus pins route to -- pin routing
 * differs per board revision -- so it takes this small function-pointer
 * seam in its config instead of naming `ra8_spi_*` or `ra8_sci_spi_*`
 * directly.
 *
 * This seam deliberately lives at Ring 3, NOT in the `ra8_io` fabric: the
 * peripheral-agnostic bus facade `ra8_io_spi_bus.h` sits at [Ring 4 / PAL],
 * and a Ring-3 driver depending on it would invert ring ordering (see
 * `docs/RING_AND_WORLD.md` and the identical `ra8_vsource` /
 * `ra8_io_blockdev` split). The sanctioned bridge is the Ring-4 adapter
 * `ra8_io_spi_bus_as_ops()`, which fills this struct with trampolines into
 * a bound ::ra8_io_spi_bus_t; the app owns the bus, binds a backend, and
 * hands the filled seam to the device driver's `_init()`/`_open()` cfg.
 *
 * The seam is transfer-only by design: bus bring-up (peripheral init,
 * CPOL/CPHA mode, bit-rate) is the app's responsibility, because only the
 * app knows which peripheral the board wires to the device.
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

/**
 * @struct ra8_spi_bus_ops_t
 * @brief Caller-filled SPI transfer seam: one full-duplex byte exchange.
 *
 * @details
 * A device driver stores this struct by value from its config and routes
 * every SPI byte through `xfer8(ctx, ...)`. The `ctx` cookie is opaque to
 * the driver; the binder that filled the seam decides what it points at
 * (the Ring-4 `ra8_io_spi_bus_as_ops()` points it at the bound
 * ::ra8_io_spi_bus_t handle, which must therefore out-live the driver).
 * Tests may fill the seam directly with a local trampoline.
 *
 * @invariant `xfer8` is non-NULL once the seam has been filled.
 * @invariant Whatever `ctx` references out-lives every `xfer8` call.
 *
 * @par Example:
 * @code
 * ra8_io_spi_bus_t bus = {};
 * (void)ra8_io_spi_bus_bind_spi_b(&bus, 0U);
 * ra8_spi_bus_ops_t ops = {};
 * (void)ra8_io_spi_bus_as_ops(&bus, &ops);
 * ra8_epaper_cfg_t cfg = { .bus = ops, ... };
 * @endcode
 *
 * @see ra8_io_spi_bus_as_ops  Ring-4 binder over the `ra8_io` SPI facade.
 * @see ra8_epaper_cfg_t       First Ring-3 consumer of this seam.
 *
 * @since 0.1.0
 */
typedef struct {
  /**
   * @brief Full-duplex single-byte exchange on the bound bus.
   *
   * @details
   * Shifts `tx` out on COPI while capturing the CIPO byte into `*rx`.
   * `rx` may be NULL to discard the received byte. Matches the
   * `ra8_spi_xfer8` / `ra8_sci_spi_xfer8` contract byte for byte.
   *
   * @param[in]  ctx Binder-owned cookie (the `ctx` member below).
   * @param[in]  tx  Byte to shift out.
   * @param[out] rx  Receive slot; may be NULL to discard.
   *
   * @return `ra8_err_t` forwarded from the bound transport.
   */
  ra8_err_t (*xfer8)(void* ctx, uint8_t tx, uint8_t* rx);

  void* ctx; /**< Opaque cookie handed to `xfer8` (binder-owned). */
} ra8_spi_bus_ops_t;

#ifdef __cplusplus
}
#endif
