/**
 * @file ra8_io_spi_bus.h
 * @brief ra8_io SPI-bus facade -- one controller-transfer vtable over the
 * @ingroup grp_io
 *        chip's two SPI implementations.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The RA8D2 implements SPI twice: the dedicated SPI_B block (`ra8_spi.h`)
 * and SCI in Simple-SPI mode (`ra8_sci_spi.h` -- the transport behind the
 * Pmod2 microSD path). Their controller transfer surfaces are byte-for-byte
 * identical, but every consumer used to hard-wire one side by function
 * name. This facade gives callers one handle-based surface so the physical
 * peripheral a board revision routes to a device is a bind-time decision,
 * not a call-site rewrite.
 *
 * Modeled directly on the ::ra8_io_blockdev_t pattern: the handle
 * (::ra8_io_spi_bus_t) is caller-allocated; a backend `_bind_*()` helper
 * (see `ra8_io_spi_bus_spi_b.h` and `ra8_io_spi_bus_sci_spi.h`) fills the
 * opaque vtable with thin trampolines to the existing `ra8_spi_*` /
 * `ra8_sci_spi_*` drivers, unmodified. No dynamic allocation occurs and no
 * MMIO happens here -- the wrapped drivers carry every HUM citation.
 *
 * Bus bring-up (peripheral init, CPOL/CPHA, chip-select GPIO) stays with
 * the caller: SPI device select is an out-of-band GPIO, so it is not part
 * of this interface. I2C/I3C, which carry an in-band address, get their
 * own facade (`ra8_io_i2c_bus.h`) rather than a merged one.
 *
 * ## Boundary with Ring-3 device drivers
 *
 * A Ring-3 device driver (e.g. `ra8_epaper`) must not depend on this
 * Ring-4 facade -- that would invert ring ordering (see
 * `docs/RING_AND_WORLD.md`). The sanctioned bridge is
 * ::ra8_io_spi_bus_as_ops, which exposes a bound bus through the Ring-3
 * seam `ra8_spi_bus_ops_t` (`ra8_spi_bus_ops.h`), mirroring
 * `ra8_io_blockdev_as_fs_backend()`.
 *
 * @par Example:
 * @code
 * ra8_io_spi_bus_t bus = {};
 * (void)ra8_io_spi_bus_bind_spi_b(&bus, 0U);      // today's board
 * // (void)ra8_io_spi_bus_bind_sci_spi(&bus, 0U); // future board revision
 * uint8_t rx = 0U;
 * (void)ra8_io_spi_bus_xfer8(&bus, 0xA5U, &rx);
 * @endcode
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
#include "ra8_spi.h"
#include "ra8_spi_bus_ops.h"

/* =============================================================================
 * Interface + handle
 * =============================================================================
 */

/**
 * @struct ra8_io_spi_bus_iface
 * @brief Opaque to applications -- backends implement this internally and
 *        export a binding helper through their own header.
 *
 * @details
 * The concrete vtable layout lives in `src/ra8_io_spi_bus_internal.h`. Apps
 * never construct one; they call a backend's `_bind_*()` helper, which binds
 * the backend's const vtable into a caller-owned ::ra8_io_spi_bus_t.
 *
 * @since 0.1.0
 */
typedef struct ra8_io_spi_bus_iface ra8_io_spi_bus_iface_t;

/**
 * @struct ra8_io_spi_bus_t
 * @brief Caller-allocated SPI-bus handle binding a backend to its context.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to a backend `_bind_*()` helper, which
 * fills the vtable and context. Treat the fields as private; reach the bus
 * only through the `ra8_io_spi_bus_*` functions below. The handle must
 * out-live every call made through it (including calls made through an
 * `ra8_spi_bus_ops_t` filled by ::ra8_io_spi_bus_as_ops).
 *
 * @invariant `iface` is non-NULL once a backend has been bound.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_spi_bus_iface_t* iface; /**< Bound backend vtable (private).    */
  void*                         ctx;   /**< Backend-private context (private). */
} ra8_io_spi_bus_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Full-duplex single-byte exchange on the bound bus.
 *
 * @details
 * Forwards to the bound backend's `xfer8` primitive (`ra8_spi_xfer8` or
 * `ra8_sci_spi_xfer8`): shifts `tx` out on COPI and captures the CIPO byte
 * into `*rx`.
 *
 * @param[in]  bus Bound bus handle.
 * @param[in]  tx  Byte to transmit.
 * @param[out] rx  Receive slot; may be NULL to discard the received byte.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame exchanged.
 * @retval k_ra8_err_null_ptr        `bus` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_hw_timeout      The transport's status poll expired.
 *
 * @pre A backend has been bound into `bus`.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success one frame was clocked; `*rx` (if non-NULL) holds the
 *       received byte.
 * @post On any non-ok return no caller-visible handle state changed.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @see ra8_io_spi_bus_write_read  Multi-frame variant.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_spi_bus_xfer8(const ra8_io_spi_bus_t* bus, uint8_t tx, uint8_t* rx);

