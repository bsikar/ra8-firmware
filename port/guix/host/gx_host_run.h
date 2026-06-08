/**
 * @file gx_host_run.h
 * @brief Host single-threaded GUIX drive helper
 *
 * @details
 * The host GUIX build runs single-threaded (generic RTOS bind, GUIX's own
 * thread loop never starts). The application loop calls gx_host_pump() once
 * per iteration to drain + dispatch queued GUIX events (pen events, button
 * clicks), then gx_system_canvas_refresh() to repaint.
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

/**
 * @brief Drain and dispatch all queued GUIX events (single-threaded host).
 *
 * @details
 * GUIX's own thread loop never runs in the single-threaded bind, so the
 * application calls this once per frame to pop and dispatch every queued event
 * (pen, button) before repainting with gx_system_canvas_refresh.
 *
 * @pre gx_system_initialize has succeeded.
 * @pre Called from the single application thread.
 * @post The GUIX event ring is empty; widget state reflects dispatched events.
 * @post Pending redraws are marked for the next canvas refresh.
 *
 * @note Not thread-safe; single-threaded drive only.
 *
 * @since 0.1.0
 */
void gx_host_pump(void);

#ifdef __cplusplus
}
#endif
