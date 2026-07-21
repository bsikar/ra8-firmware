/**
 * @file esp_uart.c
 * @brief "Ours" register-level UART0 TX backend implementing @ref esp_uart_ops_t.
 *
 * @details
 * The ROM leaves UART0 configured as its boot console (TRM Ch 9.2.3 "ROM
 * Messages Printing Control" p 377), so this spike backend does not re-program
 * the baud divisor; it only pushes bytes. Each byte write first spins -- with
 * a hard iteration cap (NASA P10 Rule 2) -- until the TX FIFO reports room via
 * `UART_STATUS_REG`'s TXFIFO_CNT field, then writes `UART_FIFO_REG`. Register
 * citations reference TRM v1.2 (see @ref esp_soc.h). The assumed 115200 8N1
 * console rate is stated nowhere in the TRM or datasheet and remains
 * `[CONFIRM]` (esptool-lore) until the bench proves it; a fused/strapped board
 * may even have ROM printing redirected or disabled (EFUSE_UART_PRINT_CONTROL,
 * TRM Ch 9.2.3 p 377).
 *
 * @note Single-threaded, IRQs masked. Singleton exposed via @ref esp_uart_ours_ops.
 * @since 0.1.0 (spike)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "esp_hal.h"
#include "esp_soc.h"

/**
 * @enum esp_uart_bounds_t
 * @brief Static loop bounds for the UART TX path (NASA P10 Rule 2).
 */
typedef enum : uint32_t {
  k_esp_uart_tx_spin_max = 1000000u, /**< Max FIFO-room poll iterations before timeout. */
  k_esp_uart_write_max   = 4096u,    /**< Max bytes accepted by one write() call.       */
} esp_uart_bounds_t;

/**
 * @struct esp_uart_ctx_t
 * @brief Per-backend state for the "ours" UART driver.
 * @invariant `initialized` is false until init() succeeds, true thereafter.
 */
typedef struct esp_uart_ctx {
  bool     initialized; /**< Set by init(); gates the TX entry points.                      */
  uint32_t baud;        /**< Baud recorded at init() (informational; ROM owns the divisor). */
} esp_uart_ctx_t;

/**
 * @var s_esp_uart0_ctx
 * @brief Singleton state backing @ref esp_uart_ours_ops.
 * @note File-scope; address handed out only through the vtable `ctx`.
 * @warning Do not mutate directly; go through the vtable.
 */
static esp_uart_ctx_t s_esp_uart0_ctx = {.initialized = false, .baud = 0u};

/**
 * @brief Read the current TX FIFO occupancy from `UART_STATUS_REG`.
 * @return Number of bytes currently queued in the TX FIFO (0..depth).
 * @pre UART0 MMIO is mapped and powered.
 * @post The returned value is masked to the TXFIFO_CNT field width.
 * @since 0.1.0 (spike)
 */
static uint32_t esp_uart_txfifo_count(void)
{
  /* TRM Ch 27.7.1 "UART Registers" Reg 27.21 UART_STATUS_REG p 769 */
  const uint32_t status = esp_uart0()->STATUS;
  return (status >> k_esp_uart_txfifo_cnt_shift) & k_esp_uart_txfifo_cnt_mask;
}

/**
 * @brief Bring the UART backend up (@ref esp_uart_ops_t::init).
 * @param[in,out] ctx Backend cookie; must be @ref s_esp_uart0_ctx.
 * @param[in] baud Requested line rate in bits/s; must be non-zero.
 * @return esp_err_t
 * @retval k_esp_ok Backend marked initialized.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg @p baud was zero.
 * @pre @p ctx is non-null.
 * @pre @p baud is non-zero.
 * @post `initialized` is true.
 * @post `baud` records the requested rate.
 * @note The divisor is owned by the ROM; @p baud is recorded, not programmed.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_uart_ours_init(void* ctx, uint32_t baud)
{
  esp_uart_ctx_t* self = (esp_uart_ctx_t*)ctx;
  if (self == nullptr) {
    return k_esp_err_null_ptr;
  }
  if (baud == 0u) {
    return k_esp_err_invalid_arg;
  }
  self->baud        = baud;
  self->initialized = true;
  return k_esp_ok;
}

/**
 * @brief Transmit one byte, blocking until FIFO room (@ref esp_uart_ops_t::putc).
 * @param[in,out] ctx Backend cookie; must be an initialized @ref s_esp_uart0_ctx.
 * @param[in] byte Octet to enqueue into the TX FIFO.
 * @return esp_err_t
 * @retval k_esp_ok Byte written to the TX FIFO.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg Backend not initialized.
 * @retval k_esp_err_timeout FIFO never reported room within the spin cap.
 * @pre @p ctx is non-null and initialized.
 * @pre The TX FIFO drains (a live UART clock).
 * @post On success exactly one byte was pushed to `UART_FIFO_REG`.
 * @post The poll loop ran at most @ref k_esp_uart_tx_spin_max iterations.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_uart_ours_putc(void* ctx, uint8_t byte)
{
  esp_uart_ctx_t* self = (esp_uart_ctx_t*)ctx;
  if (self == nullptr) {
    return k_esp_err_null_ptr;
  }
  if (!self->initialized) {
    return k_esp_err_invalid_arg;
  }
  bool has_room = false;
  for (uint32_t spins = 0u; spins < k_esp_uart_tx_spin_max; ++spins) {
    if (esp_uart_txfifo_count() < k_esp_uart_fifo_depth) {
      has_room = true;
      break;
    }
  }
  if (!has_room) {
    return k_esp_err_timeout;
  }
  /*
     * TRM Ch 27.4.2 "UART FIFO" p 734 -- hardware detects a write to
     * UART_FIFO_REG (offset: Reg 27.1 p 753) and routes the data to the TX
     * FIFO via a bypass, even though the register's summary access type is RO.
     */
  esp_uart0()->FIFO = (uint32_t)byte;
  return k_esp_ok;
}

