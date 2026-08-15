/**
 * @file ra8_sdmmc_spi_io.c
 * @brief SD card driver in SPI-mode -- public block I/O and SCI factory.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Block-level public API of the SPI-mode SD card driver declared in
 * ``ra8_sdmmc_spi.h``: init/deinit, single- and multi-block read/write,
 * erase, capacity/type queries, the ``ra8_fs`` backend adapter, and the
 * convenience SCI Simple-SPI transport factory (Pmod SD on EK-RA8D2).
 *
 * The wire-level protocol primitives (CRC, command framing, the card
 * identification sequence) live in the sibling translation unit
 * ``ra8_sdmmc_spi.c``; this unit reaches them through the module-private
 * ``ra8_sdmmc_spi_internal.h`` surface. All data-token, data-response, and
 * CRC behaviour follows SD Specification Part 1 Physical Layer Simplified
 * Specification v9.10 section 7 ("SPI Mode").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_sdmmc_spi_internal.h"
#include "ra8_sdmmc_spi_io_contracts.h"
#include "ra8_spi.h"

/* ---------------------------------------------------------------------------
 * Log tag
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_tag
 * @brief Log tag for diagnostics emitted by this module.
 * @note File-scope, read-only after init.
 * @since 0.1.0
 */
static const char* s_tag = "SDSPI";

/* ===========================================================================
 * Convenience SCI Simple-SPI transport factory (Pmod SD on EK-RA8D2)
 * ===========================================================================
 */

/**
 * @struct sci_bus_ctx_t
 * @brief Bus context handed to the factory's transport callbacks.
 * @details Populated once by ::ra8_sdmmc_spi_transport_sci and pointed at by
 * the returned transport's ``ctx`` cookie; recovers the SCI channel, PCLKA
 *          rate, and chip-select pin inside each shim. Not reentrant.
 * @since 0.1.0
 */
typedef struct {
  uint8_t        channel;  /**< SCI Simple-SPI channel index.      */
  uint32_t       pclka_hz; /**< PCLKA rate (Hz) for the baud shim. */
  ra8_port_pin_t cs;       /**< Chip-select GPIO pin (active-low). */
} sci_bus_ctx_t;

/**
 * @var s_sci_ctx
 * @brief Single-shot bus context backing the factory's transport callbacks.
 * @warning Mutated by ::ra8_sdmmc_spi_transport_sci; not thread-safe and only
 * one SD bus may be brought up through the factory at a time.
 * @since 0.1.0
 */
static sci_bus_ctx_t s_sci_ctx;

/* cppcheck-suppress constParameterCallback -- bound to
 * ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`;
 * constifying ctx would break the binding. */
