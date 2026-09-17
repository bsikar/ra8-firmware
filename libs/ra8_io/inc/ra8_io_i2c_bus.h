/**
 * @file ra8_io_i2c_bus.h
 * @brief ra8_io I2C-bus facade -- one controller-transfer vtable over the
 * @ingroup grp_io
 *        chip's two I2C implementations.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The RA8D2 implements I2C twice: the classic RIIC block (`ra8_i2c.h`,
 * three channels) and the I3C block's I2C-compatibility mode (`ra8_i3c.h`,
 * one channel). Their controller transfer surfaces are byte-for-byte
 * identical, but every consumer used to hard-wire one side by function
 * name. This facade gives callers one handle-based surface so the physical
 * peripheral a board revision routes to a device is a bind-time decision,
 * not a call-site rewrite.
 *
 * Modeled directly on the ::ra8_io_blockdev_t pattern: the handle
 * (::ra8_io_i2c_bus_t) is caller-allocated; a backend `_bind_*()` helper
 * (see `ra8_io_i2c_bus_riic.h` and `ra8_io_i2c_bus_i3c_compat.h`) fills the
 * opaque vtable with thin trampolines to the existing `ra8_i2c_*` /
 * `ra8_i3c_*` drivers, unmodified. No dynamic allocation occurs and no MMIO
 * happens here -- the wrapped drivers carry every HUM citation.
 *
 * Bus bring-up (peripheral init, bit-rate) stays with the caller. This
 * facade is deliberately separate from `ra8_io_spi_bus.h`: I2C carries an
 * in-band 7-bit address and is half-duplex request/response, while SPI has
 * out-of-band chip select and is simultaneous full-duplex -- one merged
 * shape would ignore a parameter or grow capability checks.
 *
 * ## Boundary with Ring-3 device drivers
 *
 * A Ring-3 device driver (e.g. `ra8_touch`, `ra8_smbus`) must not depend on
 * this Ring-4 facade -- that would invert ring ordering (see
 * `docs/RING_AND_WORLD.md`). The sanctioned bridge is
 * ::ra8_io_i2c_bus_as_ops, which exposes a bound bus through the Ring-3
 * seam `ra8_i2c_bus_ops_t` (`ra8_i2c_bus_ops.h`), mirroring
 * `ra8_io_blockdev_as_fs_backend()`.
 *
 * @par Example:
 * @code
 * ra8_io_i2c_bus_t bus = {};
 * (void)ra8_io_i2c_bus_bind_i3c_compat(&bus, 0U); // today's board
 * // (void)ra8_io_i2c_bus_bind_riic(&bus, 1U);    // future board revision
 * uint8_t reg_ptr[2] = {0x81U, 0x40U};
 * uint8_t status     = 0U;
 * (void)ra8_io_i2c_bus_transfer(&bus, 0x5DU, reg_ptr, 2U, &status, 1U);
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
#include "ra8_i2c_bus_ops.h"

/* =============================================================================
 * Interface + handle
 * =============================================================================
 */

/**
 * @struct ra8_io_i2c_bus_iface
 * @brief Opaque to applications -- backends implement this internally and
 *        export a binding helper through their own header.
 *
 * @details
 * The concrete vtable layout lives in `src/ra8_io_i2c_bus_internal.h`. Apps
 * never construct one; they call a backend's `_bind_*()` helper, which binds
 * the backend's const vtable into a caller-owned ::ra8_io_i2c_bus_t.
 *
 * @since 0.1.0
 */
typedef struct ra8_io_i2c_bus_iface ra8_io_i2c_bus_iface_t;

/**
 * @struct ra8_io_i2c_bus_t
 * @brief Caller-allocated I2C-bus handle binding a backend to its context.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to a backend `_bind_*()` helper, which
 * fills the vtable and context. Treat the fields as private; reach the bus
 * only through the `ra8_io_i2c_bus_*` functions below. The handle must
 * out-live every call made through it (including calls made through an
 * `ra8_i2c_bus_ops_t` filled by ::ra8_io_i2c_bus_as_ops).
 *
 * @invariant `iface` is non-NULL once a backend has been bound.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_i2c_bus_iface_t* iface; /**< Bound backend vtable (private).    */
  void*                         ctx;   /**< Backend-private context (private). */
} ra8_io_i2c_bus_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Controller write of `len` bytes to 7-bit address `addr`.
 *
 * @details
 * Forwards to the bound backend's write primitive (`ra8_i2c_write` or
 * `ra8_i3c_write`; the I3C driver's inverted `restart` flag is adapted by
 * the backend). When `send_stop` is true the transaction ends with STOP;
 * when false the bus is held so the next call injects a repeated-START.
 *
 * @param[in] bus       Bound bus handle.
 * @param[in] addr      7-bit peripheral address.
 * @param[in] data      Bytes to send (non-NULL when `len` > 0).
 * @param[in] len       Byte count.
 * @param[in] send_stop true = STOP after the payload; false = hold the bus.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Payload transmitted.
 * @retval k_ra8_err_null_ptr        `bus` was NULL, or `data` NULL with
 *                                  `len` > 0.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_nack            Peripheral did not acknowledge.
 * @retval k_ra8_err_hw_timeout      Bus stalled.
 *
 * @pre A backend has been bound into `bus`.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success the payload has been transmitted per `send_stop`.
 * @post On failure the bus is released where the transport allows.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @see ra8_io_i2c_bus_transfer  Write-then-read in one transaction.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_i2c_bus_write(const ra8_io_i2c_bus_t* bus,
                                             uint8_t                 addr,
                                             const uint8_t*          data,
                                             uint32_t                len,
                                             bool                    send_stop);

