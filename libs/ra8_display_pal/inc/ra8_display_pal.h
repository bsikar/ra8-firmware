/**
 * @file ra8_display_pal.h
 * @brief Display Platform Abstraction Layer for the RA8D2
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Backend-agnostic API that lets applications draw into a
 * framebuffer and push it to a panel without knowing which display
 * controller is on the other end. Today there are three backends:
 *
 *   - ``k_display_backend_lcd_ra8_glcdc`` -- thin wrapper over
 *     ``libs/ra8_hal/src/ra8_glcdc.c`` driving the EK-RA8D2 1024x600
 *     parallel-RGB TFT.
 *   - ``k_display_backend_eink_it8951`` -- stub for the future
 *     IT8951-compatible e-ink panel. Compiles and presents the same
 *     API surface, but returns ``k_ra8_err_not_supported`` from
 *     ``display_flush`` / ``display_get_framebuffer`` until the
 *     hardware-side driver lands.
 *
 * Apps swap backends by changing the ``iface`` field of their
 * ``display_cfg_t`` -- no other code touches the underlying
 * controller, so a future e-ink port is a single-line change in
 * the app's config.
 *
 * ## Layering
 *
 *     +-----------------------------+ display_init / get_framebuffer
 *     | application code            | display_flush(rect, hint)
 *     | (ra8_gfx / ra8_box chrome)    |
 *     +--------------+--------------+
 *                    |
 *                    v
 *     +-----------------------------+   one vtable per backend.
 *     | ra8_display_pal (this file)  |<- dispatches via display_backend_iface_t
 *     +--------------+--------------+
 *                    |
 *           +--------+---------+
 *           |                  |
 *           v                  v
 *     +------------+    +-------------+
 *     | LCD (glcdc)|    | e-ink stub  |
 *     +------------+    +-------------+
 *
 * ## Framebuffer ownership
 *
 * The caller owns the framebuffer storage and points the PAL at it
 * through ``cfg->framebuffer``. RGB565 is the canonical app-side
 * format; backends that want a different native depth (e.g. e-ink's
 * 4bpp greyscale) convert during ``display_flush``. This keeps app
 * code portable across backends.
 *
 * ## Refresh model
 *
 * - LCD panels scan continuously, so ``display_flush`` is essentially
 *   a no-op (it commits any pending DMA writes and returns).
 * - E-ink panels are bistable, so ``display_flush`` is where the real
 *   work happens: write the FB to the controller, then trigger an
 *   update with the chosen ``display_refresh_hint_t``.
 *
 * Applications that need to control speed-vs-quality on e-ink pass
 * the hint; LCD backends ignore it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Public enums
 * =============================================================================
 */

/**
 * @enum display_pixfmt_t
 * @brief Canonical pixel formats the PAL understands.
 *
 * @details
 * RGB565 is the only format LCD apps need today and is the canonical
 * choice for portable code. Greyscale formats are reserved for e-ink
 * backends that paint natively at lower depth -- the LCD backend
 * rejects them with ``k_ra8_err_not_supported``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_display_pixfmt_rgb565 = 0U, /**< 16 bpp, 5/6/5 packed.          */
  k_display_pixfmt_rgb888 = 1U, /**< 24 bpp, 8/8/8 packed.          */
  k_display_pixfmt_grey4  = 2U, /**< 4 bpp greyscale (2 px / byte). */
  k_display_pixfmt_grey1  = 3U, /**< 1 bpp greyscale (8 px / byte). */
} display_pixfmt_t;

/**
 * @enum display_refresh_hint_t
 * @brief Intent passed into ``display_flush``; backends map this onto
 *        whatever waveform/mode their controller offers.
 *
 * @details
 * Hint values describe the trade-off the caller wants -- not a
 * specific waveform name. On LCD backends every hint behaves the
 * same (flush is effectively a no-op). On e-ink the mapping is:
 *
 * - ``k_display_refresh_fast``    -> A2-like waveform (fast, ghosting OK).
 * - ``k_display_refresh_quality`` -> GC16-like waveform (slow, clean).
 * - ``k_display_refresh_init``    -> full INIT clear (once at boot).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_display_refresh_fast    = 0U, /**< Prioritise latency.       */
  k_display_refresh_quality = 1U, /**< Prioritise final quality. */
  k_display_refresh_init    = 2U, /**< Full reset of the panel.  */
} display_refresh_hint_t;

