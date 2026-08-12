/**
 * @file ra8_io_stream_internal.h
 * @brief Internal vtable type for the ra8_io byte-stream facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the stream dispatcher and the sink
 * translation units include this file. Tests under `tests/` MAY include it to
 * drive MC/DC vectors against internal helpers; application code never should.
 *
 * Defines ::ra8_io_stream_iface -- the function-pointer table each sink fills in
 * and binds into a caller-owned ::ra8_io_stream_t. It is forward declared opaque
 * in `ra8_io_stream.h`.
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

#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @struct ra8_io_stream_iface
 * @brief One vtable per byte-sink. Each callback receives the sink's opaque
 *        `ctx` so sinks keep their state private.
 *
 * @details
 * `write` is mandatory. `flush` may be NULL for sinks that have no buffering;
 * the dispatcher maps a NULL `flush` to success.
 *
 * @invariant `write` is non-NULL.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
struct ra8_io_stream_iface {
  /**
   * @brief Write `len` bytes from `buf`, reporting the accepted count.
   * @details `out_written` receives the bytes the sink consumed (<= `len`).
   */
  ra8_err_t (*write)(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written);

  /** @brief Commit any buffering. May be NULL (nothing buffered). */
  ra8_err_t (*flush)(void* ctx);
};
/* cppcheck-suppress-end [unusedStructMember] */

#ifdef __cplusplus
}
#endif
