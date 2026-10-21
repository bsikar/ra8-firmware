/**
 * @file fuzz_ra8_c6link_rpc.c
 * @brief libFuzzer harness for the ESP32-C6 control-plane decode path.
 *
 * @details
 * Drives ::priv_c6link_rpc_consume, the path every control-plane frame the
 * C6 sends takes on the host (RA8FW-285): the TLV envelope unwrap
 * (::priv_c6link_tlv_body), the vendored protobuf-c `rpc__unpack` of the
 * generated esp-hosted codec into the link's fixed decode arena, and the
 * routing of the decoded message to the outstanding wait or the event path.
 * The codec sources are compiled into this target, so libFuzzer's coverage
 * reaches inside protobuf-c and the generated unpackers.
 *
 * Input layout (all little-endian):
 *
 * | Offset | Size | Meaning |
 * |-------:|-----:|---------|
 * | 0 | 1 | bit 0: wrap the body in a well-formed TLV envelope; bit 1: arm a wait |
 * | 1 | 4 | the armed wait's UID |
 * | 5 | 4 | the armed wait's expected response id |
 * | 9 | ... | the body: the raw payload, or the protobuf bytes to wrap |
 *
 * Wrapping lets mutations reach the protobuf decoder without first having to
 * rediscover the envelope; the raw mode keeps the envelope parser itself
 * under the fuzzer. Either way the decoder reads from a heap copy of exactly
 * the payload's length, so ASan reports a read one byte past it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_err.h"

enum : uint32_t {
  k_fuzz_c6_header     = 9U,                                /**< Mode, UID, response id. */
  k_fuzz_c6_mode_wrap  = 0x01U,                             /**< Wrap the body in a TLV. */
  k_fuzz_c6_mode_armed = 0x02U,                             /**< Arm an outstanding wait. */
  k_fuzz_c6_arena      = (uint32_t)k_ra8_c6link_arena_min, /**< The smallest legal arena. */
  k_fuzz_c6_frame      = (uint32_t)k_ra8_c6link_frame_bytes, /**< One SPI transaction. */
};

static ra8_c6link_t       s_link;
static ra8_c6link_stats_t s_stats;
static uint8_t            s_arena[k_fuzz_c6_arena] __attribute__((aligned(8)));
static uint8_t            s_frame[k_fuzz_c6_frame];

/** @brief Read a little-endian word from @p at. */
static uint32_t fuzz_c6_word(const uint8_t* at)
{
  return (uint32_t)at[0] | ((uint32_t)at[1] << 8U) | ((uint32_t)at[2] << 16U) |
         ((uint32_t)at[3] << 24U);
}

/** @brief Announcement sink: the event path runs to the end, and the event is
 *  read so a field the decoder left dangling is touched. */
static void fuzz_c6_event(void* ctx, const ra8_c6link_event_t* ev)
{
  volatile uint8_t sink = 0U;
  (void)ctx;
  if (ev != nullptr) {
    sink = (uint8_t)ev->kind;
  }
  (void)sink;
}

/** @brief Extractor for an armed wait: reads the decoded message's id the
 *  way the real extractors start, and accepts it. */
static ra8_err_t fuzz_c6_take(void* ctx, const void* msg)
{
  (void)ctx;
  const Rpc* rpc = (const Rpc*)msg;
  return (rpc != nullptr && rpc->msg_id != 0) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/** @brief Reset the link to an open handle with an empty arena. */
static void fuzz_c6_reset(void)
{
  memset(&s_link, 0, sizeof s_link);
  memset(&s_stats, 0, sizeof s_stats);
  s_link.arena       = s_arena;
  s_link.arena_bytes = (uint32_t)sizeof s_arena;
  s_link.event_cb    = fuzz_c6_event;
  s_link.stats       = &s_stats;
  s_link.open        = true;
}

/** @brief Wrap @p body in a well-formed TLV envelope in ::s_frame.
 *  @return The envelope's length, or zero when the body does not fit. */
static uint16_t fuzz_c6_wrap(const uint8_t* body, size_t body_len)
{
  if (body_len > (size_t)UINT16_MAX) {
    return 0U;
  }
  uint16_t at = 0U;
  if (priv_c6link_tlv_open(s_frame, (uint16_t)sizeof s_frame, (uint16_t)body_len, &at) != k_ra8_ok) {
    return 0U;
  }
  if ((size_t)at + body_len > sizeof s_frame) {
    return 0U;
  }
  memcpy(&s_frame[at], body, body_len);
  return (uint16_t)(at + body_len);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size < (size_t)k_fuzz_c6_header || size - (size_t)k_fuzz_c6_header > (size_t)k_fuzz_c6_frame) {
    return 0;
  }
  const uint8_t  mode     = data[0];
  const uint8_t* body     = data + k_fuzz_c6_header;
  const size_t   body_len = size - (size_t)k_fuzz_c6_header;

  fuzz_c6_reset();
  if ((mode & k_fuzz_c6_mode_armed) != 0U) {
    s_link.wait.armed   = true;
    s_link.wait.uid     = fuzz_c6_word(&data[1]);
    s_link.wait.resp_id = fuzz_c6_word(&data[5]);
    s_link.wait.take    = fuzz_c6_take;
  }

  const uint8_t* source = body;
  uint16_t       len    = (uint16_t)body_len;
  if ((mode & k_fuzz_c6_mode_wrap) != 0U) {
    len    = fuzz_c6_wrap(body, body_len);
    source = s_frame;
    if (len == 0U) {
      return 0;
    }
  }
  uint8_t* payload = (uint8_t*)malloc(len == 0U ? 1U : (size_t)len);
  if (payload == nullptr) {
    return 0;
  }
  memcpy(payload, source, len);
  (void)priv_c6link_rpc_consume(&s_link, payload, len);
  free(payload);

  /* The arena must be empty again however the decode went. */
  if (s_link.arena_used != 0U || s_link.arena_last != 0U) {
    __builtin_trap();
  }
  return 0;
}
