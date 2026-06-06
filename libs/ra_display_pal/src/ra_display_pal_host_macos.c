/**
 * @file ra_display_pal_host_macos.c
 * @brief Host macOS window backend implementation for the display PAL
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Portable C implementation of the ``display_backend_iface_t`` vtable
 * for ``k_display_backend_host_macos``. All windowing is delegated to
 * the C-ABI shim in ``ra_display_pal_host_macos_window.h`` so this TU
 * contains no Objective-C and compiles on every platform; the real
 * AppKit work lives in the ``.m`` shim linked only by the desktop demo.
 *
 * The backend mirrors the LCD backend's shape: validate the cfg, snapshot
 * the geometry into a single module-static context, and answer
 * ``get_caps`` / ``get_framebuffer`` as O(1) reads. ``flush`` forwards
 * the framebuffer to the window shim; ``hint`` is ignored because a
 * desktop window, like an LCD, refreshes continuously.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_display_pal_host_macos.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal.h"
#include "ra_display_pal_host_macos_window.h"
#include "ra_display_pal_internal.h"
#include "ra_err.h"
#include "ra_log.h"

/* The desktop GUIX bridge is opt-in: only the host GUIX build defines
 * RA_DISPLAY_PAL_HOST_GUIX and puts the GUIX port headers on the include
 * path. Non-GUIX host builds (the ra_gfx demo, the unit tests) leave
 * bind_guix NULL, exactly like the LCD backend without its GUIX shim. */
#if defined(RA_DISPLAY_PAL_HOST_GUIX)
#include "gx_display_driver_host.h"
#endif

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_host_macos";

/**
 * @enum ra_display_pal_host_macos_const_t
 * @brief Numeric constants used by the host macOS backend.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_macos_rgb565_bpp = 2U, /**< Bytes per RGB565 pixel. */
} ra_display_pal_host_macos_const_t;

/**
 * @struct macos_ctx_t
 * @brief Host macOS backend private context attached to the PAL handle.
 *
 * @details
 * Carries the bound framebuffer descriptor, a copy of the caps report,
 * and the opaque window handle returned by the shim.
 *
 * @since 0.1.0
 */
typedef struct {
  display_caps_t     caps;        /**< Snapshotted capabilities.   */
  display_fb_t       fb;          /**< Bound framebuffer view.     */
  ra_macos_window_t* win;         /**< Opaque desktop window.      */
  bool               initialised; /**< Set after a successful init. */
} macos_ctx_t;

/** @brief Single host macOS backend context -- one window per process. */
static macos_ctx_t s_macos_ctx;

/* =============================================================================
 * Internal helpers (kept short for NASA P10 Rule 4)
 * =============================================================================
 */

