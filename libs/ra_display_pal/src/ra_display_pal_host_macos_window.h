/**
 * @file ra_display_pal_host_macos_window.h
 * @brief C-ABI window shim consumed by the host macOS display backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. The portable backend
 * (``ra_display_pal_host_macos.c``) talks to the desktop window only
 * through these four plain-C functions, so the backend itself contains
 * no Objective-C / AppKit and links on every platform.
 *
 * Two implementations satisfy this contract; exactly one is linked:
 *
 *   - ``ra_display_pal_host_macos_window_sim.c`` -- headless no-op
 *     (host tests, non-macOS, cross-compile target).
 *   - ``ra_display_pal_host_macos_window.m`` -- real Cocoa/AppKit
 *     window (the ``examples/host/floorplan`` demo only).
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

#include <stdint.h>

#include "ra_err.h"

/**
 * @struct ra_macos_window
 * @brief Opaque desktop-window handle.
 *
 * @details
 * The concrete definition lives in whichever shim implementation is
 * linked. The backend only ever holds and forwards the pointer.
 *
 * @since 0.1.0
 */
typedef struct ra_macos_window ra_macos_window_t;

/**
 * @brief Open a desktop window of the given pixel size.
 *
 * @param[in]  width_px  Window content width in pixels (>= 1).
 * @param[in]  height_px Window content height in pixels (>= 1).
 * @param[in]  title     NUL-terminated window title (ASCII).
 * @param[out] out_win   Receives the opaque window handle on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Window opened; handle returned.
 * @retval k_ra_err_null_ptr     ``title`` or ``out_win`` was NULL.
 * @retval k_ra_err_invalid_arg  Zero width or height.
 * @retval k_ra_err_hw_init_failed Windowing system refused the window.
 *
 * @pre Called from the main thread.
 * @pre ``out_win`` is writable.
 * @post On success ``*out_win`` is non-NULL until ``ra_macos_window_close``.
 * @post On failure ``*out_win`` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_window_open(uint16_t            width_px,
                                            uint16_t            height_px,
                                            const char*         title,
                                            ra_macos_window_t** out_win);

/**
 * @brief Upload an RGB565 framebuffer to the window and request a redraw.
 *
 * @param[in] win          Window handle from ``ra_macos_window_open``.
 * @param[in] rgb565       Base of the RGB565 pixel buffer.
 * @param[in] width_px     Framebuffer width in pixels.
 * @param[in] height_px    Framebuffer height in pixels.
 * @param[in] stride_bytes Row stride of ``rgb565`` in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Frame uploaded.
 * @retval k_ra_err_null_ptr     ``win`` or ``rgb565`` was NULL.
 * @retval k_ra_err_invalid_arg  Zero width / height / stride.
 * @retval k_ra_err_no_mem       Conversion buffer allocation failed.
 *
 * @pre ``ra_macos_window_open`` succeeded.
 * @pre ``rgb565`` holds at least ``stride_bytes * height_px`` bytes.
 * @post The window will paint the supplied frame on the next redraw.
 * @post ``rgb565`` is read only; the caller retains ownership.
 *
 * @note Not thread-safe; call from the main thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_window_present(ra_macos_window_t* win,
                                               const void*        rgb565,
                                               uint16_t           width_px,
                                               uint16_t           height_px,
                                               uint32_t           stride_bytes);

/**
 * @brief Drain pending UI events and report the window's close state.
 *
 * @param[in]  win              Window handle.
 * @param[out] out_should_close Set true once the user closed the window.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Events drained; flag written.
 * @retval k_ra_err_null_ptr ``win`` or ``out_should_close`` was NULL.
 *
 * @pre ``ra_macos_window_open`` succeeded.
 * @pre ``out_should_close`` is writable.
 * @post All queued UI events have been dispatched.
 * @post ``*out_should_close`` reflects the latest close state.
 *
 * @note Must be called from the main thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_window_pump(ra_macos_window_t* win, bool* out_should_close);

/**
 * @brief Report the most recent mouse click (and clear it).
 *
 * @details
 * The window records the position of the last left mouse-down in
 * framebuffer coordinates (top-left origin). This hands it back once and
 * resets the pending flag, so each click is reported exactly once. Call
 * after ``ra_macos_window_pump`` (which dispatches the UI events).
 *
 * @param[in]  win           Window handle.
 * @param[out] out_has_click Set true if a click was pending.
 * @param[out] out_x         Framebuffer column of the click.
 * @param[out] out_y         Framebuffer row of the click.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Click state reported.
 * @retval k_ra_err_null_ptr Any argument was NULL.
 *
 * @pre ``ra_macos_window_open`` succeeded.
 * @pre The output pointers are writable.
 * @post The pending-click flag is cleared.
 * @post ``*out_x`` / ``*out_y`` are valid only when ``*out_has_click``.
 *
 * @note Must be called from the main thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_window_poll_click(ra_macos_window_t* win,
                                                  bool*              out_has_click,
                                                  uint16_t*          out_x,
                                                  uint16_t*          out_y);

/**
 * @brief Close the window and release its resources.
 *
 * @param[in] win Window handle (may already be closing).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Closed (or already closed).
 * @retval k_ra_err_null_ptr ``win`` was NULL.
 *
 * @pre ``ra_macos_window_open`` succeeded.
 * @pre No concurrent present / pump on another thread.
 * @post The window is no longer visible.
 * @post ``win`` must not be reused after this call.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_window_close(ra_macos_window_t* win);

#ifdef __cplusplus
}
#endif
