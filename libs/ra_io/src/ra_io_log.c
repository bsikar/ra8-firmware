/**
 * @file ra_io_log.c
 * @brief Adapter that forwards `ra_log` bytes into an ra_io stream.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Installs a ::ra_log_byte_sink_fn_t that writes each log byte to a bound
 * ::ra_io_stream_t. The dependency runs the right way: `ra_core` defines the
 * sink callback type, `ra_io` implements it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_log.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_stream.h"
#include "ra_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_log";

/**
 * @brief Byte sink: write one log byte to the attached stream.
 *
 * @details
 * Best-effort: a stream write error is ignored so a failing log destination
 * never propagates an error into the logging call site.
 *
 * @param[in] ctx  The attached ::ra_io_stream_t (as a void cookie).
 * @param[in] byte Log byte to forward.
 *
 * @return void
 *
 * @pre `ctx` is the stream passed to ::ra_io_log_attach.
 * @pre The stream out-lives the redirect.
 * @post The byte was offered to the stream (errors ignored).
 * @post No global state is mutated.
 *
 * @note Not thread-safe with respect to concurrent logging.
 *
 * @since 0.1.0
 */
static void io_log_byte(void* ctx, uint8_t byte)
{
  if (ctx == nullptr) {
    return; /* GCOVR_EXCL_LINE -- ctx is the stream ra_io_log_attach validated non-NULL. */
  }
  ra_io_stream_t* s = (ra_io_stream_t*)ctx;
  (void)ra_io_stream_write(s, &byte, 1U, nullptr);
}

ra_err_t ra_io_log_attach(ra_io_stream_t* s)
{
  RA_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  if (s->iface == nullptr) {
    return k_ra_err_not_initialized;
  }
  ra_log_set_byte_sink(io_log_byte, s);
  return k_ra_ok;
}

void ra_io_log_detach(void)
{
  ra_log_set_byte_sink(nullptr, nullptr);
}