/**
 * @brief Validate the caller's ``display_cfg_t`` for the host backend.
 *
 * @details
 * Applies the same rules as the LCD / e-ink backends so drawing code is
 * portable: non-NULL framebuffer, non-zero dimensions, RGB565 only, and
 * a buffer large enough for the geometry.
 *
 * @param[in] cfg Caller configuration (non-NULL; checked by caller).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Configuration accepted.
 * @retval k_ra_err_null_ptr      ``cfg->framebuffer`` was NULL.
 * @retval k_ra_err_invalid_arg   Dimensions or buffer size out of range.
 * @retval k_ra_err_not_supported Pixel format other than RGB565.
 *
 * @pre cfg != NULL (caller checks).
 * @pre cfg->framebuffer null check happens here.
 * @post No state mutated.
 * @post Return depends solely on cfg.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_macos_validate_cfg(const display_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg->framebuffer, s_tag, "cfg->framebuffer");
  if (cfg->width_px == 0U || cfg->height_px == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->pixfmt != k_display_pixfmt_rgb565) {
    return k_ra_err_not_supported;
  }
  const uint32_t need_bytes =
    (uint32_t)cfg->width_px * (uint32_t)cfg->height_px * (uint32_t)k_macos_rgb565_bpp;
  if (cfg->framebuffer_bytes < need_bytes) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Snapshot the validated cfg into ``s_macos_ctx``.
 *
 * @details
 * Lets ``get_caps`` / ``get_framebuffer`` answer without touching the
 * caller's cfg again. ``continuous_refresh`` is true (a desktop window
 * repaints on demand, like a scan-out panel) and ``refresh_latency``
 * is reported as zero.
 *
 * @param[in] cfg Validated caller configuration.
 *
 * @pre ``internal_macos_validate_cfg`` returned ``k_ra_ok``.
 * @pre cfg is alive for the duration of the call.
 * @post ``s_macos_ctx`` mirrors cfg's geometry + framebuffer pointer.
 * @post ``s_macos_ctx.initialised == true``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void internal_macos_snapshot(const display_cfg_t* cfg)
{
  const uint32_t stride                    = (uint32_t)cfg->width_px * (uint32_t)k_macos_rgb565_bpp;
  s_macos_ctx.caps.width_px                = cfg->width_px;
  s_macos_ctx.caps.height_px               = cfg->height_px;
  s_macos_ctx.caps.pixfmt                  = k_display_pixfmt_rgb565;
  s_macos_ctx.caps.stride_bytes            = stride;
  s_macos_ctx.caps.refresh_latency_us_typ  = 0U;
  s_macos_ctx.caps.supports_partial_update = false;
  s_macos_ctx.caps.continuous_refresh      = true;

  s_macos_ctx.fb.pixels       = cfg->framebuffer;
  s_macos_ctx.fb.width_px     = cfg->width_px;
  s_macos_ctx.fb.height_px    = cfg->height_px;
  s_macos_ctx.fb.stride_bytes = stride;
  s_macos_ctx.fb.pixfmt       = k_display_pixfmt_rgb565;
  s_macos_ctx.initialised     = true;
}

/**
 * @brief Confirm a rectangle stays inside the active framebuffer.
 *
 * @details
 * Split into four sequential comparisons so MC/DC vectors are trivial,
 * matching the LCD backend's bounds check.
 *
 * @param[in] caps Backend capabilities.
 * @param[in] r    Candidate rectangle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Rectangle fits.
 * @retval k_ra_err_invalid_arg Rectangle would step out of bounds.
 *
 * @pre caps reflects the bound framebuffer.
 * @pre r was supplied by the caller.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_macos_check_rect(const display_caps_t* caps, display_rect_t r)
{
  if (r.x > caps->width_px) {
    return k_ra_err_invalid_arg;
  }
  if (r.y > caps->height_px) {
    return k_ra_err_invalid_arg;
  }
  if ((uint32_t)r.x + (uint32_t)r.w > (uint32_t)caps->width_px) {
    return k_ra_err_invalid_arg;
  }
  if ((uint32_t)r.y + (uint32_t)r.h > (uint32_t)caps->height_px) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Vtable callbacks
 * =============================================================================
 */

/**
 * @brief Host backend init -- validate, open the window, snapshot.
 *
 * @details Vtable entry for ``k_display_backend_host_macos``. The heavy
 *          lifting is in the helpers above so the body stays under the
 *          NASA P10 Rule 4 size budget.
 *
 * @param[in]  cfg     Caller configuration.
 * @param[out] out_ctx Filled with the backend context pointer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Backend ready; window open.
 * @retval k_ra_err_null_ptr       Required pointer was NULL.
 * @retval k_ra_err_invalid_arg    Dimensions / buffer size invalid.
 * @retval k_ra_err_not_supported  Pixel format other than RGB565.
 * @retval k_ra_err_busy           Backend already initialised.
 * @retval k_ra_err_hw_init_failed Window could not be opened.
 *
 * @pre PAL dispatcher has not bound a backend yet.
 * @pre Called from the main thread.
 * @post On success a window is open and ``s_macos_ctx.initialised`` is true.
 * @post On failure no window remains open.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t macos_init(const display_cfg_t* cfg, void** out_ctx)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA_CHECK_NULL_PTR(out_ctx, s_tag, "out_ctx");

  const ra_err_t v = internal_macos_validate_cfg(cfg);
  if (v != k_ra_ok) {
    return v;
  }
  if (s_macos_ctx.initialised) {
    ra_log_error(s_tag, "macos_init: already initialised");
    return k_ra_err_busy;
  }

  ra_macos_window_t* win = nullptr;
  const ra_err_t     we =
    ra_macos_window_open(cfg->width_px, cfg->height_px, "ra8d2 display PAL (host macOS)", &win);
  if (we != k_ra_ok) {
    return we;
  }

  s_macos_ctx.win = win;
  internal_macos_snapshot(cfg);
  *out_ctx = &s_macos_ctx;
  ra_log_info(s_tag, "macos_init: host window backend bound");
  return k_ra_ok;
}

/**
 * @brief Vtable get_caps -- O(1) copy from ``s_macos_ctx.caps``.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Capabilities snapshot.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Caps written.
 * @retval k_ra_err_null_ptr Either argument was NULL.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre out is writable.
 * @post ``*out`` equals the stored caps.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t macos_get_caps(const void* ctx, display_caps_t* out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA_CHECK_NULL_PTR(out, s_tag, "out");
  *out = ((const macos_ctx_t*)ctx)->caps;
  return k_ra_ok;
}

/**
 * @brief Vtable get_framebuffer -- hand back the FB descriptor.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Framebuffer descriptor.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Descriptor written.
 * @retval k_ra_err_null_ptr Either argument was NULL.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre out is writable.
 * @post ``out->pixels`` matches ``cfg.framebuffer``.
 * @post No state mutated.
 *
 * @note Thread-safe relative to other PAL calls.
 *
 * @since 0.1.0
 */
