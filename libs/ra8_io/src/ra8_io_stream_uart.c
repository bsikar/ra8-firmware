/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_stream_uart.c
 * @brief UART/SCI byte-stream sink -- polled writes through ra8_sci.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Thin wrapper binding ::ra8_io_stream_iface to `ra8_sci_write_polling` /
 * `ra8_sci_flush`. Touches no raw MMIO -- the `ra8_sci` driver owns the HUM
 * citations.
 */

#include "ra8_io_stream_uart.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream_internal.h"
#include "ra8_sci.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_stream_uart";

/**
 * @brief UART sink: write all bytes out of the SCI channel by polling.
 *
 * @details
 * Forwards to `ra8_sci_write_polling`. On success every byte was shifted out, so
 * the accepted count equals `len`.
 *
 * @param[in]  ctx         UART sink state (as a void cookie).
 * @param[in]  buf         Source bytes.
 * @param[in]  len         Number of bytes to send.
 * @param[out] out_written Accepted byte count, or NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All bytes sent.
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_*        Propagated from `ra8_sci_write_polling`.
 *
 * @pre `ctx` is a populated UART sink state.
 * @pre The SCI channel is initialised.
 * @post On success all `len` bytes were transmitted.
 * @post `*out_written` (when provided) equals `len` on success.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t uart_write(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_stream_uart_state_t* st = (const ra8_io_stream_uart_state_t*)ctx;
  const ra8_err_t                   e  = ra8_sci_write_polling(st->channel, buf, len);
  if (e != k_ra8_ok) {
    return e;
  }
  if (out_written != nullptr) {
    *out_written = len;
  }
  return k_ra8_ok;
}

/**
 * @brief UART sink: drain the SCI transmit shift register.
 *
 * @details
 * Forwards to `ra8_sci_flush` so a caller can guarantee the line is on the wire
 * before sleeping or asserting a panic.
 *
 * @param[in] ctx UART sink state (as a void cookie).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Transmit drained.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from `ra8_sci_flush`.
 *
 * @pre `ctx` is a populated UART sink state.
 * @pre The SCI channel is initialised.
 * @post On success the shift register is empty.
 * @post No sink state is mutated.
 *
 * @note Not thread-safe with respect to the same channel.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t uart_flush(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  const ra8_io_stream_uart_state_t* st = (const ra8_io_stream_uart_state_t*)ctx;
  return ra8_sci_flush(st->channel);
}

/** @brief UART stream sink vtable. */
static const ra8_io_stream_iface_t k_uart_iface = {
  .write = uart_write,
  .flush = uart_flush,
};

ra8_err_t
ra8_io_stream_uart_init(ra8_io_stream_t* s, ra8_io_stream_uart_state_t* state, uint8_t channel)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  state->channel = channel;
  s->iface       = &k_uart_iface;
  s->ctx         = state;
  return k_ra8_ok;
}
