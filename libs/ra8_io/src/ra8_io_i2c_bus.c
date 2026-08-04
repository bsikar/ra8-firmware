/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_i2c_bus.c
 * @brief I2C-bus dispatcher -- forwards each public call into the bound
 *        backend vtable, and bridges any backend to an `ra8_i2c_bus_ops_t`.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Stateless dispatcher: it validates the handle, then forwards through the
 * bound ::ra8_io_i2c_bus_iface. The bridge installs static trampolines
 * whose `ctx` is the ::ra8_io_i2c_bus_t itself, mirroring the
 * `ra8_io_blockdev_as_fs_backend()` idiom. No MMIO is touched here; the
 * wrapped drivers carry every HUM citation.
 */

#include "ra8_io_i2c_bus.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_io_i2c_bus_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_i2c_bus";

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
static ra8_err_t internal_validate(const ra8_io_i2c_bus_t* bus)
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

ra8_err_t ra8_io_i2c_bus_write(const ra8_io_i2c_bus_t* bus,
                               uint8_t                 addr,
                               const uint8_t*          data,
                               uint32_t                len,
                               bool                    send_stop)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->write, s_tag, "backend write op missing");
  return bus->iface->write(bus->ctx, addr, data, len, send_stop);
}

ra8_err_t
ra8_io_i2c_bus_read(const ra8_io_i2c_bus_t* bus, uint8_t addr, uint8_t* data, uint32_t len)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->read, s_tag, "backend read op missing");
  return bus->iface->read(bus->ctx, addr, data, len);
}

ra8_err_t ra8_io_i2c_bus_transfer(const ra8_io_i2c_bus_t* bus,
                                  uint8_t                 addr,
                                  const uint8_t*          wr,
                                  uint32_t                wr_len,
                                  uint8_t*                rd,
                                  uint32_t                rd_len)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(bus->iface->transfer, s_tag, "backend transfer op missing");
  return bus->iface->transfer(bus->ctx, addr, wr, wr_len, rd, rd_len);
}

/* =============================================================================
 * ra8_i2c_bus_ops_t bridge (Ring-3 seam)
 * =============================================================================
 */

/**
 * @brief Seam trampoline -- forward a controller write into the bound bus.
 *
 * @details
 * Casts the seam cookie back to the bus handle and dispatches the write
 * through ::ra8_io_i2c_bus_write.
 *
 * @param[in] ctx       The ::ra8_io_i2c_bus_t handle (as a void cookie).
 * @param[in] addr      7-bit peripheral address.
 * @param[in] data      Bytes to send.
 * @param[in] len       Byte count.
 * @param[in] send_stop true = STOP after payload; false = hold the bus.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Payload transmitted.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_i2c_bus_write.
 *
 * @pre `ctx` is a bound ::ra8_io_i2c_bus_t.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success the payload has been transmitted.
 * @post No handle state is mutated.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_ops_write(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx (i2c bus) must not be nullptr");
  return ra8_io_i2c_bus_write((const ra8_io_i2c_bus_t*)ctx, addr, data, len, send_stop);
}

/**
 * @brief Seam trampoline -- forward a controller read into the bound bus.
 *
 * @details
 * Casts the seam cookie back to the bus handle and dispatches the read
 * through ::ra8_io_i2c_bus_read.
 *
 * @param[in]  ctx  The ::ra8_io_i2c_bus_t handle (as a void cookie).
 * @param[in]  addr 7-bit peripheral address.
 * @param[out] data Destination buffer.
 * @param[in]  len  Byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Bytes received.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_i2c_bus_read.
 *
 * @pre `ctx` is a bound ::ra8_io_i2c_bus_t.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success `data` holds the received bytes.
 * @post No handle state is mutated.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ops_read(void* ctx, uint8_t addr, uint8_t* data, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx (i2c bus) must not be nullptr");
  return ra8_io_i2c_bus_read((const ra8_io_i2c_bus_t*)ctx, addr, data, len);
}

/**
 * @brief Seam trampoline -- forward a combined transfer into the bound bus.
 *
 * @details
 * Casts the seam cookie back to the bus handle and dispatches the
 * write-then-RESTART-then-read through ::ra8_io_i2c_bus_transfer.
 *
 * @param[in]  ctx    The ::ra8_io_i2c_bus_t handle (as a void cookie).
 * @param[in]  addr   7-bit peripheral address.
 * @param[in]  wr     Bytes to send first.
 * @param[in]  wr_len Number of bytes to send.
 * @param[out] rd     Destination buffer.
 * @param[in]  rd_len Number of bytes to read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Both phases completed.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_io_i2c_bus_transfer.
 *
 * @pre `ctx` is a bound ::ra8_io_i2c_bus_t.
 * @pre The underlying channel was initialised by its own driver init.
 * @post On success both phases completed and STOP was issued.
 * @post No handle state is mutated.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ops_transfer(void*          ctx,
                                       uint8_t        addr,
                                       const uint8_t* wr,
                                       uint32_t       wr_len,
                                       uint8_t*       rd,
                                       uint32_t       rd_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx (i2c bus) must not be nullptr");
  return ra8_io_i2c_bus_transfer((const ra8_io_i2c_bus_t*)ctx, addr, wr, wr_len, rd, rd_len);
}

ra8_err_t ra8_io_i2c_bus_as_ops(const ra8_io_i2c_bus_t* bus, ra8_i2c_bus_ops_t* out)
{
  const ra8_err_t v = internal_validate(bus);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  out->write    = internal_ops_write;
  out->read     = internal_ops_read;
  out->transfer = internal_ops_transfer;
  out->ctx      = (void*)(uintptr_t)bus;
  return k_ra8_ok;
}