/* =============================================================================
 * Public structs
 * =============================================================================
 */

/**
 * @struct display_caps_t
 * @brief Capabilities a backend reports after ``display_init``.
 *
 * @details
 * Lets apps query the runtime properties of the bound backend without
 * coupling to its implementation. ``refresh_latency_us_typ`` is 0 for
 * scan-out panels (LCD) and tens to hundreds of milliseconds for
 * e-ink in quality mode -- apps that drive animation should branch
 * on it rather than blindly issuing one ``display_flush`` per frame.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t         width_px;                /**< Display visible width.            */
  uint16_t         height_px;               /**< Display visible height.           */
  display_pixfmt_t pixfmt;                  /**< App-facing canonical format.      */
  uint32_t         stride_bytes;            /**< Framebuffer row stride in bytes.  */
  uint32_t         refresh_latency_us_typ;  /**< Typical flush() cost in usec.     */
  bool             supports_partial_update; /**< true => sub-rect flush honoured.  */
  bool             continuous_refresh;      /**< true => panel scans continuously. */
} display_caps_t;

/**
 * @struct display_fb_t
 * @brief Framebuffer descriptor returned by ``display_get_framebuffer``.
 *
 * @details
 * ``pixels`` points at the same buffer the caller passed in via
 * ``display_cfg_t.framebuffer``. The backend may have re-validated
 * alignment / size; the values reported here are authoritative for
 * subsequent paint loops.
 *
 * @since 0.1.0
 */
typedef struct {
  void*            pixels;       /**< Base of pixel data (RGB565 unless noted). */
  uint16_t         width_px;     /**< Framebuffer width.                        */
  uint16_t         height_px;    /**< Framebuffer height.                       */
  uint32_t         stride_bytes; /**< Row stride in bytes.                      */
  display_pixfmt_t pixfmt;       /**< Pixel format of ``pixels``.               */
} display_fb_t;

/**
 * @struct display_rect_t
 * @brief Rectangle in framebuffer coordinates passed into
 *        ``display_flush``.
 *
 * @details
 * Coordinates are inclusive on the upper-left, exclusive on the
 * lower-right (standard half-open interval).  Use
 * ``display_full_rect`` to get the rectangle covering the whole
 * framebuffer.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t x; /**< Left edge in pixels. */
  uint16_t y; /**< Top edge in pixels.  */
  uint16_t w; /**< Width in pixels.     */
  uint16_t h; /**< Height in pixels.    */
} display_rect_t;

/**
 * @struct display_backend_iface
 * @brief Opaque to applications -- backends implement this internally
 *        and export a const instance through their own header.
 *
 * @details
 * Apps never construct an ``display_backend_iface_t`` themselves;
 * they reference the const instance their chosen backend exports
 * (e.g. ``k_display_backend_lcd_ra8_glcdc``) and pass its address
 * through ``display_cfg_t.iface``.
 *
 * @since 0.1.0
 */
typedef struct display_backend_iface display_backend_iface_t;

/**
 * @struct display_handle
 * @brief Opaque handle returned by ``display_init``.
 *
 * @details
 * All subsequent PAL calls take a ``display_handle_t*`` so the PAL
 * knows which backend to dispatch through. The concrete definition
 * lives in ``src/ra8_display_pal_internal.h`` and is invisible to
 * applications.
 *
 * @since 0.1.0
 */
typedef struct display_handle display_handle_t;

/**
 * @struct display_cfg_t
 * @brief Configuration descriptor passed into ``display_init``.
 *
 * @details
 * The caller fills this with the backend they want, the framebuffer
 * storage they want the backend to use, and the geometry the
 * application is targeting. Each backend validates the fields and
 * returns an error on mismatch (e.g. an LCD backend rejects a 1bpp
 * grey format).
 *
 * @invariant ``iface``, ``framebuffer`` are non-NULL.
 * @invariant ``framebuffer_bytes >= stride_bytes * height_px``.
 *
 * @since 0.1.0
 */