/**
 * @brief Factory transport set-clock shim: retune the SCI baud divider.
 * @details Recovers the SCI channel + PCLKA from the bus context and forwards
 *          to ``ra8_sci_spi_set_clock``; carries no register access of its own.
 * @param[in] ctx Bus context (::sci_bus_ctx_t*); must be non-NULL.
 * @param[in] hz  Target SPI clock in Hz.
 * @return ra8_err_t passthrough from ``ra8_sci_spi_set_clock``.
 * @retval k_ra8_ok           Baud divider retuned.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_sci_ctx.
 * @pre ``ra8_sci_spi_init`` has configured the SCI channel.
 * @post On success the SCI channel clock is retuned to @p hz.
 * @post No bus transaction is issued (clock configuration only).
 * @note ISR-unsafe; blocking. `ctx` is non-const to match the transport
 *       function-pointer type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sci_transport_set_clock(void* ctx, uint32_t hz)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sci_bus_ctx_t* c = (const sci_bus_ctx_t*)ctx;
  return ra8_sci_spi_set_clock(c->channel, hz, c->pclka_hz);
}

/**
 * @brief Factory transport chip-select shim: drive CS low (asserted) or high.
 * @details Drives the chip-select GPIO from the bus context (active-low
 * select); carries no register access of its own.
 * @param[in] ctx      Bus context (::sci_bus_ctx_t*); must be non-NULL.
 * @param[in] asserted true to select (CS low), false to release (CS high).
 * @return ra8_err_t passthrough from ``ra8_gpio_write``.
 * @retval k_ra8_ok           CS driven to the requested level.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_sci_ctx.
 * @pre The CS pin was claimed as a GPIO output by
 * ::ra8_sdmmc_spi_transport_sci.
 * @post On success the CS pin reflects @p asserted.
 * @post No other pin or bus state changes.
 * @note ISR-unsafe. `ctx` is non-const to match the transport function-pointer
 *       type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sci_transport_cs(void* ctx, bool asserted)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sci_bus_ctx_t* c = (const sci_bus_ctx_t*)ctx;
  return ra8_gpio_write(c->cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/**
 * @brief Factory transport transfer shim: full-duplex byte exchange.
 * @details Forwards a full-duplex byte exchange to the SCI channel from the bus
 *          context; carries no register access of its own.
 * @param[in]  ctx Bus context (::sci_bus_ctx_t*); must be non-NULL.
 * @param[in]  tx  TX bytes, or NULL to shift idle 0xFF.
 * @param[out] rx  RX buffer, or NULL to discard.
 * @param[in]  len Byte count; must be > 0.
 * @return ra8_err_t passthrough from ``ra8_sci_spi_xfer``.
 * @retval k_ra8_ok           @p len bytes clocked on the bus.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @pre @p ctx points to the live ::s_sci_ctx.
 * @pre ``ra8_sci_spi_init`` configured the channel and CS is asserted.
 * @post On success @p rx holds the received bytes when non-NULL.
 * @post No state beyond @p rx and the SCI data registers is modified.
 * @note ISR-unsafe; blocking. `ctx` is non-const to match the transport
 *       function-pointer type (constParameterCallback suppressed in-tree).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_sci_transport_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const sci_bus_ctx_t* c = (const sci_bus_ctx_t*)ctx;
  return ra8_sci_spi_xfer(c->channel, tx, rx, len);
}

/**
 * @brief Route the four Pmod SPI pins and bring up the SCI Simple-SPI channel.
 * @details Muxes SCK/CIPO/COPI to the SCI async function, claims CS as a GPIO
 *          output held high, then initialises the SCI channel at the SD
 * power-on clock. All HAL calls are reused (no register access here).
 * @param[in] channel  SCI channel index (0..9).
 * @param[in] pclk_hz  PCLKA rate (Hz) feeding the baud divider; non-zero.
 * @param[in] pins     Non-NULL SCK / CIPO / COPI / CS descriptor.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Pins routed, CS claimed high, SCI channel up.
 * @retval k_ra8_err_null_ptr @p pins is NULL.
 * @retval other             Propagated from ``ra8_pfs_route_peripheral`` /
 *                           ``ra8_gpio_output_init`` / ``ra8_sci_spi_init``.
 * @pre ``ra8_cgc_init`` has run; @p pclk_hz is the live PCLKA rate.
 * @pre The four @p pins are free (not routed to another peripheral).
 * @post On success the four pins are muxed and the SCI channel is configured.
 * @post On failure the affected pins are left in their prior state.
 * @note CS is a plain GPIO output (SD SPI-mode holds CS across a frame).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sci_transport_bringup(uint8_t  channel,
                                                             uint32_t pclk_hz,
                                                             const ra8_sdmmc_spi_sci_pins_t* pins)
{
  RA8_CHECK_NULL_PTR(pins, s_tag, "pins");

  ra8_err_t err = ra8_pfs_route_peripheral(pins->sck, k_ra8_psel_sci_async, "sdspi.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(pins->cipo, k_ra8_psel_sci_async, "sdspi.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(pins->copi, k_ra8_psel_sci_async, "sdspi.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_output_init(pins->cs, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
    .pclk_hz   = pclk_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  return ra8_sci_spi_init(channel, &spi_cfg);
}

ra8_err_t ra8_sdmmc_spi_transport_sci(uint8_t                         sci_channel,
                                      uint32_t                        pclk_hz,
                                      const ra8_sdmmc_spi_sci_pins_t* pins,
                                      ra8_sdmmc_spi_transport_t*      out)
{
  RA8_CHECK_NULL_PTR(pins, s_tag, "pins");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  if (pclk_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }

  ra8_err_t err = internal_sci_transport_bringup(sci_channel, pclk_hz, pins);
  if (err != k_ra8_ok) {
    return err;
  }

  s_sci_ctx.channel  = sci_channel;
  s_sci_ctx.pclka_hz = pclk_hz;
  s_sci_ctx.cs       = pins->cs;

  out->set_clock = internal_sci_transport_set_clock;
  out->cs        = internal_sci_transport_cs;
  out->xfer      = internal_sci_transport_xfer;
  out->ctx       = &s_sci_ctx;
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

/** @copydoc internal_prepare_init */
RA8_INTERNAL static ra8_err_t internal_prepare_init(const ra8_sdmmc_spi_transport_t* transport)
{
  if (g_sdmmc_spi_state.initialized) {
    ra8_log_error(s_tag, "already initialized");
    return k_ra8_err_invalid_state;
  }
  g_sdmmc_spi_state.transport       = *transport;
  g_sdmmc_spi_state.card_type       = k_ra8_sdmmc_spi_type_unknown;
  g_sdmmc_spi_state.capacity_blocks = 0U;
  return g_sdmmc_spi_state.transport.set_clock(g_sdmmc_spi_state.transport.ctx,
                                               (uint32_t)k_ra8_sdmmc_spi_clock_init_hz);
}

