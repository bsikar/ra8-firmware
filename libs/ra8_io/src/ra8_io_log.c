/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_log.c
 * @brief Adapter that forwards `ra8_log` bytes into an ra8_io stream.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Installs a ::ra8_log_byte_sink_fn_t that writes each log byte to a bound
 * ::ra8_io_stream_t. The dependency runs the right way: `ra8_core` defines the
 * sink callback type, `ra8_io` implements it.
 */

#include "ra8_io_log.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_log";

/**
 * @brief Byte sink: write one log byte to the attached stream.
 *
 * @details
 * Best-effort: a stream write error is ignored so a failing log destination
 * never propagates an error into the logging call site.
 *
 * @param[in] ctx  The attached ::ra8_io_stream_t (as a void cookie).
 * @param[in] byte Log byte to forward.
 *
 * @return void
 *
 * @pre `ctx` is the stream passed to ::ra8_io_log_attach.
 * @pre The stream out-lives the redirect.
 * @post The byte was offered to the stream (errors ignored).
 * @post No global state is mutated.
 *
 * @note Not thread-safe with respect to concurrent logging.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void io_log_byte(void* ctx, uint8_t byte)
{
  if (ctx == nullptr) {
    return; /* GCOVR_EXCL_LINE -- ctx is the stream ra8_io_log_attach validated non-NULL. */
  }
  ra8_io_stream_t* s = (ra8_io_stream_t*)ctx;
  (void)ra8_io_stream_write(s, &byte, 1U, nullptr);
}

ra8_err_t ra8_io_log_attach(ra8_io_stream_t* s)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  if (s->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  ra8_log_set_byte_sink(io_log_byte, s);
  return k_ra8_ok;
}

void ra8_io_log_detach(void)
{
  ra8_log_set_byte_sink(nullptr, nullptr);
}