typedef struct {
  /** @brief Backend vtable (one of the ``k_display_backend_*`` consts). */
  const display_backend_iface_t* iface;
  /** @brief Application framebuffer storage (caller owns). */
  void* framebuffer;
  /** @brief Size of the framebuffer buffer in bytes. */
  uint32_t framebuffer_bytes;
  /** @brief Visible framebuffer width in pixels. */
  uint16_t width_px;
  /** @brief Visible framebuffer height in pixels. */
  uint16_t height_px;
  /** @brief Pixel format of ``framebuffer``. */
  display_pixfmt_t pixfmt;
  /**
   * @brief Panel RGB timing for parallel-RGB backends, opaque here so the
   * generic PAL stays HAL-agnostic. The LCD/GLCDC backend casts it to
   * ``const ra8_glcdc_timing_t*``; the board's BSP supplies it (e.g.
   * ``&s_ra8_panel_ek_ra8d2_timing``). Leave ``nullptr`` for backends that
   * do not drive panel timing (host window, e-ink).
   */
  const void* panel_timing;
} display_cfg_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Initialise the display PAL and bring the selected backend
 *        up to a state where ``display_flush`` succeeds.
 *
 * @details
 * Validates ``cfg``, dispatches into the backend's init function
 * (which on the LCD path runs the full GLCDC bring-up sequence:
 * panel power-on, GLCDC pin/clock setup, settle delay, controller
 * init, BG colour clear, ``start(true)``, ``layer1_show``), then
 * stores the resulting context inside the module-static handle and
 * returns its address through ``out_handle``.
 *
 * @param[in]  cfg        Backend + geometry descriptor (see struct).
 * @param[out] out_handle Filled with the PAL handle on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Backend ready; ``out_handle`` populated.
 * @retval k_ra8_err_null_ptr      ``cfg`` / ``cfg->iface`` /
 *                                ``cfg->framebuffer`` / ``out_handle`` NULL.
 * @retval k_ra8_err_invalid_arg   Width / height / pixfmt rejected by backend.
 * @retval k_ra8_err_busy          PAL already initialised; deinit first.
 * @retval k_ra8_err_not_supported Pixel format unsupported by chosen backend.
 *
 * @pre Power to the underlying controller is on (true at reset).
 * @pre The caller's framebuffer buffer is alive and aligned.
 * @post On success the handle is valid until ``display_deinit`` is called.
 * @post On any non-ok return ``out_handle`` is untouched.
 *
 * @note Not thread-safe; single-shot startup helper.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_init(const display_cfg_t* cfg, display_handle_t** out_handle);

