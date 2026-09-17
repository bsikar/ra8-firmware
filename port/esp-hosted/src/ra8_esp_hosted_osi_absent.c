/**
 * @file port/esp-hosted/src/ra8_esp_hosted_osi_absent.c
 * @brief Vtable rows for transports this board does not carry.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Sixteen of the seventy-two OS-abstraction rows belong to the SDIO,
 * half-duplex SPI and UART transports. On this board none of those three
 * exists: the ESP32-C6 is reached over full-duplex SPI and nothing else.
 * The co-processor image is built with `CONFIG_ESP_SPI_HOST_INTERFACE=y`
 * (``coprocessor/esp32c6/sdkconfig.defaults``), the harness carries a
 * four-wire SPI bus plus two side-band signals
 * (``coprocessor/esp32c6/pins.env``), and the decision is recorded in
 * ``docs/design/c6_wireless_architecture.md``.
 *
 * @par These are answers, not placeholders
 * "That transport is not present on this board" is the truth, and it is
 * the whole truth: no implementation of these rows could ever move a byte,
 * because there is no wire for them to move it on. So each row here
 * validates its own arguments -- a caller passing a null buffer has made a
 * different mistake and deserves a different answer -- and then reports
 * the absence, naming the row it was called through so the log identifies
 * the caller without a debugger.
 *
 * The alternative, leaving the rows null, is strictly worse: the same
 * mistake becomes a null-pointer fault with no diagnosis, at whatever
 * moment the vendored core first reaches for a transport that was never
 * configured.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "esp_log.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_log.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_attributes.h"
#include "ra8_esp_hosted_osi_internal.h"

DEFINE_LOG_TAG(absent);

/**
 * @brief Report a call into a transport this board does not carry.
 *
 * @details
 * The single place the absence is reported, so every row says the same
 * thing and names itself. Kept separate from the rows so the message
 * cannot drift between them.
 *
 * @param[in] slot Name of the vtable row that was called. Every call site in
 *                 this file passes a string literal, so it is never null; the
 *                 log bridge would render a null as ``(null)`` anyway, which
 *                 is why no guard is spent on a case that cannot arise.
 *
 * @return Always the failure code.
 * @retval RET_FAIL The transport is not present on this board.
 *
 * @pre The caller is a vtable row for a transport other than full-duplex
 *      SPI.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No state is modified.
 * @post Exactly one line is emitted per call.
 *
 * @note Reentrant; formats onto the logger's own bounded stack line.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_absent(const char* slot)
{
  ESP_LOGE(s_esp_hosted_tag, "%s: transport not present on this board", slot);
  return RET_FAIL;
}

/**
 * @brief Vtable row: bring an SDIO card up.
 *
 * @details
 * There is no SDIO connection to the co-processor on this board; see the
 * file header.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] show_config Whether the caller wanted the configuration
 *                        logged.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` was null.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No hardware is touched.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_sdio_card_init(void* ctx, bool show_config)
{
  (void)show_config;
  if (ctx == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_card_init");
}

/**
 * @brief Vtable row: shut an SDIO card down.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` was null.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No hardware is touched.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_sdio_card_deinit(void* ctx)
{
  if (ctx == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_card_deinit");
}

/**
 * @brief Vtable row: read an SDIO peripheral register.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] reg Register offset the caller wanted.
 * @param[out] data Destination buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not written.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
internal_sdio_read_reg(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required)
{
  (void)reg;
  (void)lock_required;
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_read_reg");
}

/**
 * @brief Vtable row: write an SDIO peripheral register.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] reg Register offset the caller wanted.
 * @param[in] data Source buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not read.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
internal_sdio_write_reg(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required)
{
  (void)reg;
  (void)lock_required;
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_write_reg");
}

/**
 * @brief Vtable row: read an SDIO data block.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] reg Register offset the caller wanted.
 * @param[out] data Destination buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not written.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
internal_sdio_read_block(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required)
{
  (void)reg;
  (void)lock_required;
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_read_block");
}

/**
 * @brief Vtable row: write an SDIO data block.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] reg Register offset the caller wanted.
 * @param[in] data Source buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not read.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
internal_sdio_write_block(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required)
{
  (void)reg;
  (void)lock_required;
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_write_block");
}

/**
 * @brief Vtable row: wait for an SDIO peripheral interrupt.
 *
 * @details See the file header; no SDIO transport exists here.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] ticks_to_wait How long the caller was prepared to wait.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` was null.
 * @retval RET_FAIL No SDIO transport exists on this board.
 *
 * @pre The caller selected the SDIO transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post The caller is not blocked; the answer is immediate.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_sdio_wait_intr(void* ctx, uint32_t ticks_to_wait)
{
  (void)ticks_to_wait;
  if (ctx == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_sdio_wait_slave_intr"); /* LEGACY-OK: upstream row name */
}