/** @copydoc internal_finalize_init */
RA8_INTERNAL static ra8_err_t internal_finalize_init(void)
{
  ra8_err_t err = g_sdmmc_spi_state.transport.set_clock(g_sdmmc_spi_state.transport.ctx,
                                                        (uint32_t)k_ra8_sdmmc_spi_clock_data_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  g_sdmmc_spi_state.initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_sdmmc_spi_init(const ra8_sdmmc_spi_transport_t* transport)
{
  ra8_err_t err = priv_sdmmc_spi_validate_transport(transport);
  RA8_RETURN_ON_ERROR(err, s_tag, "transport invalid");
  err = internal_prepare_init(transport);
  RA8_RETURN_ON_ERROR(err, s_tag, "prepare init");
  err = priv_sdmmc_spi_run_init_sequence();
  RA8_RETURN_ON_ERROR(err, s_tag, "SD init sequence");
  err = internal_finalize_init();
  RA8_RETURN_ON_ERROR(err, s_tag, "finalize init");
  return k_ra8_ok;
}

ra8_err_t ra8_sdmmc_spi_deinit(void)
{
  g_sdmmc_spi_state.initialized     = false;
  g_sdmmc_spi_state.card_type       = k_ra8_sdmmc_spi_type_unknown;
  g_sdmmc_spi_state.capacity_blocks = 0U;
  return k_ra8_ok;
}

/** @copydoc internal_lba_to_arg */
RA8_INTERNAL static uint32_t internal_lba_to_arg(uint32_t lba)
{
  if (g_sdmmc_spi_state.card_type == k_ra8_sdmmc_spi_type_sdhc) {
    return lba;
  }
  return lba * (uint32_t)k_ra8_sdmmc_spi_block_size;
}

/** @copydoc internal_cmd_require_ready */
RA8_INTERNAL static ra8_err_t internal_cmd_require_ready(sd_cmd_t cmd, uint32_t arg)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = priv_sdmmc_spi_send_command(cmd, arg, &r1);
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  if (r1 != 0U) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/** @copydoc internal_erase_range */
RA8_INTERNAL static ra8_err_t internal_erase_range(uint32_t lba, uint32_t count)
{
  ra8_err_t err = internal_cmd_require_ready(k_sd_cmd_erase_wr_blk_start, internal_lba_to_arg(lba));
  if (err != k_ra8_ok) {
    return err;
  }
  err =
    internal_cmd_require_ready(k_sd_cmd_erase_wr_blk_end, internal_lba_to_arg((lba + count) - 1U));
  if (err != k_ra8_ok) {
    return err;
  }
  /* CMD38 then a long busy wait -- a bulk erase can hold the card busy for
   * seconds, far longer than a single-block write. */
  err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = priv_sdmmc_spi_send_command(k_sd_cmd_erase, 0U, &r1);
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  if (r1 != 0U) {
    (void)priv_sdmmc_spi_cs_release();
    return k_ra8_err_protocol_error;
  }
  err = priv_sdmmc_spi_wait_not_busy_bounded((uint32_t)k_sd_max_erase_poll_bytes);
  (void)priv_sdmmc_spi_cs_release();
  return err;
}

/** @copydoc internal_read_block_payload */
RA8_INTERNAL static ra8_err_t internal_read_block_payload(uint8_t* buf)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    ra8_err_t err = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &buf[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/** @copydoc internal_read_block_crc_check */
RA8_INTERNAL static ra8_err_t internal_read_block_crc_check(const uint8_t* buf)
{
  uint8_t crc_hi = 0U;
  uint8_t crc_lo = 0U;
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &crc_hi);
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &crc_lo);
  const uint16_t expected = ra8_sdmmc_spi_crc16(buf, (uint32_t)k_ra8_sdmmc_spi_block_size);
  const uint16_t actual   = (uint16_t)(((uint16_t)crc_hi << k_sd_bit_byte) | (uint16_t)crc_lo);
  if (expected != actual) {
    return k_ra8_err_crc_mismatch;
  }
  return k_ra8_ok;
}

/** @copydoc internal_read_data_phase */
RA8_INTERNAL static ra8_err_t internal_read_data_phase(uint32_t lba, uint8_t* buf)
{
  uint8_t   r1 = 0U;
  ra8_err_t err =
    priv_sdmmc_spi_send_command(k_sd_cmd_read_single_block, internal_lba_to_arg(lba), &r1);
  if (err != k_ra8_ok) {
    return err;
  }
  if (r1 != 0U) {
    return k_ra8_err_protocol_error;
  }
  err = priv_sdmmc_spi_wait_data_token();
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_read_block_payload(buf);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_read_block_crc_check(buf);
}

ra8_err_t ra8_sdmmc_spi_read_block(uint32_t lba, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (lba >= g_sdmmc_spi_state.capacity_blocks) {
    return k_ra8_err_out_of_range;
  }
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  RA8_RETURN_ON_ERROR(err, s_tag, "cs assert");
  err = internal_read_data_phase(lba, buf);
  (void)priv_sdmmc_spi_cs_release();
  return err;
}

/* Drain @p count streamed blocks (token + payload + CRC) of an open CMD18 --
 * see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_read_multi_stream(uint8_t* buf, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    uint8_t*  block = &buf[(size_t)i * (size_t)k_ra8_sdmmc_spi_block_size];
    ra8_err_t err   = priv_sdmmc_spi_wait_data_token();
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_read_block_payload(block);
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_read_block_crc_check(block);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/** @copydoc internal_read_multi_stop */
RA8_INTERNAL static ra8_err_t internal_read_multi_stop(void)
{
  uint8_t   r1  = 0U;
  ra8_err_t err = priv_sdmmc_spi_send_stop_transmission(&r1);
  if (err != k_ra8_ok) {
    return err;
  }
  if (r1 != 0U) {
    (void)priv_sdmmc_spi_wait_not_busy();
    return k_ra8_err_protocol_error;
  }
  return priv_sdmmc_spi_wait_not_busy();
}

ra8_err_t ra8_sdmmc_spi_read_blocks(uint32_t lba, uint8_t* buf, uint32_t count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (count == 0U) {
    return k_ra8_ok;
  }
  if ((lba >= g_sdmmc_spi_state.capacity_blocks) ||
      (count > (g_sdmmc_spi_state.capacity_blocks - lba))) {
    return k_ra8_err_out_of_range;
  }
  if (count == 1U) {
    return ra8_sdmmc_spi_read_block(lba, buf);
  }
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  RA8_RETURN_ON_ERROR(err, s_tag, "cs assert");
  uint8_t r1 = 0U;
  err = priv_sdmmc_spi_send_command(k_sd_cmd_read_multi_block, internal_lba_to_arg(lba), &r1);
  if ((err != k_ra8_ok) || (r1 != 0U)) {
    (void)priv_sdmmc_spi_cs_release();
    return (err != k_ra8_ok) ? err : k_ra8_err_protocol_error;
  }
  err = internal_read_multi_stream(buf, count);
  /* CMD12 runs on the success AND the abort path: the card keeps streaming
   * read data until it sees STOP_TRANSMISSION, so it must leave the data
   * state before CS is released or the next command collides with the
   * still-open stream. A stream error takes precedence over a stop error. */
  const ra8_err_t stop_err = internal_read_multi_stop();
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  return stop_err;
}

/** @copydoc internal_write_data_block */
RA8_INTERNAL static ra8_err_t internal_write_data_block(const uint8_t* buf, uint8_t start_token)
{
  ra8_err_t err = priv_sdmmc_spi_send_idle(1U); /* N_WR pad (spec >= 1 byte). */
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_sdmmc_spi_xfer_one(start_token, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  err = g_sdmmc_spi_state.transport.xfer(g_sdmmc_spi_state.transport.ctx,
                                         buf,
                                         nullptr,
                                         (uint32_t)k_ra8_sdmmc_spi_block_size);
  if (err != k_ra8_ok) {
    /* Some transports require non-NULL rx -- fall back to per-byte. */
    for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
      uint8_t dummy = 0U;
      err           = priv_sdmmc_spi_xfer_one(buf[i], &dummy);
      if (err != k_ra8_ok) {
        return err;
      }
    }
  }
  const uint16_t crc = ra8_sdmmc_spi_crc16(buf, (uint32_t)k_ra8_sdmmc_spi_block_size);
  (void)priv_sdmmc_spi_xfer_one(
    (uint8_t)((uint32_t)(crc >> k_sd_bit_byte) & (uint32_t)k_sd_mask_byte),
    nullptr);
  (void)priv_sdmmc_spi_xfer_one((uint8_t)((uint32_t)crc & (uint32_t)k_sd_mask_byte), nullptr);
  uint8_t response = 0U;
  err              = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &response);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((response & (uint8_t)k_sd_data_response_mask) != (uint8_t)k_sd_data_response_accepted) {
    (void)priv_sdmmc_spi_wait_not_busy();
    return k_ra8_err_protocol_error;
  }
  return priv_sdmmc_spi_wait_not_busy();
}

