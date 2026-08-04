/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_i2c_bus_i3c_compat.c
 * @brief I3C I2C-compatibility backend for the ra8_io I2C-bus facade --
 *        thin shim over ra8_i3c.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_i2c_bus_iface by forwarding to the unified I3C driver
 * (`ra8_i3c.h`) in its I2C-compatibility controller mode, unmodified. The
 * bound channel index travels in the handle context
 * (`(void*)(uintptr_t)channel`), so the backend keeps no static state.
 *
 * Semantics adaptation: the facade (like `ra8_i2c_write`) exposes a
 * `send_stop` flag, while `ra8_i3c_write` takes the inverted `restart`
 * flag ("hold the bus for a following transfer"); the write trampoline
 * negates accordingly. `ra8_i3c_read` gets `restart = false` because the
 * facade read always releases the bus, matching `ra8_i2c_read`. No raw
 * MMIO is touched here; the driver carries every HUM citation.
 */

#include "ra8_io_i2c_bus_i3c_compat.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_i2c_bus_i3c";

/**
 * @brief I3C-compat backend: controller write.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra8_i3c_write` with `restart = !send_stop` (the I3C driver's flag says
 * "hold the bus", the facade's says "release it").
 *
 * @param[in] ctx       Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in] addr      7-bit peripheral address.
 * @param[in] data      Bytes to send.
 * @param[in] len       Byte count.
 * @param[in] send_stop true = STOP after payload; false = hold the bus.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Payload transmitted.
 * @retval k_ra8_err_null_ptr   `data` NULL with `len` > 0.
 * @retval k_ra8_err_nack       Peripheral NACKed.
 * @retval k_ra8_err_hw_timeout Bus stalled.
 *
 * @pre The channel was initialised via `ra8_i3c_init` in I2C mode.
 * @pre `ctx` carries a channel bound by `ra8_io_i2c_bus_bind_i3c_compat`.
 * @post On success the payload was transmitted per `send_stop`.
 * @post On failure the bus is released where the driver allows.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
i3c_compat_write(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop)
{
  return ra8_i3c_write((uint8_t)(uintptr_t)ctx, addr, data, len, !send_stop);
}

/**
 * @brief I3C-compat backend: controller read.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra8_i3c_read` with `restart = false`, so the transaction always ends
 * with STOP -- the same contract as `ra8_i2c_read`.
 *
 * @param[in]  ctx  Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  addr 7-bit peripheral address.
 * @param[out] data Destination buffer.
 * @param[in]  len  Byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bytes received.
 * @retval k_ra8_err_null_ptr    `data` was NULL.
 * @retval k_ra8_err_invalid_arg Channel/addr out of range or `len` zero.
 * @retval k_ra8_err_nack        Peripheral NACKed.
 *
 * @pre The channel was initialised via `ra8_i3c_init` in I2C mode.
 * @pre `ctx` carries a channel bound by `ra8_io_i2c_bus_bind_i3c_compat`.
 * @post On success `data` holds the received bytes.
 * @post STOP was issued and the bus released.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t i3c_compat_read(void* ctx, uint8_t addr, uint8_t* data, uint32_t len)
{
  return ra8_i3c_read((uint8_t)(uintptr_t)ctx, addr, data, len, false);
}

/**
 * @brief I3C-compat backend: write-then-RESTART-then-read.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra8_i3c_transfer`, which owns the repeated-START framing in I2C mode.
 *
 * @param[in]  ctx    Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  addr   7-bit peripheral address.
 * @param[in]  wr     Bytes to send first.
 * @param[in]  wr_len Number of bytes to send.
 * @param[out] rd     Destination buffer.
 * @param[in]  rd_len Number of bytes to read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Both phases completed; STOP issued.
 * @retval k_ra8_err_null_ptr      A required buffer was NULL.
 * @retval k_ra8_err_invalid_state Channel is in native I3C mode.
 * @retval k_ra8_err_nack          Peripheral NACKed.
 *
 * @pre The channel was initialised via `ra8_i3c_init` in I2C mode.
 * @pre `ctx` carries a channel bound by `ra8_io_i2c_bus_bind_i3c_compat`.
 * @post On success both phases completed and STOP was issued.
 * @post On failure the bus is released.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t i3c_compat_transfer(void*          ctx,
                                     uint8_t        addr,
                                     const uint8_t* wr,
                                     uint32_t       wr_len,
                                     uint8_t*       rd,
                                     uint32_t       rd_len)
{
  return ra8_i3c_transfer((uint8_t)(uintptr_t)ctx, addr, wr, wr_len, rd, rd_len);
}

/** @brief I3C I2C-compat backend vtable -- every row forwards to `ra8_i3c_*`. */
static const ra8_io_i2c_bus_iface_t k_i3c_compat_iface = {
  .write    = i3c_compat_write,
  .read     = i3c_compat_read,
  .transfer = i3c_compat_transfer,
};

ra8_err_t ra8_io_i2c_bus_bind_i3c_compat(ra8_io_i2c_bus_t* bus, uint8_t channel)
{
  RA8_CHECK_NULL_PTR(bus, s_tag, "bus must not be nullptr");
  if (channel >= (uint8_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  bus->iface = &k_i3c_compat_iface;
  bus->ctx   = (void*)(uintptr_t)channel;
  return k_ra8_ok;
}
