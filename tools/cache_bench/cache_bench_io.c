/**
 * @file cache_bench_io.c
 * @brief Bounded formatting and complete-write helpers for cache_bench.
 * @details Completes short injected writes and formats one bounded automatic
 *          record without owning a stream or acquiring dynamic storage.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "cache_bench_io.h"

#include <stdio.h>

typedef enum : size_t {
  k_cb_format_capacity = 512U,     /**< Largest single report fragment.      */
  k_cb_write_limit     = 1048576U, /**< Fail-closed short-write retry bound. */
} cb_io_limit_t;

cb_io_status_t cb_sink_write_all(cb_sink_t* sink, const void* data, size_t length)
{
  if ((sink == nullptr) || (sink->write == nullptr) || ((data == nullptr) && (length != 0U))) {
    return k_cb_io_fault;
  }
  size_t offset = 0U;
  size_t calls  = 0U;
  while (offset < length) {
    size_t               written = 0U;
    const cb_io_status_t status =
      sink->write(sink->ctx, &((const uint8_t*)data)[offset], length - offset, &written);
    if ((status != k_cb_io_ok) || (written == 0U) || (written > (length - offset))) {
      return (status == k_cb_io_ok) ? k_cb_io_fault : status;
    }
    offset += written;
    calls++;
    if (calls > (size_t)k_cb_write_limit) {
      return k_cb_io_fault;
    }
  }
  return k_cb_io_ok;
}

cb_io_status_t cb_sink_vformat(cb_sink_t* sink, const char* format, va_list args)
{
  if ((sink == nullptr) || (format == nullptr)) {
    return k_cb_io_fault;
  }
  char      buffer[k_cb_format_capacity];
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  if ((length < 0) || ((size_t)length >= sizeof(buffer))) {
    return k_cb_io_capacity;
  }
  return cb_sink_write_all(sink, buffer, (size_t)length);
}

cb_io_status_t cb_sink_format(cb_sink_t* sink, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const cb_io_status_t status = cb_sink_vformat(sink, format, args);
  va_end(args);
  return status;
}
