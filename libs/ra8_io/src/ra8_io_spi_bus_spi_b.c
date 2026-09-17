/**
 * @file ra8_io_spi_bus_spi_b.c
 * @brief SPI_B backend for the ra8_io SPI-bus facade -- thin shim over ra8_spi.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_spi_bus_iface by forwarding to the SPI_B driver
 * (`ra8_spi.h`), unmodified. The bound channel index travels in the handle
 * context (`(void*)(uintptr_t)channel`), so the backend keeps no static
 * state and multiple bound buses coexist. Argument validation (channel
 * range, NULL buffers, divider limits) stays with the wrapped driver,
 * which also carries every HUM citation -- no raw MMIO is touched here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_spi_bus_spi_b.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_internal.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_spi_bus_spi_b";

/**
 * @brief SPI_B backend: full-duplex single-byte exchange.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra8_spi_xfer8`, which validates the channel and owns the SPSR polling.
 *
 * @param[in]  ctx Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  tx  Byte to transmit.
 * @param[out] rx  Receive slot; may be NULL to discard.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Frame exchanged.
 * @retval k_ra8_err_null_ptr   Channel out of range (driver reports it).
 * @retval k_ra8_err_hw_timeout SPSR.SPTEF / SPSR.SPRF never asserted.
 *
 * @pre The channel was initialised via `ra8_spi_init`.
 * @pre `ctx` carries a channel index bound by `ra8_io_spi_bus_bind_spi_b`.
 * @post On success one frame was clocked in both directions.
 * @post On failure the channel is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_spi_b_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  return ra8_spi_xfer8((uint8_t)(uintptr_t)ctx, tx, rx);
}

/**
 * @brief SPI_B backend: full-duplex multi-frame transfer.
 *
 * @details
 * Forwards straight to `ra8_spi_write_read`, which supports 8/16/32-bit
 * frames, treats a single NULL buffer as write-only / read-with-idle-fill,
 * and rejects both buffers NULL.
 *
 * @param[in]  ctx   Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  tx    Source buffer or NULL.
 * @param[out] rx    Destination buffer or NULL.
 * @param[in]  len   Number of frames.
 * @param[in]  width Per-frame bit width.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Every frame exchanged.
 * @retval k_ra8_err_null_ptr    Both buffers NULL with `len` > 0.
 * @retval k_ra8_err_invalid_arg Channel or `width` invalid.
 * @retval k_ra8_err_hw_timeout  SPSR polling expired.
 *
 * @pre The channel was initialised via `ra8_spi_init`.
 * @pre Buffer alignment matches `width`.
 * @post On success `len` frames were clocked in both directions.
 * @post On failure the channel is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_spi_b_write_read(void*               ctx,
                                                        const void*         tx,
                                                        void*               rx,
                                                        uint32_t            len,
                                                        ra8_spi_bit_width_t width)
{
  return ra8_spi_write_read((uint8_t)(uintptr_t)ctx, tx, rx, len, width);
}

/**
 * @brief SPI_B backend: re-program the SPBR bit-rate divider.
 *
 * @details
 * Forwards to `ra8_spi_set_clock`, which validates the channel and clock
 * arguments and reprograms SPCR3.SPBR without tearing the channel down.
 *
 * @param[in] ctx     Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in] baud_hz Target bit-rate in Hz.
 * @param[in] pclk_hz Current PCLKA frequency in Hz.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Divider reprogrammed.
 * @retval k_ra8_err_invalid_arg Channel or clock arguments invalid.
 *
 * @pre The channel was initialised via `ra8_spi_init`.
 * @pre `ctx` carries a channel index bound by `ra8_io_spi_bus_bind_spi_b`.
 * @post On success the channel's divider reflects the new rate.
 * @post On failure the divider is unchanged.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_spi_b_set_clock(void* ctx, uint32_t baud_hz, uint32_t pclk_hz)
{
  return ra8_spi_set_clock((uint8_t)(uintptr_t)ctx, baud_hz, pclk_hz);
}

/** @brief SPI_B backend vtable -- every row forwards to `ra8_spi_*`. */
static const ra8_io_spi_bus_iface_t s_spi_b_iface = {
  .xfer8      = internal_spi_b_xfer8,
  .write_read = internal_spi_b_write_read,
  .set_clock  = internal_spi_b_set_clock,
};

ra8_err_t ra8_io_spi_bus_bind_spi_b(ra8_io_spi_bus_t* bus, uint8_t channel)
{
  RA8_CHECK_NULL_PTR(bus, s_tag, "bus must not be nullptr");
  if (channel >= (uint8_t)k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  bus->iface = &s_spi_b_iface;
  bus->ctx   = (void*)(uintptr_t)channel;
  return k_ra8_ok;
}