static ra_err_t macos_get_framebuffer(void* ctx, display_fb_t* out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA_CHECK_NULL_PTR(out, s_tag, "out");
  *out = ((const macos_ctx_t*)ctx)->fb;
  return k_ra_ok;
}

/**
 * @brief Vtable flush -- upload the framebuffer to the window.
 *
 * @details
 * Validates ``rect`` for parity with the panel backends, then presents
 * the whole framebuffer through the window shim. ``hint`` is ignored: a
 * desktop window repaints continuously, so there is no waveform to pick.
 *
 * @param[in] ctx  Backend context.
 * @param[in] rect Region the caller painted (validated, not sub-copied).
 * @param[in] hint Refresh-mode intent (ignored).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Frame uploaded.
 * @retval k_ra_err_null_ptr    ctx was NULL.
 * @retval k_ra_err_invalid_arg rect leaves the framebuffer.
 * @retval (from shim)          Forwarded from ``ra_macos_window_present``.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre rect lies inside the framebuffer.
 * @post The window will repaint with the current framebuffer.
 * @post No backend state mutated.
 *
 * @note Not thread-safe; call from the main thread.
 *
 * @since 0.1.0
 */
static ra_err_t macos_flush(void* ctx, display_rect_t rect, display_refresh_hint_t hint)
{
  (void)hint; /* Desktop windows repaint continuously; hint is informational. */
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  macos_ctx_t*   c = (macos_ctx_t*)ctx;
  const ra_err_t v = internal_macos_check_rect(&c->caps, rect);
  if (v != k_ra_ok) {
    return v;
  }
  return ra_macos_window_present(c->win,
                                 c->fb.pixels,
                                 c->fb.width_px,
                                 c->fb.height_px,
                                 c->fb.stride_bytes);
}

/**
 * @brief Vtable clear -- fill the framebuffer with a uniform RGB565 value.
 *
 * @details Tight CPU loop matching the LCD backend. Does not present;
 *          the caller follows up with ``display_flush`` to show it.
 *
 * @param[in] ctx   Backend context.
 * @param[in] color Pixel value (low 16 bits used).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Buffer cleared.
 * @retval k_ra_err_null_ptr ctx was NULL.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre Framebuffer is reachable from the CPU.
 * @post Every pixel of the FB equals ``color & 0xFFFF``.
 * @post Window state is unchanged until the next ``display_flush``.
 *
 * @note Not thread-safe with concurrent paint loops.
 *
 * @since 0.1.0
 */
static ra_err_t macos_clear(void* ctx, uint32_t color)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  macos_ctx_t*   c            = (macos_ctx_t*)ctx;
  uint16_t*      pixels       = (uint16_t*)c->fb.pixels;
  const uint32_t pixels_total = (uint32_t)c->fb.width_px * (uint32_t)c->fb.height_px;
  const uint16_t rgb565       = (uint16_t)(color & 0xFFFFU);
  for (uint32_t i = 0U; i < pixels_total; ++i) {
    pixels[i] = rgb565;
  }
  return k_ra_ok;
}