ra8_err_t ra8_sdmmc_spi_write_block(uint32_t lba, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (lba >= g_sdmmc_spi_state.capacity_blocks) {
    return k_ra8_err_out_of_range;
  }
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  RA8_RETURN_ON_ERROR(err, s_tag, "cs assert");
  uint8_t r1 = 0U;
  err = priv_sdmmc_spi_send_command(k_sd_cmd_write_single_block, internal_lba_to_arg(lba), &r1);
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  if (r1 != 0U) {
    (void)priv_sdmmc_spi_cs_release();
    return k_ra8_err_protocol_error;
  }
  err = internal_write_data_block(buf, (uint8_t)k_sd_token_data_start_single);
  (void)priv_sdmmc_spi_cs_release();
  return err;
}

/** @copydoc internal_write_multi_stream */
RA8_INTERNAL static ra8_err_t internal_write_multi_stream(const uint8_t* buf, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    const ra8_err_t err =
      internal_write_data_block(&buf[(size_t)i * (size_t)k_ra8_sdmmc_spi_block_size],
                                (uint8_t)k_sd_token_data_start_multi);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  (void)priv_sdmmc_spi_send_idle(1U);
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_stop_multi, nullptr);
  (void)priv_sdmmc_spi_send_idle(1U);
  return priv_sdmmc_spi_wait_not_busy();
}

