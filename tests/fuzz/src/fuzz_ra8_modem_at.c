/**
 * @file fuzz_ra8_modem_at.c
 * @brief libFuzzer harness for the ra8_modem_at AT response parser.
 *
 * @details
 * The AT command driver consumes a stream of bytes from a modem (via
 * its rx_byte() callback) and assembles them into framed responses.
 * The parser must handle arbitrary garbage from a hostile modem without
 * overflowing line buffers or transitioning into invalid states.
 *
 * The fuzzer wires a simple in-memory FIFO for the rx side (seeded
 * from the libFuzzer input), a no-op tx side, and a fake monotonic
 * clock that always advances so the driver eventually times out. Each
 * iteration calls ra8_modem_at_send_cmd() which internally pumps the
 * rx_byte() callback and runs the response parser to completion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_modem_at.h"

/**
 * @enum modem_at_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_modem_at_default_timeout_ms_50 =
    50U, /**< Modem AT default per-command timeout, in milliseconds. */
} modem_at_uint8_const_t;

enum : uint16_t {
  k_fuzz_line_buf_len = 256U,  /**< Fuzz line buffer length. */
  k_fuzz_max_input    = 4096U, /**< Fuzz maximum input.      */
};

static const uint8_t* s_rx_data;
static size_t         s_rx_size;
static size_t         s_rx_pos;
static uint32_t       s_now_ms;
static uint8_t        s_line_buf[k_fuzz_line_buf_len];

static ra8_err_t fuzz_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
  return k_ra8_ok;
}

static ra8_err_t fuzz_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  if (s_rx_pos >= s_rx_size) {
    return k_ra8_err_no_data;
  }
  *out = s_rx_data[s_rx_pos];
  ++s_rx_pos;
  return k_ra8_ok;
}

static uint32_t fuzz_now(void* ctx)
{
  (void)ctx;
  /* Always advance so the driver eventually times out instead of
   * spinning forever on an empty FIFO. */
  s_now_ms += 1U;
  return s_now_ms;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > k_fuzz_max_input) {
    return 0;
  }
  s_rx_data = data;
  s_rx_size = size;
  s_rx_pos  = 0U;
  s_now_ms  = 0U;

  ra8_modem_at_cfg_t cfg = {
    .io           = {.tx_byte = fuzz_tx, .rx_byte = fuzz_rx, .now_ms = fuzz_now, .ctx = nullptr},
    .line_buf     = s_line_buf,
    .line_buf_len = (uint16_t)sizeof s_line_buf,
    .default_timeout_ms = k_modem_at_default_timeout_ms_50,
  };
  if (ra8_modem_at_init(&cfg) != k_ra8_ok) {
    return 0;
  }
  /* Drive the response parser by issuing a benign AT command and
   * letting the driver walk the entire seeded RX FIFO. */
  (void)ra8_modem_at_send_cmd("AT", "OK", k_modem_at_default_timeout_ms_50);
  return 0;
}