/**
 * @brief Vtable row: read a half-duplex SPI register.
 *
 * @details See the file header; the link is full-duplex SPI, not
 * half-duplex, and the two are different bus configurations rather than
 * two modes of one.
 *
 * @param[in] reg Register offset the caller wanted.
 * @param[out] data Destination word.
 * @param[in] poll Whether the caller wanted the read polled.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``data`` was null.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not written.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_spi_hd_read_reg(uint32_t reg, uint32_t* data, int poll, bool lock_required)
{
  (void)reg;
  (void)poll;
  (void)lock_required;
  if (data == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_spi_hd_read_reg");
}

/**
 * @brief Vtable row: write a half-duplex SPI register.
 *
 * @details See the file header; no half-duplex SPI transport exists here.
 *
 * @param[in] reg Register offset the caller wanted.
 * @param[in] data Source word.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``data`` was null.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not read.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_spi_hd_write_reg(uint32_t reg, uint32_t* data, bool lock_required)
{
  (void)reg;
  (void)lock_required;
  if (data == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_spi_hd_write_reg");
}

/**
 * @brief Vtable row: read a half-duplex SPI data burst.
 *
 * @details See the file header; no half-duplex SPI transport exists here.
 *
 * @param[out] data Destination buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``data`` was null or ``size`` was zero.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not written.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_spi_hd_read_dma(uint8_t* data, uint16_t size, bool lock_required)
{
  (void)lock_required;
  if ((data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_spi_hd_read_dma");
}

/**
 * @brief Vtable row: write a half-duplex SPI data burst.
 *
 * @details See the file header; no half-duplex SPI transport exists here.
 *
 * @param[in] data Source buffer.
 * @param[in] size Bytes the caller wanted.
 * @param[in] lock_required Whether the caller wanted the bus lock taken.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``data`` was null or ``size`` was zero.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not read.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_spi_hd_write_dma(uint8_t* data, uint16_t size, bool lock_required)
{
  (void)lock_required;
  if ((data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_spi_hd_write_dma");
}

/**
 * @brief Vtable row: choose how many half-duplex SPI data lines to use.
 *
 * @details See the file header; no half-duplex SPI transport exists here.
 *
 * @param[in] data_lines Line count the caller wanted. The half-duplex
 *                        transport defines exactly three legal widths, named
 *                        by ``H_SPI_HD_CONFIG_{1_DATA_LINE,2_DATA_LINES,
 *                        4_DATA_LINES}``; anything else is a malformed
 *                        request rather than an unsupported one, and is
 *                        reported as such.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``data_lines`` was not one of the three legal widths.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No bus configuration is changed.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_spi_hd_set_data_lines(uint32_t data_lines)
{
  if ((data_lines != (uint32_t)H_SPI_HD_CONFIG_1_DATA_LINE) &&
      (data_lines != (uint32_t)H_SPI_HD_CONFIG_2_DATA_LINES) &&
      (data_lines != (uint32_t)H_SPI_HD_CONFIG_4_DATA_LINES)) {
    return RET_INVALID;
  }
  return internal_absent("_h_spi_hd_set_data_lines");
}

/**
 * @brief Vtable row: issue the half-duplex SPI command-nine sequence.
 *
 * @details
 * See the file header; no half-duplex SPI transport exists here. The row
 * takes no parameters, so its only validation is of the module state --
 * which is exactly the condition it reports.
 *
 * @return Failure, always.
 * @retval RET_FAIL No half-duplex SPI transport exists on this board.
 *
 * @pre The caller selected the half-duplex SPI transport, which this build
 *      does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No bus activity occurs.
 * @post Exactly one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_spi_hd_send_cmd9(void)
{
  return internal_absent("_h_spi_hd_send_cmd9");
}

/**
 * @brief Vtable row: read from the UART transport.
 *
 * @details See the file header; no UART link to the co-processor exists.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[out] data Destination buffer.
 * @param[in] size Bytes the caller wanted.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No UART transport exists on this board.
 *
 * @pre The caller selected the UART transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not written.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_uart_read(void* ctx, uint8_t* data, uint16_t size)
{
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_uart_read");
}

/**
 * @brief Vtable row: write to the UART transport.
 *
 * @details See the file header; no UART link to the co-processor exists.
 *
 * @param[in] ctx Transport context supplied by the caller.
 * @param[in] data Source buffer.
 * @param[in] size Bytes the caller wanted.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` or ``data`` was null, or ``size`` was zero.
 * @retval RET_FAIL No UART transport exists on this board.
 *
 * @pre The caller selected the UART transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post ``data`` is not read.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
/* NOLINTNEXTLINE(readability-non-const-parameter) -- hosted_osi_funcs_t row is non-const. */
static int internal_uart_write(void* ctx, uint8_t* data, uint16_t size)
{
  if ((ctx == nullptr) || (data == nullptr) || (size == 0U)) {
    return RET_INVALID;
  }
  return internal_absent("_h_uart_write");
}

