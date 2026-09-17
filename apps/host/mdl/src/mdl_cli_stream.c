/**
 * @file mdl_cli_stream.c
 * @brief Bounded byte-stream composition helpers for mdl diagnostics.
 * @details Implements fixed-fragment and bounded integer rendering over the
 *          injected diagnostic stream without variadic formatting.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "mdl_cli_internal.h"

/** @brief Maximum fragments accepted by one CLI diagnostic operation. */
typedef enum : uint8_t {
  k_mdl_cli_part_limit = 16, /**< Fixed bound for ordered text fragments. */
} mdl_cli_part_limit_t;

RA8_PRIV ra8_err_t priv_mdl_cli_put_parts(ra8_io_stream_t*   stream,
                                          const char* const* parts,
                                          size_t             count)
{
  if ((stream == nullptr) || (parts == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (count > k_mdl_cli_part_limit) {
    return k_ra8_err_invalid_size;
  }
  RA8_LOOP_BOUND(k_mdl_cli_part_limit);
  for (size_t i = 0U; i < count; ++i) {
    if (parts[i] == nullptr) {
      return k_ra8_err_null_ptr;
    }
    const ra8_err_t err = ra8_io_stream_puts(stream, parts[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_mdl_cli_reject_parts(ra8_io_stream_t*   stream,
                                             const char* const* parts,
                                             size_t             count)
{
  const ra8_err_t err = priv_mdl_cli_put_parts(stream, parts, count);
  return (err == k_ra8_ok) ? k_ra8_err_invalid_arg : err;
}
