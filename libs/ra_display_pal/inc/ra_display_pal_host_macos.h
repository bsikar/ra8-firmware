/**
 * @file ra_display_pal_host_macos.h
 * @brief Host desktop backend (native macOS window) for the display PAL
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Exports the const ``display_backend_iface_t`` instance that presents
 * a caller-owned RGB565 framebuffer in a native macOS (Cocoa) window.
 * It is a *host-only development backend*: it lets ``ra_gfx`` / display
 * code be exercised on a desktop without flashing the EK-RA8D2, which
 * is useful for drafting panel layouts (e.g. a floor-plan view) before
 * the hardware is on the bench.
 *
 * Apps select it with
 * ``display_cfg_t.iface = &k_display_backend_host_macos;`` -- the rest
 * of the PAL API is identical to the LCD / e-ink backends, so the same
 * drawing code runs unchanged against the panel later.
 *
 * ## Build model
 *
 * The vtable implementation (``ra_display_pal_host_macos.c``) is
 * portable C and links anywhere. The actual window is driven through a
 * thin C-ABI shim (``ra_display_pal_host_macos_window.h``) with two
 * implementations selected at *link* time, not by macro:
 *
 *   - ``ra_display_pal_host_macos_window_sim.c`` -- headless no-op shim
 *     compiled into the host unit-test build (and any non-macOS build)
 *     so the backend stays linkable and testable everywhere.
 *   - ``ra_display_pal_host_macos_window.m`` -- the real Cocoa/AppKit
 *     shim, compiled only by the ``examples/host/floorplan`` demo,
 *     which links the macOS frameworks.
 *
 * The firmware / cross-compile target tree is unaffected: it never
 * lists this backend in an app's source set.
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

#include "ra_display_pal.h"
#include "ra_err.h"

/**
 * @var k_display_backend_host_macos
 * @brief Host macOS window backend vtable.
 *
 * @details
 * Pass its address through ``display_cfg_t.iface``. The backend's
 * ``init`` opens a Cocoa window sized to the cfg geometry,
 * ``get_framebuffer`` hands back the caller's RGB565 buffer,
 * ``flush`` uploads the framebuffer to the window, ``clear`` fills the
 * framebuffer with a solid RGB565 value, and ``deinit`` closes the
 * window. ``bind_guix`` is NULL (the desktop backend has no GUIX
 * driver shim), so ``display_bind_guix`` reports
 * ``k_ra_err_not_supported``.
 *
 * @since 0.1.0
 */
extern const display_backend_iface_t k_display_backend_host_macos;

/**
 * @brief Pump the host window's event queue and report whether the user
 *        asked to close it.
 *
 * @details
 * Host-only convenience for a desktop run loop: the PAL vtable has no
 * notion of an event pump because on-panel backends scan continuously,
 * so this lives outside the vtable. Call it once per frame between
 * ``display_flush`` calls; when ``*out_should_close`` becomes true the
 * user closed the window and the caller should ``display_deinit`` and
 * exit.
 *
 * @param[out] out_should_close Set to true once the window is closing.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Events drained; flag written.
 * @retval k_ra_err_null_ptr        ``out_should_close`` was NULL.
 * @retval k_ra_err_not_initialized No window is currently open.
 *
 * @pre ``display_init`` with ``k_display_backend_host_macos`` succeeded.
 * @pre ``out_should_close`` is writable.
 * @post Pending UI events have been dispatched.
 * @post ``*out_should_close`` reflects the window's close state.
 *
 * @note Must be called from the main thread (AppKit requirement).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_display_pal_host_macos_pump(bool* out_should_close);

/**
 * @brief Report the most recent mouse click in the host window.
 *
 * @details
 * Host-only convenience mirroring ``ra_display_pal_host_macos_pump``:
 * hands back the position of the last unprocessed left click in
 * framebuffer coordinates (top-left origin) and clears it. Call once per
 * frame after the pump to drive click-based UI (e.g. a tab bar).
 *
 * @param[out] out_has_click Set true if a click was pending.
 * @param[out] out_x         Framebuffer column of the click.
 * @param[out] out_y         Framebuffer row of the click.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Click state reported.
 * @retval k_ra_err_null_ptr        Any argument was NULL.
 * @retval k_ra_err_not_initialized No window is currently open.
 *
 * @pre ``display_init`` with ``k_display_backend_host_macos`` succeeded.
 * @pre The output pointers are writable.
 * @post The pending-click flag is cleared.
 *
 * @note Must be called from the main thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_display_pal_host_macos_poll_click(bool* out_has_click, uint16_t* out_x, uint16_t* out_y);

#ifdef __cplusplus
}
#endif