/**
 * @brief Vtable row: discard whatever the UART transport has buffered.
 *
 * @details See the file header; no UART link to the co-processor exists.
 *
 * @param[in] ctx Transport context supplied by the caller.
 *
 * @return Failure, always.
 * @retval RET_INVALID ``ctx`` was null.
 * @retval RET_FAIL No UART transport exists on this board.
 *
 * @pre The caller selected the UART transport, which this build does not.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No buffer is discarded, because none exists.
 * @post At most one line is emitted.
 *
 * @note Reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_uart_flush_input(void* ctx)
{
  if (ctx == nullptr) {
    return RET_INVALID;
  }
  return internal_absent("_h_uart_flush_input");
}

/** @brief Implementation of `priv_ra8_esp_hosted_osi_bind_absent()` -- fills all
 *  sixteen rows so none is ever left null. */
RA8_PRIV
void priv_ra8_esp_hosted_osi_bind_absent(hosted_osi_funcs_t* out)
{
  if (out == nullptr) {
    return;
  }

  out->_h_sdio_card_init       = internal_sdio_card_init;
  out->_h_sdio_card_deinit     = internal_sdio_card_deinit;
  out->_h_sdio_read_reg        = internal_sdio_read_reg;
  out->_h_sdio_write_reg       = internal_sdio_write_reg;
  out->_h_sdio_read_block      = internal_sdio_read_block;
  out->_h_sdio_write_block     = internal_sdio_write_block;
  out->_h_sdio_wait_slave_intr = internal_sdio_wait_intr; /* LEGACY-OK: upstream field */

  out->_h_spi_hd_read_reg       = internal_spi_hd_read_reg;
  out->_h_spi_hd_write_reg      = internal_spi_hd_write_reg;
  out->_h_spi_hd_read_dma       = internal_spi_hd_read_dma;
  out->_h_spi_hd_write_dma      = internal_spi_hd_write_dma;
  out->_h_spi_hd_set_data_lines = internal_spi_hd_set_data_lines;
  out->_h_spi_hd_send_cmd9      = internal_spi_hd_send_cmd9;

  out->_h_uart_read        = internal_uart_read;
  out->_h_uart_write       = internal_uart_write;
  out->_h_uart_flush_input = internal_uart_flush_input;
}
