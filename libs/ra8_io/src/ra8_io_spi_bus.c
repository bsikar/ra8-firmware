/**
 * @file ra8_io_spi_bus.c
 * @brief SPI-bus dispatcher -- forwards each public call into the bound
 *        backend vtable, and bridges any backend to an `ra8_spi_bus_ops_t`.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Stateless dispatcher: it validates the handle, then forwards through the
 * bound ::ra8_io_spi_bus_iface. The bridge installs a static trampoline
 * whose `ctx` is the ::ra8_io_spi_bus_t itself, mirroring the
 * `ra8_io_blockdev_as_fs_backend()` idiom. No MMIO is touched here; the
 * wrapped drivers carry every HUM citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_spi_bus.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_spi_bus_internal.h"
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_spi_bus";

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reject a handle that is NULL or has no backend bound.
 *
 * @details
 * Run on every dispatch path. Kept tiny so each public entry point stays
 * well under the NASA Power-of-10 Rule 4 sixty-line cap.
 *
 * @param[in] bus Candidate handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `bus` is non-NULL with a bound backend.
 * @retval k_ra8_err_null_ptr        `bus` was NULL.
 * @retval k_ra8_err_not_initialized `bus->iface` was NULL (never bound).
 *
 * @pre None.
 * @pre None.
 * @post No state is mutated.
 * @post The return reflects only the binding state of `bus`.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate(const ra8_io_spi_bus_t* bus)
{
  if (bus == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (bus->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API -- dispatch through the bound backend
 * =============================================================================
 */

ra8_err_t ra8_io_spi_bus_xfer8(const ra8_io_spi_bus_t* bus, uint8_t tx, uint8_t* rx)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->xfer8, s_tag, "backend xfer8 op missing");
  return bus->iface->xfer8(bus->ctx, tx, rx);
}

ra8_err_t ra8_io_spi_bus_write_read(const ra8_io_spi_bus_t* bus,
                                    const void*             tx,
                                    void*                   rx,
                                    uint32_t                len,
                                    ra8_spi_bit_width_t     width)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->write_read, s_tag, "backend write_read op missing");
  return bus->iface->write_read(bus->ctx, tx, rx, len, width);
}

ra8_err_t ra8_io_spi_bus_set_clock(const ra8_io_spi_bus_t* bus, uint32_t baud_hz, uint32_t pclk_hz)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->set_clock, s_tag, "backend set_clock op missing");
  return bus->iface->set_clock(bus->ctx, baud_hz, pclk_hz);
}

/* =============================================================================
 * ra8_spi_bus_ops_t bridge (Ring-3 seam)
 * =============================================================================
 */

/**
 * @brief Seam trampoline -- forward an `xfer8` into the bound bus.
 *
 * @details
 * Casts the seam cookie back to the bus handle and dispatches the byte
 * exchange through ::ra8_io_spi_bus_xfer8.
 *
 * @param[in]  ctx The ::ra8_io_spi_bus_t handle (as a void cookie).
 * @param[in]  tx  Byte to transmit.
 * @param[out] rx  Receive slot; may be NULL to discard.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Frame exchanged.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_spi_bus_xfer8.
 *
 * @pre `ctx` is a bound ::ra8_io_spi_bus_t.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success one frame was clocked.
 * @post No handle state is mutated.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ops_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx (spi bus) must not be nullptr");
  return ra8_io_spi_bus_xfer8((const ra8_io_spi_bus_t*)ctx, tx, rx);
}

ra8_err_t ra8_io_spi_bus_as_ops(const ra8_io_spi_bus_t* bus, ra8_spi_bus_ops_t* out)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  out->xfer8 = internal_ops_xfer8;
  out->ctx   = (void*)(uintptr_t)bus;
  return k_ra8_ok;
}
