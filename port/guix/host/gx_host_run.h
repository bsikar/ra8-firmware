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
 * @pre gx_system_initialize has succeeded.
 * @post The GUIX event ring is empty; widget state reflects dispatched events.
 *
 * @since 0.1.0
 */
void gx_host_pump(void);

#ifdef __cplusplus
}
#endif
