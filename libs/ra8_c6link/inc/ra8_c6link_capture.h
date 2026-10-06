/**
 * @file ra8_c6link_capture.h
 * @brief Transport wrapper that reports every C6 link transaction as text.
 *
 * @details
 * ::ra8_c6link_capture_bind fills a ::ra8_c6link_transport_t whose rows
 * forward to an inner transport and hand each transaction to a line sink, so
 * a bench run can record the real wire traffic over a debug console. Nothing
 * in the link depends on it: an image that never binds it is unchanged.
 *
 * One record per line, each ending in `\n`:
 *   - `c6cap <seq> tx <hex>`: the frame clocked out, trailing zero bytes dropped.
 *   - `c6cap <seq> rx <hex>`: the frame clocked in, trailing zero bytes dropped;
 *     only written when the inner transfer returned `k_ra8_ok`.
 *   - `c6cap <seq> hs <0|1>`: a HANDSHAKE level change seen before transaction
 *     `seq`. The first sample is always reported.
 *
 * The sink receives each line in pieces (the record head, the hex in chunks
 * of at most 128 characters, then the newline); a console writes them as they
 * arrive.
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

#include "ra8_c6link_transport.h"
#include "ra8_err.h"

/** @brief Receives one piece of a capture line; pieces are never null-terminated. */
typedef void (*ra8_c6link_capture_sink_t)(void* ctx, const char* text, uint16_t len);

/**
 * @struct ra8_c6link_capture
 * @brief State behind a bound capture transport; caller-allocated.
 * @details Must outlive the link opened on the transport it fills.
 */
typedef struct ra8_c6link_capture {
  ra8_c6link_transport_t inner;   /**< The transport every row forwards to. */
  ra8_c6link_capture_sink_t sink; /**< Where the lines go.                  */
  void* sink_ctx;                 /**< Handed back to every sink call.      */
  uint32_t seq;                   /**< Transactions clocked so far.         */
  uint8_t handshake;              /**< Last level reported, 0xFF for none.  */
} ra8_c6link_capture_t;

/**
 * @brief Wrap @p inner so every transaction is reported to @p sink.
 * @param cap      State to (re)initialise; the bound rows point at it.
 * @param inner    Fully filled transport; copied into @p cap.
 * @param sink     Line sink; must not be null.
 * @param sink_ctx Opaque context for @p sink.
 * @param out      Receives the capture transport to pass to ::ra8_c6link_open.
 * @return `k_ra8_ok`, `k_ra8_err_null_ptr` for a null @p cap, @p inner or
 *         @p out, or `k_ra8_err_invalid_arg` for a null row or sink.
 */
ra8_err_t ra8_c6link_capture_bind(ra8_c6link_capture_t* cap, const ra8_c6link_transport_t* inner,
                                  ra8_c6link_capture_sink_t sink, void* sink_ctx,
                                  ra8_c6link_transport_t* out);

#ifdef __cplusplus
}
#endif