/**
 * @brief Controller read of `len` bytes from 7-bit address `addr`.
 *
 * @details
 * Forwards to the bound backend's read primitive (`ra8_i2c_read` or
 * `ra8_i3c_read`). Issues START, the address byte with R/W# = 1, drains
 * `len` bytes, then issues STOP and releases the bus.
 *
 * @param[in]  bus  Bound bus handle.
 * @param[out] data Destination buffer (non-NULL).
 * @param[in]  addr 7-bit peripheral address.
 * @param[in]  len  Byte count (non-zero).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Bytes received.
 * @retval k_ra8_err_null_ptr        `bus` or `data` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_invalid_arg     `len` is zero.
 * @retval k_ra8_err_nack            Peripheral did not acknowledge.
 * @retval k_ra8_err_hw_timeout      Bus stalled.
 *
 * @pre A backend has been bound into `bus`.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success `data[0 .. len)` holds the received bytes.
 * @post STOP was issued and the bus released (success or failure).
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @see ra8_io_i2c_bus_write  Controller-write counterpart.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_i2c_bus_read(const ra8_io_i2c_bus_t* bus, uint8_t addr, uint8_t* data, uint32_t len);

/**
 * @brief Combined write-then-RESTART-then-read in one bus transaction.
 *
 * @details
 * Forwards to the bound backend's combined primitive (`ra8_i2c_transfer`
 * or `ra8_i3c_transfer`) -- the classic "address a register, read its
 * contents" shape: write `wr_len` bytes, repeated START, read `rd_len`
 * bytes, STOP.
 *
 * @param[in]  bus    Bound bus handle.
 * @param[in]  addr   7-bit peripheral address.
 * @param[in]  wr     Bytes to send first (NULL only when `wr_len` 0).
 * @param[in]  wr_len Number of bytes to send.
 * @param[out] rd     Destination buffer (NULL only when `rd_len` 0).
 * @param[in]  rd_len Number of bytes to read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Both phases completed; STOP issued.
 * @retval k_ra8_err_null_ptr        `bus` was NULL, or a required buffer
 *                                  was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `bus`.
 * @retval k_ra8_err_invalid_arg     Both lengths zero.
 * @retval k_ra8_err_nack            Peripheral did not acknowledge.
 * @retval k_ra8_err_hw_timeout      Bus stalled.
 *
 * @pre A backend has been bound into `bus`.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success both phases completed and STOP was issued.
 * @post On failure the bus is released where the transport allows.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_i2c_bus_transfer(const ra8_io_i2c_bus_t* bus,
                                                uint8_t                 addr,
                                                const uint8_t*          wr,
                                                uint32_t                wr_len,
                                                uint8_t*                rd,
                                                uint32_t                rd_len);

/**
 * @brief Expose a bound bus through the Ring-3 seam `ra8_i2c_bus_ops_t`.
 *
 * @details
 * Fills `out` with trampolines that forward the seam's `write` / `read` /
 * `transfer` into this facade, with `out->ctx` pointing at `bus`. This is
 * the sanctioned bridge for handing a bound bus to a Ring-3 device driver
 * (e.g. `ra8_touch`, `ra8_smbus`) without the driver depending upward on
 * `ra8_io` -- the exact analogue of `ra8_io_blockdev_as_fs_backend()`.
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
 * @post On success all three seam rows are non-NULL and `out->ctx`
 *       references `bus`.
 * @post No bus or handle state is mutated.
 *
 * @note `bus` must remain valid for the entire lifetime of the seam.
 *
 * @see ra8_i2c_bus_ops_t  The Ring-3 seam definition.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_i2c_bus_as_ops(const ra8_io_i2c_bus_t* bus, ra8_i2c_bus_ops_t* out);

#ifdef __cplusplus
}
#endif