/**
 * @brief Query the bound backend's runtime capabilities.
 *
 * @param[in]  d   PAL handle returned by ``display_init``.
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Capabilities written to ``*out``.
 * @retval k_ra8_err_null_ptr Either argument was NULL.
 *
 * @pre ``display_init`` has succeeded.
 * @pre ``out`` is writable.
 * @post On success ``*out`` mirrors what the backend reported at init time.
 * @post No internal state is mutated.
 *
 * @note Thread-safe (pure read of immutable state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_get_caps(const display_handle_t* d, display_caps_t* out);

/**
 * @brief Return the framebuffer descriptor the backend is targeting.
 *
 * @details
 * The pointer in ``out->pixels`` is the same buffer the caller
 * passed via ``cfg->framebuffer``. The width / height / stride may
 * differ from the values the caller passed if the backend rounded
 * them up to satisfy an alignment requirement.
 *
 * @param[in]  d   PAL handle.
 * @param[out] out Filled with the framebuffer descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Descriptor written.
 * @retval k_ra8_err_null_ptr      Either argument was NULL.
 * @retval k_ra8_err_not_supported Backend does not expose a
 *                                CPU-writable framebuffer (e.g. the
 *                                e-ink stub before it is wired up).
 *
 * @pre ``display_init`` has succeeded.
 * @pre ``out`` is writable.
 * @post On success the caller may write ``out->width_px *
 *       out->stride_bytes`` bytes starting at ``out->pixels``.
 *
 * @note Thread-safe relative to other PAL calls; not safe to call
 *       in parallel with ``display_deinit``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_get_framebuffer(display_handle_t* d, display_fb_t* out);

/**
 * @brief Commit pending framebuffer changes to the panel.
 *
 * @details
 * On scan-out backends (LCD) this is a memory barrier plus an
 * optional cache clean. On e-ink backends this is the real work
 * step: copy ``rect`` from the framebuffer into the controller's
 * internal buffer, then trigger a display update with the waveform
 * mapped from ``hint``.
 *
 * @param[in] d    PAL handle.
 * @param[in] rect Region of the framebuffer to push. Use
 *                 ``display_full_rect(d)`` for a whole-screen update.
 * @param[in] hint Intent driving the waveform/mode selection.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Flush completed (or queued for the
 *                                next scan on continuous-refresh panels).
 * @retval k_ra8_err_null_ptr      ``d`` was NULL.
 * @retval k_ra8_err_invalid_arg   ``rect`` extends outside the framebuffer.
 * @retval k_ra8_err_not_supported Backend does not implement flush yet
 *                                (e-ink stub returns this).
 *
 * @pre ``display_init`` has succeeded.
 * @pre ``rect.x + rect.w <= caps.width_px`` and likewise for y / h.
 * @post On success ``rect`` of the panel reflects framebuffer state.
 * @post On non-ok return the panel is unchanged.
 *
 * @note Thread-safe relative to PAL state; the underlying controller
 *       is single-threaded.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
display_flush(display_handle_t* d, display_rect_t rect, display_refresh_hint_t hint);

/**
 * @brief Clear the entire framebuffer to ``color``.
 *
 * @details
 * Writes ``color`` into every pixel of the framebuffer using the
 * backend's native paint loop. Does NOT call ``display_flush`` --
 * callers that want the cleared frame visible on e-ink must follow
 * up with a ``display_flush(d, display_full_rect(d), k_display_refresh_quality)``.
 *
 * @param[in] d     PAL handle.
 * @param[in] color Pixel value to write (interpretation depends on
 *                  ``caps.pixfmt``).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Buffer cleared.
 * @retval k_ra8_err_null_ptr ``d`` was NULL.
 *
 * @pre ``display_init`` has succeeded.
 * @pre The framebuffer pointer is reachable.
 * @post Every pixel of the framebuffer equals ``color``.
 * @post Panel state is unchanged until the next ``display_flush``.
 *
 * @note Not thread-safe with concurrent paint loops.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_clear(display_handle_t* d, uint32_t color);

/**
 * @brief Release the backend and invalidate the PAL handle.
 *
 * @details
 * Mirrors ``display_init``: tears down the controller, releases the
 * static handle, and leaves the PAL ready to be re-initialised with
 * a different backend.
 *
 * @param[in] d PAL handle to release.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Backend released; ``d`` is now stale.
 * @retval k_ra8_err_null_ptr ``d`` was NULL.
 *
 * @pre ``display_init`` has succeeded.
 * @pre No paint loop is in flight.
 * @post Subsequent calls through ``d`` return ``k_ra8_err_invalid_arg``.
 * @post A fresh ``display_init`` will succeed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_deinit(display_handle_t* d);

/**
 * @brief Convenience helper returning the rect covering the whole
 *        framebuffer.
 *
 * @details Short-hand for the common case where the caller wants to
 *          flush the entire framebuffer.  Equivalent to
 *          ``{0, 0, caps.width_px, caps.height_px}`` if the handle
 *          is valid, or an all-zero rectangle if it is not.
 *
 * @param[in] d PAL handle.
 *
 * @return display_rect_t {0, 0, caps.width_px, caps.height_px}
 *         on success, or an all-zero rectangle if ``d`` is NULL.
 * @retval Full-screen rect when ``d`` is a live handle.
 * @retval All-zero rect when ``d`` is NULL or stale.
 *
 * @pre ``display_init`` has succeeded.
 * @pre ``d`` is non-NULL.
 * @post No state is mutated.
 * @post Returned rectangle has w/h equal to current caps.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
display_rect_t display_full_rect(const display_handle_t* d);

#ifdef __cplusplus
}
#endif
