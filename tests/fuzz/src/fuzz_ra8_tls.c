/**
 * @file fuzz_ra8_tls.c
 * @brief libFuzzer harness for the ra8_tls Mbed TLS facade.
 *
 * @details
 * The host test build defines ``RA8_OFF_TARGET`` for every TU, so
 * ``ra8_tls.c`` substitutes a single-byte loopback drain for each
 * ``mbedtls_ssl_*`` call. That keeps the harness self-contained
 * (no real Mbed TLS link dependency) while still exercising:
 *
 *   - ra8_tls_session_open / close pool bookkeeping (NASA Rule 3 cap).
 *   - The handshake / send / recv state machine and BIO function-pointer
 *     plumbing under attacker-controlled byte streams.
 *   - The loop ring used by the BIO send/recv callbacks.
 *
 * Each fuzz input feeds the BIO recv callback with arbitrary bytes the
 * "transport" hands to the facade. The facade is driven through:
 * global_init -> session_open -> handshake -> send(input) -> recv ->
 * close, all inside a single iteration so libFuzzer can isolate any
 * state corruption to a minimal reproducer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_tls.h"

enum : uint16_t {
  k_fuzz_max_input  = 4096U, /**< Fuzz maximum input. */
  k_fuzz_loop_cap   = 4096U, /**< Fuzz loop cap.      */
  k_fuzz_recv_chunk = 256U,  /**< Fuzz recv chunk.    */
};

typedef struct {
  uint8_t        buf[k_fuzz_loop_cap]; /**< Buffer.    */
  uint16_t       count;                /**< Count.     */
  uint16_t       head;                 /**< Head.      */
  uint16_t       tail;                 /**< Tail.      */
  const uint8_t* feed_data;            /**< Feed data. */
  size_t         feed_size;            /**< Feed size. */
  size_t         feed_pos;             /**< Feed pos.  */
} loop_ctx_t;

static loop_ctx_t s_loop;
static uint8_t    s_initialized;

static ra8_err_t loop_send(void* ctx, const uint8_t* buf, size_t len, size_t* out_sent)
{
  (void)ctx;
  *out_sent      = 0U;
  size_t written = 0U;
  while ((written < len) && (s_loop.count < (uint16_t)k_fuzz_loop_cap)) {
    s_loop.buf[s_loop.head] = buf[written]; /* Producer write. */
    s_loop.head             = (uint16_t)((s_loop.head + 1U) % (uint16_t)k_fuzz_loop_cap);
    s_loop.count++;
    written++;
  }
  *out_sent = written;
  return k_ra8_ok;
}

static ra8_err_t loop_recv(void* ctx, uint8_t* buf, size_t len, size_t* out_received)
{
  (void)ctx;
  *out_received = 0U;
  /* Prefer attacker-controlled fuzz bytes when available; fall through
   * to whatever the facade itself produced (ring) once exhausted. */
  size_t produced = 0U;
  while ((produced < len) && (s_loop.feed_pos < s_loop.feed_size)) {
    buf[produced] = s_loop.feed_data[s_loop.feed_pos];
    s_loop.feed_pos++;
    produced++;
  }
  while ((produced < len) && (s_loop.count > 0U)) {
    buf[produced] = s_loop.buf[s_loop.tail];
    s_loop.tail   = (uint16_t)((s_loop.tail + 1U) % (uint16_t)k_fuzz_loop_cap);
    s_loop.count--;
    produced++;
  }
  *out_received = produced;
  return k_ra8_ok;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_max_input) {
    return 0;
  }
  if (s_initialized == 0U) {
    if (ra8_tls_global_init() != k_ra8_ok) {
      return 0;
    }
    s_initialized = 1U;
  }

  (void)memset(&s_loop, 0, sizeof s_loop);
  s_loop.feed_data = data;
  s_loop.feed_size = size;

  ra8_tls_session_cfg_t cfg = {};
  cfg.transport.send        = loop_send;
  cfg.transport.recv        = loop_recv;
  cfg.transport.ctx         = nullptr;
  cfg.server_name           = nullptr;

  ra8_tls_session_t session = nullptr;
  if (ra8_tls_session_open(&session, &cfg) != k_ra8_ok) {
    return 0;
  }
  (void)ra8_tls_handshake(session);

  size_t sent = 0U;
  (void)ra8_tls_send(session, data, size, &sent);

  uint8_t recv_buf[k_fuzz_recv_chunk];
  size_t  received = 0U;
  (void)ra8_tls_recv(session, recv_buf, sizeof recv_buf, &received);

  (void)ra8_tls_session_close(session);
  return 0;
}
