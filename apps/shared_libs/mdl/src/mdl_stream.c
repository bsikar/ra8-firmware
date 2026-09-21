/**
 * @file mdl_stream.c
 * @brief Private bounded text helpers over the portable byte-stream contract.
 * @details Chains exact fragments and bounded numeric rendering while retaining
 *          the first sink error without allocation or variadic formatting.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_stream_internal.h"

/** @brief Maximum repeated byte count accepted by one helper call. */
typedef enum : uint8_t {
  k_mdl_stream_repeat_max = 64, /**< Fixed loop bound for presenter fragments. */
} mdl_stream_limit_t;

RA8_PRIV ra8_err_t priv_mdl_stream_text(ra8_err_t prior, ra8_io_stream_t* stream, const char* text)
{
  return (prior == k_ra8_ok) ? ra8_io_stream_puts(stream, text) : prior;
}

RA8_PRIV ra8_err_t priv_mdl_stream_u64(ra8_err_t prior, ra8_io_stream_t* stream, uint64_t value)
{
  return (prior == k_ra8_ok) ? ra8_io_stream_put_u64(stream, value) : prior;
}

RA8_PRIV ra8_err_t priv_mdl_stream_hex(ra8_err_t prior, ra8_io_stream_t* stream, uint32_t value)
{
  return (prior == k_ra8_ok) ? ra8_io_stream_put_hex(stream, value, 1U) : prior;
}

RA8_PRIV ra8_err_t priv_mdl_stream_repeat(ra8_err_t        prior,
                                          ra8_io_stream_t* stream,
                                          char             byte,
                                          size_t           count)
{
  if (prior != k_ra8_ok) {
    return prior;
  }
  if (count > (size_t)k_mdl_stream_repeat_max) {
    return k_ra8_err_invalid_size;
  }
  RA8_LOOP_BOUND(k_mdl_stream_repeat_max);
  for (size_t i = 0U; i < count; ++i) {
    const ra8_err_t error = ra8_io_stream_putc(stream, byte);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_mdl_stream_flush(ra8_err_t prior, ra8_io_stream_t* stream)
{
  return (prior == k_ra8_ok) ? ra8_io_stream_flush(stream) : prior;
}