/**
 * @brief Full-duplex multi-frame transfer on the bound bus.
 *
 * @details
 * Forwards to the backend's bulk primitive (`ra8_spi_write_read` or
 * `ra8_sci_spi_xfer`). Either buffer may be NULL: a NULL `tx` shifts idle
 * fill bytes (SD / SPI-flash read convention) and a NULL `rx` discards the
 * received bytes; both NULL is rejected by the backend. `len == 0` is a
 * no-op success. The SCI Simple-SPI backend supports 8-bit frames only and
 * reports ::k_ra8_err_not_supported for wider `width` values.
 *
 * @param[in]  bus   Bound bus handle.
 * @param[in]  tx    Source buffer (`len` units of `width`) or NULL.
 * @param[out] rx    Destination buffer (`len` units of `width`) or NULL.
 * @param[in]  len   Number of frames to exchange.
 * @param[in]  width Per-frame bit width (::ra8_spi_bit_width_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Every frame exchanged.
 * @retval k_ra8_err_null_ptr        `bus` was NULL, or both buffers NULL
 *                                  with `len` > 0.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_not_supported   Backend cannot do `width` (SCI != 8).
 * @retval k_ra8_err_hw_timeout      The transport's status poll expired.
 *
 * @pre A backend has been bound into `bus`.
 * @pre Buffer alignment matches `width` (16/32-bit frames need aligned
 *      pointers on the SPI_B backend).
 * @post On success `len` frames were clocked in both directions.
 * @post On any non-ok return no caller-visible handle state changed.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @see ra8_io_spi_bus_xfer8  Single-byte variant.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_spi_bus_write_read(const ra8_io_spi_bus_t* bus,
                                                  const void*             tx,
                                                  void*                   rx,
                                                  uint32_t                len,
                                                  ra8_spi_bit_width_t     width);

/**
 * @brief Re-program the bound bus bit-rate without reconfiguring.
 *
 * @details
 * Forwards to `ra8_spi_set_clock` / `ra8_sci_spi_set_clock`. Used by
 * protocols that must start slow and speed up after negotiation (e.g. the
 * SD-card 400 kHz identification phase).
 *
 * @param[in] bus     Bound bus handle.
 * @param[in] baud_hz Target bit-rate in Hz (non-zero).
 * @param[in] pclk_hz Current peripheral clock in Hz (PCLKA on RA8D2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Bit-rate divider reprogrammed.
 * @retval k_ra8_err_null_ptr        `bus` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_invalid_arg     `baud_hz` zero or divider out of range.
 *
 * @pre A backend has been bound into `bus`.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success the channel's divider reflects the new rate.
 * @post On any non-ok return no caller-visible handle state changed.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_spi_bus_set_clock(const ra8_io_spi_bus_t* bus, uint32_t baud_hz, uint32_t pclk_hz);

/**
 * @brief Expose a bound bus through the Ring-3 seam `ra8_spi_bus_ops_t`.
 *
 * @details
 * Fills `out` with a trampoline that forwards the seam's `xfer8` into this
 * facade, with `out->ctx` pointing at `bus`. This is the sanctioned bridge
 * for handing a bound bus to a Ring-3 device driver (e.g. `ra8_epaper`)
 * without the driver depending upward on `ra8_io` -- the exact analogue of
 * `ra8_io_blockdev_as_fs_backend()`.
 *
 * @param[in]  bus Bound bus handle (must out-live every seam call).
 * @param[out] out Seam to populate.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out` wired to `bus`.
 * @retval k_ra8_err_null_ptr        `bus` or `out` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 *
 * @pre A backend has been bound into `bus`.
 * @pre `out` is writable and out-lives no call made through it after
 *      `bus` is destroyed.
 * @post On success `out->xfer8` is non-NULL and `out->ctx` references
 *       `bus`.
 * @post No bus or handle state is mutated.
 *
 * @note `bus` must remain valid for the entire lifetime of the seam.
 *
 * @see ra8_spi_bus_ops_t  The Ring-3 seam definition.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_spi_bus_as_ops(const ra8_io_spi_bus_t* bus, ra8_spi_bus_ops_t* out);

#ifdef __cplusplus
}
#endif