/**
 * @brief Vtable deinit -- close the window and drop state.
 *
 * @param[in] ctx Backend context.
 *
 * @return ra_err_t Error code from ``ra_macos_window_close`` (or
 *         ``k_ra_err_null_ptr`` if ctx was NULL).
 * @retval k_ra_ok           Tear-down succeeded.
 * @retval k_ra_err_null_ptr ctx was NULL.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre No concurrent paint loops.
 * @post ``s_macos_ctx.initialised == false``.
 * @post ``s_macos_ctx.win == nullptr``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t macos_deinit(void* ctx)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  macos_ctx_t*   c   = (macos_ctx_t*)ctx;
  const ra_err_t err = ra_macos_window_close(c->win);
  c->win             = nullptr;
  c->initialised     = false;
  c->fb.pixels       = nullptr;
  return err;
}

/* =============================================================================
 * Host-only helpers (outside the vtable)
 * =============================================================================
 */

ra_err_t ra_display_pal_host_macos_pump(bool* out_should_close)
{
  RA_CHECK_NULL_PTR(out_should_close, s_tag, "out_should_close");
  if (!s_macos_ctx.initialised) {
    return k_ra_err_not_initialized;
  }
  return ra_macos_window_pump(s_macos_ctx.win, out_should_close);
}

ra_err_t ra_display_pal_host_macos_poll_click(bool* out_has_click, uint16_t* out_x, uint16_t* out_y)
{
  RA_CHECK_NULL_PTR(out_has_click, s_tag, "out_has_click");
  RA_CHECK_NULL_PTR(out_x, s_tag, "out_x");
  RA_CHECK_NULL_PTR(out_y, s_tag, "out_y");
  if (!s_macos_ctx.initialised) {
    return k_ra_err_not_initialized;
  }
  return ra_macos_window_poll_click(s_macos_ctx.win, out_has_click, out_x, out_y);
}

#if defined(RA_DISPLAY_PAL_HOST_GUIX)

/**
 * @brief Vtable bind_guix -- forward to the host GUIX 565rgb driver.
 *
 * @details
 * Mirrors the LCD backend's ``lcd_bind_guix``: stash the bound
 * framebuffer with the host GUIX driver and hand back its setup-fn
 * (cast to the PAL's opaque slot) plus the panel dimensions, ready for
 * ``gx_display_create``.
 *
 * @param[in]  ctx           Backend context.
 * @param[out] out_setup_fn  Receives the GUIX setup-fn as an opaque ptr.
 * @param[out] out_width_px  Receives the framebuffer width.
 * @param[out] out_height_px Receives the framebuffer height.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Bound; outputs populated.
 * @retval k_ra_err_null_ptr Any argument was NULL.
 * @retval (from host driver) Forwarded from host_guix_display_driver_bind.
 *
 * @pre ``macos_init`` has succeeded.
 * @pre The output pointers are writable.
 * @post ``*out_setup_fn`` can be cast back to ``UINT (*)(GX_DISPLAY*)``.
 * @post On non-ok return outputs are untouched.
 *
 * @note Not thread-safe; single-shot at GUIX bring-up.
 *
 * @since 0.1.0
 */
static ra_err_t
macos_bind_guix(void* ctx, void** out_setup_fn, uint16_t* out_width_px, uint16_t* out_height_px)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA_CHECK_NULL_PTR(out_setup_fn, s_tag, "out_setup_fn");
  RA_CHECK_NULL_PTR(out_width_px, s_tag, "out_width_px");
  RA_CHECK_NULL_PTR(out_height_px, s_tag, "out_height_px");

  const macos_ctx_t* c = (const macos_ctx_t*)ctx;
  const ra_err_t err = host_guix_display_driver_bind(c->fb.pixels, c->fb.width_px, c->fb.height_px);
  if (err != k_ra_ok) {
    return err;
  }
  *out_setup_fn  = (void*)host_guix_display_driver_setup;
  *out_width_px  = c->fb.width_px;
  *out_height_px = c->fb.height_px;
  return k_ra_ok;
}

#endif /* RA_DISPLAY_PAL_HOST_GUIX */

/* =============================================================================
 * Public iface instance
 * =============================================================================
 */

const display_backend_iface_t k_display_backend_host_macos = {
  .init            = macos_init,
  .get_caps        = macos_get_caps,
  .get_framebuffer = macos_get_framebuffer,
  .flush           = macos_flush,
  .clear           = macos_clear,
  .deinit          = macos_deinit,
#if defined(RA_DISPLAY_PAL_HOST_GUIX)
  .bind_guix = macos_bind_guix,
#else
  /* No GUIX shim in non-GUIX host builds; display_bind_guix returns
   * k_ra_err_not_supported. */
  .bind_guix = nullptr,
#endif
};