/**
 * @brief Write @p count contiguous 512-byte blocks with one CMD25 transaction.
 *
 * @details The fast bulk-write path: an optional ACMD23 pre-erase hint, then a
 * single WRITE_MULTIPLE_BLOCK (CMD25) streaming every block with the
 * multi-block data-start token (0xFC), terminated by the stop-tran token
 * (0xFD). One command for the whole run instead of @p count single-block CMD24
 * writes -- the SD spec fast path for clearing a large region (e.g. a multi-MB
 * FAT during format).
 *
 * @param[in] lba   First logical block address.
 * @param[in] buf   Source buffer of @p count * 512 bytes.
 * @param[in] count Number of contiguous blocks (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All @p count blocks were accepted and programmed.
 * @retval k_ra8_err_null_ptr      @p buf is NULL.
 * @retval k_ra8_err_invalid_state The driver is not initialised.
 * @retval k_ra8_err_out_of_range  @p lba + @p count exceeds the card capacity.
 * @retval k_ra8_err_protocol_error A command R1 or data-response token
 * rejected.
 *
 * @pre The driver is initialised and a card is present.
 * @pre @p buf holds at least @p count * 512 bytes.
 * @post The stop token has been sent and the card is no longer busy.
 * @post CS is released on every return path.
 *
 * @note Not thread-safe; serialise card access. ACMD23 is best-effort -- a card
 *       that rejects it still gets a correct (if unpre-erased) CMD25 stream.
 * @since 0.1.0
 */
