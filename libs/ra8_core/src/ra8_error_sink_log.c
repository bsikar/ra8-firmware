/**
 * @file ra8_error_sink_log.c
 * @brief Production `ra8_error_interface_t` backed by the log backend
 *
 * @details
 * `ra8_error_interface.h` publishes `g_ra8_error_sink_log` as the
 * production sink a driver points at when it wants to report a
 * non-fatal error (a retry that eventually succeeded, a degraded
 * sensor, a CRC mismatch) without halting the system through
 * `ra8_fatal_error()`. Until this translation unit existed the symbol
 * was declared and never defined, so taking its address did not link.
 *
 * The sink is deliberately thin: it forwards the report into the
 * standard `ra8_log_error_val()` backend and nothing else. It keeps no
 * state, so it needs no init call and is safe to bind from a
 * constant initializer at file scope, which is how a driver is meant
 * to default its injected sink before a test replaces it.
 *
 * NULL `tag` or `msg` arguments are substituted rather than passed
 * through: a report describing a failure must not itself become a
 * NULL dereference inside the formatter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_error_interface.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @brief Tag substituted when a caller reports with a NULL tag.
 */
static const char* const k_ra8_error_sink_tag_unset = "ERR_SINK";

/**
 * @brief Message substituted when a caller reports with a NULL message.
 */
static const char* const k_ra8_error_sink_msg_unset = "(no message)";

/**
 * @brief Forward one non-fatal error report to the log backend.
 *
 * @details Substitutes house strings for a NULL @p tag or @p msg, then emits
 *          the report through `ra8_log_error_val()` with @p err as the
 *          companion value. The sink is stateless, so @p ctx is unused; it
 *          exists because the vtable shape has to serve stateful sinks (a
 *          test ring buffer) as well as this one.
 *
 * @param[in] ctx Opaque context; always NULL for this sink and ignored.
 * @param[in] tag Source component tag, or NULL.
 * @param[in] msg Human-readable message, or NULL.
 * @param[in] err Error code classifying the report.
 *
 * @pre None: every argument is tolerated, including NULL.
 * @post One ERROR-level log line is emitted when the compile-time log level
 *       admits it; otherwise the call is a no-op.
 * @post No caller-visible state is modified.
 *
 * @note Thread-safety inherited from the log backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_error_sink_log_report(void* ctx, const char* tag,
                                                        const char* msg, ra8_err_t err)
{
  (void)ctx;

  const char* const safe_tag = (tag != NULL) ? tag : k_ra8_error_sink_tag_unset;
  const char* const safe_msg = (msg != NULL) ? msg : k_ra8_error_sink_msg_unset;

  ra8_log_error_val(safe_tag, safe_msg, (uint32_t)err);

  /* RA8_LOG_LEVEL below ERROR compiles the macro above to ((void)0),
   * which would leave the locals and `err` unused under -Werror. */
  (void)safe_tag;
  (void)safe_msg;
  (void)err;
}

/**
 * @brief Production error sink: reports land on the standard log backend.
 */
const ra8_error_interface_t g_ra8_error_sink_log = {
  .report = internal_error_sink_log_report,
  .ctx    = NULL,
};
