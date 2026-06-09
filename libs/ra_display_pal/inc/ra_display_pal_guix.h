/**
 * @file ra_display_pal_guix.h
 * @brief Optional GUIX bridge for the display PAL
 *
 * @deprecated GUIX retirement -- tracked by issue #81. The GUIX display path
 * is being replaced by the ra_reflow chrome renderer (issue #80). Do not add
 * new callers; this file is scheduled for wholesale removal once the chrome
 * migration lands.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Apps that drive an Eclipse GUIX UI through the PAL include this
 * header to convert the bound backend into the
 * ``display_driver_setup`` callback ``gx_display_create`` expects.
 *
 * The PAL stays out of the GUIX type universe until this header is
 * included so non-GUIX apps (and the host unit tests, which build
 * with ``RA_SIMULATOR_MODE``) do not need GUIX on the include path.
 *
 * Usage:
 *
 * @code
 * display_handle_t* d = nullptr;
 * (void)display_init(&app_cfg, &d);
 *
 * display_guix_attach_t attach;
 * (void)display_bind_guix(d, &attach);
 *
 * GX_DISPLAY display;
 * gx_display_create(&display, "panel", attach.setup_fn,
 *                   (GX_VALUE)attach.width_px,
 *                   (GX_VALUE)attach.height_px);
 * @endcode
 *
 * E-ink-style backends without a GUIX shim return
 * ``k_ra_err_not_supported`` from ``display_bind_guix`` until their
 * driver lands.
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

#include "ra_display_pal.h"
#include "ra_err.h"

/**
 * @struct display_guix_attach_t
 * @brief Output of ``display_bind_guix``.
 *
 * @details
 * Carries every argument ``gx_display_create`` needs beyond the
 * ``GX_DISPLAY*`` and name strings. The setup-fn slot is held as a
 * generic function pointer ``void (*)(void)`` so this header never
 * pulls in GUIX vendor types -- apps that drive GUIX cast it back
 * to the real ``UINT (*)(GX_DISPLAY*)`` at the call site.
 *
 * Example app-side use:
 *
 * @code
 * display_guix_attach_t a;
 * (void)display_bind_guix(d, &a);
 * gx_display_create(&display, "panel",
 *                   (UINT (*)(GX_DISPLAY*))a.setup_fn,
 *                   (GX_VALUE)a.width_px,
 *                   (GX_VALUE)a.height_px);
 * @endcode
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  void (*setup_fn)(void); /**< GUIX display_driver_setup callback, opaque. */
  uint16_t width_px;      /**< Panel width passed to gx_display_create.    */
  uint16_t height_px;     /**< Panel height passed to gx_display_create.   */
} display_guix_attach_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Bind the active backend's framebuffer to a GUIX driver
 *        shim and return everything ``gx_display_create`` needs.
 *
 * @details
 * The PAL hides which backend-specific GUIX shim is wired up. The
 * LCD backend forwards to the existing
 * ``ra_guix_display_driver_bind`` /
 * ``ra_guix_display_driver_setup`` pair in ``port/guix/``. Other
 * backends without a GUIX driver return ``k_ra_err_not_supported``
 * so callers can degrade gracefully.
 *
 * @param[in]  d   PAL handle returned by ``display_init``.
 * @param[out] out Filled with setup-fn + dimensions on success.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Attach populated.
 * @retval k_ra_err_null_ptr      ``d`` or ``out`` was NULL.
 * @retval k_ra_err_not_supported Backend has no GUIX shim today.
 *
 * @pre ``display_init`` has succeeded.
 * @pre ``out`` is writable.
 * @post On success the caller can pass ``out->setup_fn``,
 *       ``out->width_px``, ``out->height_px`` straight into
 *       ``gx_display_create``.
 * @post On non-ok return ``*out`` is untouched.
 *
 * @note Not thread-safe; call once during GUIX bring-up.
 *
 * @since 0.1.0
 */
[[nodiscard]] [[deprecated(
  "GUIX retirement -- see issue #81; migrating to ra_reflow chrome")]] ra_err_t
display_bind_guix(display_handle_t* d, display_guix_attach_t* out);

#ifdef __cplusplus
}
#endif
