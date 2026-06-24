/**
 * @file ra_io_log.h
 * @brief Redirect `ra_log` output into an ra_io stream.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Bridges the `ra_core` logger to the `ra_io` stream facade: attach a bound
 * ::ra_io_stream_t and every subsequent log byte (the `[TAG] LEVEL: message`
 * line the logger would have sent to ITM) is written to that stream instead.
 * This makes the log destination run-time selectable -- a UART, a USB-CDC link,
 * a RAM capture buffer, or a raw block device -- without `ra_core` depending on
 * `ra_io`. Detach to restore the default ITM backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"
#include "ra_io_stream.h"

/**
 * @brief Route `ra_log` output to a bound ra_io stream.
 *
 * @param[in] s Bound stream to receive log bytes (must out-live the redirect).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Logging now flows to `s`.
 * @retval k_ra_err_null_ptr        `s` was NULL.
 * @retval k_ra_err_not_initialized `s` has no sink bound.
 *
 * @pre `s` is bound to a sink and out-lives the redirect.
 * @pre `ra_log_init` has run (or will run) for the default path.
 * @post Subsequent log lines are written to `s`.
 * @post Call ::ra_io_log_detach to restore the ITM backend.
 *
 * @note Not thread-safe with respect to concurrent logging.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_log_attach(ra_io_stream_t* s);

/**
 * @brief Restore the default ITM log backend.
 *
 * @details Clears any stream installed by ::ra_io_log_attach.
 *
 * @pre None.
 * @pre None.
 * @post Subsequent log output goes to the ITM port again.
 * @post The previously attached stream receives no further bytes.
 *
 * @note Not thread-safe with respect to concurrent logging.
 *
 * @since 0.1.0
 */
void ra_io_log_detach(void);

#ifdef __cplusplus
}
#endif