/**
 * @brief Transmit a byte buffer (@ref esp_uart_ops_t::write).
 * @param[in,out] ctx Backend cookie; must be an initialized @ref s_esp_uart0_ctx.
 * @param[in] data Source buffer; must be non-null when @p len is non-zero.
 * @param[in] len Byte count, 0..@ref k_esp_uart_write_max.
 * @return esp_err_t
 * @retval k_esp_ok All @p len bytes transmitted.
 * @retval k_esp_err_null_ptr @p ctx or @p data was null.
 * @retval k_esp_err_invalid_arg @p len exceeds @ref k_esp_uart_write_max.
 * @retval k_esp_err_timeout A byte write timed out (propagated from putc).
 * @pre @p ctx is non-null and initialized.
 * @pre @p data is non-null.
 * @post On success @p len bytes were pushed in order.
 * @post The loop ran at most @ref k_esp_uart_write_max iterations.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_uart_ours_write(void* ctx, const uint8_t* data, size_t len)
{
  esp_uart_ctx_t* self = (esp_uart_ctx_t*)ctx;
  if (self == nullptr) {
    return k_esp_err_null_ptr;
  }
  if (data == nullptr) {
    return k_esp_err_null_ptr;
  }
  if (len > k_esp_uart_write_max) {
    return k_esp_err_invalid_arg;
  }
  for (size_t i = 0u; i < len; ++i) {
    const esp_err_t err = esp_uart_ours_putc(ctx, data[i]);
    if (err != k_esp_ok) {
      return err;
    }
  }
  return k_esp_ok;
}

/**
 * @brief Block (bounded) until the TX FIFO drains (@ref esp_uart_ops_t::flush).
 * @param[in,out] ctx Backend cookie; must be an initialized @ref s_esp_uart0_ctx.
 * @return esp_err_t
 * @retval k_esp_ok TX FIFO reached empty.
 * @retval k_esp_err_null_ptr @p ctx was null.
 * @retval k_esp_err_invalid_arg Backend not initialized.
 * @retval k_esp_err_timeout FIFO never emptied within the spin cap.
 * @pre @p ctx is non-null and initialized.
 * @pre The TX FIFO drains (a live UART clock).
 * @post On success the TX FIFO occupancy read zero.
 * @post The poll loop ran at most @ref k_esp_uart_tx_spin_max iterations.
 * @since 0.1.0 (spike)
 */
static esp_err_t esp_uart_ours_flush(void* ctx)
{
  esp_uart_ctx_t* self = (esp_uart_ctx_t*)ctx;
  if (self == nullptr) {
    return k_esp_err_null_ptr;
  }
  if (!self->initialized) {
    return k_esp_err_invalid_arg;
  }
  for (uint32_t spins = 0u; spins < k_esp_uart_tx_spin_max; ++spins) {
    if (esp_uart_txfifo_count() == 0u) {
      return k_esp_ok;
    }
  }
  return k_esp_err_timeout;
}

/**
 * @var s_esp_uart_ours_ops
 * @brief Fully-wired "ours" UART vtable handed out by @ref esp_uart_ours_ops.
 * @note `const` so it lives in .rodata; `ctx` points at @ref s_esp_uart0_ctx.
 */
static const esp_uart_ops_t s_esp_uart_ours_ops = {
  .ctx   = &s_esp_uart0_ctx,
  .init  = esp_uart_ours_init,
  .putc  = esp_uart_ours_putc,
  .write = esp_uart_ours_write,
  .flush = esp_uart_ours_flush,
};

const esp_uart_ops_t* esp_uart_ours_ops(void)
{
  return &s_esp_uart_ours_ops;
}