ra8_err_t ra8_sdmmc_spi_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (count == 0U) {
    return k_ra8_ok;
  }
  if ((lba >= g_sdmmc_spi_state.capacity_blocks) ||
      (count > (g_sdmmc_spi_state.capacity_blocks - lba))) {
    return k_ra8_err_out_of_range;
  }
  if (count == 1U) {
    return ra8_sdmmc_spi_write_block(lba, buf);
  }
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  RA8_RETURN_ON_ERROR(err, s_tag, "cs assert");
  /* Pre-erase hint (ACMD23): best-effort -- ignore the response so a card that
   * does not implement it still streams correctly below. */
  uint8_t acmd_r1 = 0U;
  (void)priv_sdmmc_spi_send_acmd(k_sd_acmd_set_wr_blk_erase_count, count, &acmd_r1);
  uint8_t r1 = 0U;
  err = priv_sdmmc_spi_send_command(k_sd_cmd_write_multi_block, internal_lba_to_arg(lba), &r1);
  if ((err != k_ra8_ok) || (r1 != 0U)) {
    (void)priv_sdmmc_spi_cs_release();
    return (err != k_ra8_ok) ? err : k_ra8_err_protocol_error;
  }
  err = internal_write_multi_stream(buf, count);
  (void)priv_sdmmc_spi_cs_release();
  return err;
}

