/**
 * @file ra_io_i2c_bus_riic.c
 * @brief RIIC backend for the ra_io I2C-bus facade -- thin shim over ra_i2c.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra_io_i2c_bus_iface by forwarding to the classic RIIC
 * polling driver (`ra_i2c.h`), unmodified. The bound channel index travels
 * in the handle context (`(void*)(uintptr_t)channel`), so the backend
 * keeps no static state and multiple bound buses coexist. Argument
 * validation (channel range, NULL buffers, zero lengths) stays with the
 * wrapped driver, which also carries every HUM citation -- no raw MMIO is
 * touched here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_i2c_bus_riic.h"

#include <stdint.h>

#include "ra8d2_i2c_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_i2c.h"
#include "ra_io_i2c_bus.h"
#include "ra_io_i2c_bus_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_i2c_bus_riic";

/**
 * @brief RIIC backend: controller write.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra_i2c_write`, whose `send_stop` flag matches the facade contract
 * directly.
 *
 * @param[in] ctx       Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in] addr      7-bit peripheral address.
 * @param[in] data      Bytes to send.
 * @param[in] len       Byte count.
 * @param[in] send_stop true = STOP after payload; false = hold the bus.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok             Payload transmitted.
 * @retval k_ra_err_null_ptr   `data` NULL with `len` > 0 or bad channel.
 * @retval k_ra_err_nack       Peripheral NACKed.
 * @retval k_ra_err_hw_timeout TDRE / TEND poll timed out.
 *
 * @pre The channel was initialised via `ra_i2c_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_i2c_bus_bind_riic`.
 * @post On success the payload was transmitted per `send_stop`.
 * @post On failure the bus is released where the driver allows.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t
riic_write(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop)
{
  return ra_i2c_write((uint8_t)(uintptr_t)ctx, addr, data, len, send_stop);
}

/**
 * @brief RIIC backend: controller read.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra_i2c_read`, which always ends the transaction with STOP.
 *
 * @param[in]  ctx  Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  addr 7-bit peripheral address.
 * @param[out] data Destination buffer.
 * @param[in]  len  Byte count.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Bytes received.
 * @retval k_ra_err_null_ptr    `data` NULL or bad channel.
 * @retval k_ra_err_invalid_arg `len` is zero.
 * @retval k_ra_err_nack        Peripheral NACKed the address byte.
 *
 * @pre The channel was initialised via `ra_i2c_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_i2c_bus_bind_riic`.
 * @post On success `data` holds the received bytes.
 * @post STOP was issued and the bus released.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t riic_read(void* ctx, uint8_t addr, uint8_t* data, uint32_t len)
{
  return ra_i2c_read((uint8_t)(uintptr_t)ctx, addr, data, len);
}

/**
 * @brief RIIC backend: write-then-RESTART-then-read.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra_i2c_transfer`, which owns the repeated-START framing.
 *
 * @param[in]  ctx    Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  addr   7-bit peripheral address.
 * @param[in]  wr     Bytes to send first.
 * @param[in]  wr_len Number of bytes to send.
 * @param[out] rd     Destination buffer.
 * @param[in]  rd_len Number of bytes to read.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Both phases completed; STOP issued.
 * @retval k_ra_err_null_ptr    A required buffer was NULL or bad channel.
 * @retval k_ra_err_invalid_arg Both lengths zero.
 * @retval k_ra_err_nack        Peripheral NACKed.
 *
 * @pre The channel was initialised via `ra_i2c_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_i2c_bus_bind_riic`.
 * @post On success both phases completed and STOP was issued.
 * @post The bus is released regardless of outcome.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t riic_transfer(void*          ctx,
                              uint8_t        addr,
                              const uint8_t* wr,
                              uint32_t       wr_len,
                              uint8_t*       rd,
                              uint32_t       rd_len)
{
  return ra_i2c_transfer((uint8_t)(uintptr_t)ctx, addr, wr, wr_len, rd, rd_len);
}

/** @brief RIIC backend vtable -- every row forwards to `ra_i2c_*`. */
static const ra_io_i2c_bus_iface_t k_riic_iface = {
  .write    = riic_write,
  .read     = riic_read,
  .transfer = riic_transfer,
};

ra_err_t ra_io_i2c_bus_bind_riic(ra_io_i2c_bus_t* bus, uint8_t channel)
{
  RA_CHECK_NULL_PTR(bus, s_tag, "bus must not be nullptr");
  if (channel >= (uint8_t)k_ra_i2c_channel_count) {
    return k_ra_err_invalid_arg;
  }
  bus->iface = &k_riic_iface;
  bus->ctx   = (void*)(uintptr_t)channel;
  return k_ra_ok;
}
