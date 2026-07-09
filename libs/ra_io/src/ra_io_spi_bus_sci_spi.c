/**
 * @file ra_io_spi_bus_sci_spi.c
 * @brief SCI Simple-SPI backend for the ra_io SPI-bus facade -- thin shim
 *        over ra_sci_spi.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra_io_spi_bus_iface by forwarding to the SCI Simple-SPI
 * driver (`ra_sci_spi.h`), unmodified. The bound channel index travels in
 * the handle context (`(void*)(uintptr_t)channel`), so the backend keeps
 * no static state and multiple bound buses coexist. The SCI block clocks
 * 8-bit frames only, so the multi-frame row rejects wider widths with
 * ::k_ra_err_not_supported instead of silently mis-framing. No raw MMIO is
 * touched here; the driver carries every HUM citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_spi_bus_sci_spi.h"

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_spi_bus.h"
#include "ra_io_spi_bus_internal.h"
#include "ra_sci_spi.h"
#include "ra_spi.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_spi_bus_sci_spi";

/**
 * @brief SCI backend: full-duplex single-byte exchange.
 *
 * @details
 * Decodes the channel from the handle context and forwards to
 * `ra_sci_spi_xfer8`, which validates the channel and owns the CSR
 * polling.
 *
 * @param[in]  ctx Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  tx  Byte to transmit.
 * @param[out] rx  Receive slot; may be NULL to discard.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame exchanged.
 * @retval k_ra_err_null_ptr    Channel out of range (driver reports it).
 * @retval k_ra_err_hw_timeout  The TX-empty or RX-full poll expired.
 *
 * @pre The channel was initialised via `ra_sci_spi_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_spi_bus_bind_sci_spi`.
 * @post On success one frame was clocked in both directions.
 * @post On failure the channel is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t sci_spi_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  return ra_sci_spi_xfer8((uint8_t)(uintptr_t)ctx, tx, rx);
}

/**
 * @brief SCI backend: full-duplex multi-byte transfer (8-bit frames only).
 *
 * @details
 * The SCI Simple-SPI data register clocks one byte per frame, so any
 * `width` other than ::k_ra_spi_width_8 is rejected up-front with
 * ::k_ra_err_not_supported. Valid requests forward to `ra_sci_spi_xfer`,
 * which treats a NULL `tx` as idle (0xFF) fill and a NULL `rx` as discard.
 *
 * @param[in]  ctx   Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in]  tx    Source bytes or NULL for idle fill.
 * @param[out] rx    Destination bytes or NULL to discard.
 * @param[in]  len   Number of bytes.
 * @param[in]  width Per-frame bit width; only ::k_ra_spi_width_8.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Every byte exchanged.
 * @retval k_ra_err_not_supported `width` is not 8-bit.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 * @retval k_ra_err_hw_timeout   A per-frame poll expired.
 *
 * @pre The channel was initialised via `ra_sci_spi_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_spi_bus_bind_sci_spi`.
 * @post On success `len` frames were clocked.
 * @post On failure the channel is left as the driver leaves it.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t
sci_spi_write_read(void* ctx, const void* tx, void* rx, uint32_t len, ra_spi_bit_width_t width)
{
  if (width != k_ra_spi_width_8) {
    return k_ra_err_not_supported;
  }
  return ra_sci_spi_xfer((uint8_t)(uintptr_t)ctx, (const uint8_t*)tx, (uint8_t*)rx, len);
}

/**
 * @brief SCI backend: re-program the CCR2 bit-rate divider.
 *
 * @details
 * Forwards to `ra_sci_spi_set_clock`, which validates the arguments and
 * picks the smallest CKS keeping BRR in range.
 *
 * @param[in] ctx     Bound channel index, encoded as `(void*)(uintptr_t)`.
 * @param[in] baud_hz Target bit-rate in Hz.
 * @param[in] pclk_hz SCI baud-clock source (PCLKA) in Hz.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Divider reprogrammed.
 * @retval k_ra_err_invalid_arg Channel out of range or `baud_hz` zero.
 *
 * @pre The channel was initialised via `ra_sci_spi_init`.
 * @pre `ctx` carries a channel index bound by `ra_io_spi_bus_bind_sci_spi`.
 * @post On success CCR2.CKS/BRR reflect the new rate.
 * @post On failure the divider is unchanged.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
static ra_err_t sci_spi_set_clock(void* ctx, uint32_t baud_hz, uint32_t pclk_hz)
{
  return ra_sci_spi_set_clock((uint8_t)(uintptr_t)ctx, baud_hz, pclk_hz);
}

/** @brief SCI Simple-SPI backend vtable -- every row forwards to `ra_sci_spi_*`. */
static const ra_io_spi_bus_iface_t k_sci_spi_iface = {
  .xfer8      = sci_spi_xfer8,
  .write_read = sci_spi_write_read,
  .set_clock  = sci_spi_set_clock,
};

ra_err_t ra_io_spi_bus_bind_sci_spi(ra_io_spi_bus_t* bus, uint8_t channel)
{
  RA_CHECK_NULL_PTR(bus, s_tag, "bus must not be nullptr");
  if (channel >= (uint8_t)k_ra_sci_channel_count) {
    return k_ra_err_invalid_arg;
  }
  bus->iface = &k_sci_spi_iface;
  bus->ctx   = (void*)(uintptr_t)channel;
  return k_ra_ok;
}