ra8_err_t ra8_sdmmc_spi_erase_blocks(uint32_t lba, uint32_t count)
{
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((lba >= g_sdmmc_spi_state.capacity_blocks) ||
      (count > (g_sdmmc_spi_state.capacity_blocks - lba))) {
    return k_ra8_err_out_of_range;
  }
  /* Probe: erase only the FIRST block and read it back. The SD post-erase value
   * is card-dependent (0x00 on some, 0xFF on others), and the SCR
   * DATA_STAT_AFTER_ERASE bit's polarity is unreliable in practice, so measure
   * it. A non-zero read-back means this card erases to ones: report "not
   * supported" so the caller writes zeros -- and skip erasing the rest, which
   * would be wasted work (a one-block probe instead of the whole region). */
  ra8_err_t err = internal_erase_range(lba, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t blk[k_ra8_sdmmc_spi_block_size] = {};
  err                                     = ra8_sdmmc_spi_read_block(lba, blk);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_block_size; i++) {
    if (blk[i] != 0U) {
      return k_ra8_err_not_supported;
    }
  }
  /* The card erases to zero: erase the remaining range in one operation. */
  if (count > 1U) {
    err = internal_erase_range(lba + 1U, count - 1U);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sdmmc_spi_get_capacity(uint32_t* out_blocks)
{
  RA8_CHECK_NULL_PTR(out_blocks, s_tag, "out_blocks is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_blocks = g_sdmmc_spi_state.capacity_blocks;
  return k_ra8_ok;
}

ra8_err_t ra8_sdmmc_spi_get_card_type(ra8_sdmmc_spi_card_type_t* out_type)
{
  RA8_CHECK_NULL_PTR(out_type, s_tag, "out_type is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_type = g_sdmmc_spi_state.card_type;
  return k_ra8_ok;
}

/* ===========================================================================
 * ra8_fs backend adapter
 * ===========================================================================
 */

/** @copydoc internal_fs_read_block */
RA8_INTERNAL static ra8_err_t
internal_fs_read_block(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  /* SD block numbers are 32-bit (SDXC tops out at 2 TiB), so an address past
   * that reach is refused rather than truncated -- the 64-bit `ra8_fs`
   * backend interface (#683) simply exceeds what this medium can hold. */
  if (lba > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range;
  }
  /* One CMD18 multi-block transaction for the whole run (fast); the
   * single-block path is used only for a lone block. */
  return ra8_sdmmc_spi_read_blocks((uint32_t)lba, buf, count);
}

/** @copydoc internal_fs_write_block */
RA8_INTERNAL static ra8_err_t
internal_fs_write_block(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (lba > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range; /* SD block numbers are 32-bit; see the read
                                      shim */
  }
  /* One CMD25 multi-block transaction for the whole run (fast); the
   * single-block path is used only for a lone block. */
  return ra8_sdmmc_spi_write_blocks((uint32_t)lba, buf, count);
}

/** @copydoc internal_fs_erase_block */
RA8_INTERNAL static ra8_err_t internal_fs_erase_block(void* ctx, uint64_t lba, uint64_t count)
{
  (void)ctx;
  if ((lba > (uint64_t)UINT32_MAX) || (count > (uint64_t)UINT32_MAX)) {
    return k_ra8_err_out_of_range; /* SD block numbers are 32-bit; see the read
                                      shim */
  }
  return ra8_sdmmc_spi_erase_blocks((uint32_t)lba, (uint32_t)count);
}

/** @copydoc internal_fs_get_capacity */
RA8_INTERNAL static ra8_err_t
internal_fs_get_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == nullptr) || (block_size == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *block_count = g_sdmmc_spi_state.capacity_blocks;
  *block_size  = (uint32_t)k_ra8_sdmmc_spi_block_size;
  return k_ra8_ok;
}

ra8_err_t ra8_sdmmc_spi_bind_fs_backend(ra8_fs_backend_t* out_backend)
{
  RA8_CHECK_NULL_PTR(out_backend, s_tag, "out_backend is null");
  if (!g_sdmmc_spi_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  out_backend->read_block   = internal_fs_read_block;
  out_backend->write_block  = internal_fs_write_block;
  out_backend->get_capacity = internal_fs_get_capacity;
  out_backend->erase_blocks = internal_fs_erase_block;
  out_backend->ctx          = nullptr;
  return k_ra8_ok;
}
