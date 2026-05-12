/**
 * @file ra_display_pal_eink.c
 * @brief E-ink (IT8951) backend stub for the display PAL
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Compiles and links so apps can target this backend at build time
 * before the silicon is on the bench. ``init`` validates the cfg
 * and parks state; ``get_caps`` reports the configured dimensions;
 * every other vtable entry returns ``k_ra_err_not_supported`` until
 * the SPI + waveform driver lands.
 *
 * The intent is that the PAL contract is verifiable end-to-end --
 * apps can swap the iface pointer to this backend and discover at
 * link time whether their drawing code is backend-portable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_display_pal_eink.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal.h"
#include "ra_display_pal_internal.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_eink";

/**
 * @struct eink_ctx_t
 * @brief E-ink backend context. Currently mirrors the LCD layout so
 *        the future driver only has to flesh out ``flush``.
 *
 * @since 0.1.0
 */
typedef struct {
  display_caps_t caps;
  display_fb_t   fb;
  bool           initialised;
} eink_ctx_t;

/** @brief Single e-ink backend context. */
static eink_ctx_t s_eink_ctx;

/**
 * @enum ra_display_pal_eink_const_t
 * @brief Internal numeric constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_eink_rgb565_bpp         = 2U,
  k_eink_refresh_quality_us = 450000U, /**< Typical GC16 latency. */
} ra_display_pal_eink_const_t;

/* =============================================================================
 * Vtable callbacks
 * =============================================================================
 */

/**
 * @brief Accept a sane cfg and stash it for ``get_caps`` / ``get_framebuffer``.
 *
 * @details
 * The stub validates the same fields the LCD backend validates so
 * apps can find sizing bugs early. No SPI / panel traffic happens.
 *
 * @param[in]  cfg     Caller configuration.
 * @param[out] out_ctx Filled with the backend context pointer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Cfg accepted; ctx returned.
 * @retval k_ra_err_null_ptr      Required pointer was NULL.
 * @retval k_ra_err_invalid_arg   Dimensions / buffer too small.
 * @retval k_ra_err_not_supported Pixel format not RGB565 (the
 *                                canonical app-side format).
 *
 * @pre None.
 * @pre None.
 * @post On success ``s_eink_ctx`` mirrors cfg.
 * @post On failure ``s_eink_ctx`` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
/**
 * @brief Validate cfg using the same rules as the LCD backend.
 *
 * @details Split out so ``eink_init`` stays under the clang-tidy
 *          readability-function-size threshold.
 *
 * @param[in] cfg Caller configuration (must not be NULL).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Cfg accepted.
 * @retval k_ra_err_null_ptr      ``cfg->framebuffer`` was NULL.
 * @retval k_ra_err_invalid_arg   Dimensions / buffer size out of range.
 * @retval k_ra_err_not_supported Pixel format not RGB565.
 *
 * @pre cfg != NULL (caller checks).
 * @pre cfg->framebuffer null check is performed here.
 * @post No state mutated.
 * @post Return depends solely on cfg.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_eink_validate_cfg(const display_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg->framebuffer, s_tag, "cfg->framebuffer");
  if (cfg->width_px == 0U || cfg->height_px == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->pixfmt != k_display_pixfmt_rgb565) {
    return k_ra_err_not_supported;
  }
  const uint32_t need_bytes =
    (uint32_t)cfg->width_px * (uint32_t)cfg->height_px * (uint32_t)k_eink_rgb565_bpp;
  if (cfg->framebuffer_bytes < need_bytes) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Snapshot cfg into the static e-ink context.
 *
 * @details
 * Mirrors the LCD backend's snapshot helper: copies cfg geometry
 * into the static ``s_eink_ctx``, computes stride, and reports the
 * known IT8951 GC16 latency in ``caps.refresh_latency_us_typ`` so
 * apps can branch on it.
 *
 * @param[in] cfg Validated caller configuration.
 *
 * @pre ``internal_eink_validate_cfg`` returned ``k_ra_ok``.
 * @pre cfg is alive for the duration of the call.
 * @post ``s_eink_ctx`` mirrors cfg.
 * @post ``s_eink_ctx.initialised == true``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void internal_eink_snapshot(const display_cfg_t* cfg)
{
  const uint32_t stride                   = (uint32_t)cfg->width_px * (uint32_t)k_eink_rgb565_bpp;
  s_eink_ctx.caps.width_px                = cfg->width_px;
  s_eink_ctx.caps.height_px               = cfg->height_px;
  s_eink_ctx.caps.pixfmt                  = k_display_pixfmt_rgb565;
  s_eink_ctx.caps.stride_bytes            = stride;
  s_eink_ctx.caps.refresh_latency_us_typ  = (uint32_t)k_eink_refresh_quality_us;
  s_eink_ctx.caps.supports_partial_update = true;
  s_eink_ctx.caps.continuous_refresh      = false;

  s_eink_ctx.fb.pixels       = cfg->framebuffer;
  s_eink_ctx.fb.width_px     = cfg->width_px;
  s_eink_ctx.fb.height_px    = cfg->height_px;
  s_eink_ctx.fb.stride_bytes = stride;
  s_eink_ctx.fb.pixfmt       = k_display_pixfmt_rgb565;
  s_eink_ctx.initialised     = true;
}

/**
 * @brief E-ink stub backend init -- validates cfg, snapshots state.
 *
 * @details
 * Vtable entry for ``k_display_backend_eink_it8951``. The stub does
 * no SPI / waveform traffic; once silicon is available this body
 * will bring up SPI, read the IT8951 panel-info block, and reserve
 * the host-side conversion buffer.
 *
 * @param[in]  cfg     Caller configuration.
 * @param[out] out_ctx Filled with the backend context pointer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Cfg accepted.
 * @retval k_ra_err_null_ptr      cfg / out_ctx / cfg->framebuffer NULL.
 * @retval k_ra_err_invalid_arg   Dimensions or buffer size invalid.
 * @retval k_ra_err_not_supported Pixel format not RGB565.
 *
 * @pre PAL dispatcher has not yet bound a backend.
 * @pre cfg is alive for the call.
 * @post ``s_eink_ctx`` mirrors cfg.
 * @post ``s_eink_ctx.initialised == true`` on success.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_init(const display_cfg_t* cfg, void** out_ctx)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA_CHECK_NULL_PTR(out_ctx, s_tag, "out_ctx");
  const ra_err_t v = internal_eink_validate_cfg(cfg);
  if (v != k_ra_ok) {
    return v;
  }
  internal_eink_snapshot(cfg);
  *out_ctx = &s_eink_ctx;
  ra_log_info(s_tag, "eink_init: stub backend bound (flush returns not_supported)");
  return k_ra_ok;
}

/**
 * @brief Vtable get_caps -- O(1) copy from ``s_eink_ctx.caps``.
 *
 * @details The stub already knows what the panel will report; this
 *          mirrors the LCD backend's caps shape for callers writing
 *          backend-agnostic code.
 *
 * @param[in]  ctx Backend context (validated upstream).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Caps written.
 * @retval k_ra_err_null_ptr Either argument was NULL.
 *
 * @pre ``eink_init`` has succeeded.
 * @pre out is writable.
 * @post ``*out`` equals the stored caps.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_get_caps(const void* ctx, display_caps_t* out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA_CHECK_NULL_PTR(out, s_tag, "out");
  *out = ((const eink_ctx_t*)ctx)->caps;
  return k_ra_ok;
}

/**
 * @brief Vtable get_framebuffer -- stub returns ``k_ra_err_not_supported``.
 *
 * @details Once the hardware driver lands this will hand out the
 *          RGB565 conversion buffer. The stub refuses so callers
 *          that paint directly into the FB see the gap at run-time.
 *
 * @param[in]  ctx Backend context (unused in the stub).
 * @param[out] out Framebuffer descriptor (unused in the stub).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_not_supported Always (stub).
 *
 * @pre ``eink_init`` has succeeded.
 * @pre out is writable.
 * @post No state mutated.
 * @post ``*out`` is untouched.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_get_framebuffer(void* ctx, display_fb_t* out)
{
  (void)ctx;
  (void)out;
  return k_ra_err_not_supported;
}

/**
 * @brief Vtable flush -- stub returns ``k_ra_err_not_supported``.
 *
 * @details Once the hardware driver lands this will copy ``rect``
 *          from the app FB into the IT8951's internal buffer and
 *          trigger an update using the waveform mapped from ``hint``.
 *
 * @param[in] ctx  Backend context (unused).
 * @param[in] rect Region of the FB to push (unused).
 * @param[in] hint Refresh-mode intent (unused).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_not_supported Always (stub).
 *
 * @pre ``eink_init`` has succeeded.
 * @pre rect lies inside the framebuffer.
 * @post Panel state is unchanged.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_flush(void* ctx, display_rect_t rect, display_refresh_hint_t hint)
{
  (void)ctx;
  (void)rect;
  (void)hint;
  return k_ra_err_not_supported;
}

/**
 * @brief Vtable clear -- stub returns ``k_ra_err_not_supported``.
 *
 * @details Once the hardware driver lands this will fill the host
 *          conversion buffer with ``color`` and queue a refresh.
 *
 * @param[in] ctx   Backend context (unused).
 * @param[in] color Pixel value (unused).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_not_supported Always (stub).
 *
 * @pre ``eink_init`` has succeeded.
 * @pre None.
 * @post Panel state is unchanged.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_clear(void* ctx, uint32_t color)
{
  (void)ctx;
  (void)color;
  return k_ra_err_not_supported;
}

/**
 * @brief Vtable deinit -- drops ``s_eink_ctx`` state.
 *
 * @details No SPI traffic to tear down in the stub; the only effect
 *          is clearing the ``initialised`` flag so the dispatcher
 *          can re-bind.
 *
 * @param[in] ctx Backend context.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Always (the stub cannot fail).
 * @retval k_ra_err_null_ptr ctx was NULL.
 *
 * @pre ``eink_init`` has succeeded.
 * @pre No concurrent caller.
 * @post ``s_eink_ctx.initialised == false``.
 * @post ``s_eink_ctx.fb.pixels == nullptr``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t eink_deinit(void* ctx)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  eink_ctx_t* c  = (eink_ctx_t*)ctx;
  c->initialised = false;
  c->fb.pixels   = nullptr;
  return k_ra_ok;
}

/* =============================================================================
 * Public iface instance
 * =============================================================================
 */

const display_backend_iface_t k_display_backend_eink_it8951 = {
  .init            = eink_init,
  .get_caps        = eink_get_caps,
  .get_framebuffer = eink_get_framebuffer,
  .flush           = eink_flush,
  .clear           = eink_clear,
  .deinit          = eink_deinit,
  /* No GUIX shim for the e-ink backend yet -- apps that want GUIX
   * on e-ink will need a dedicated ``gx_display_driver_ra_it8951``
   * once the hardware driver lands. */
  .bind_guix = nullptr,
};
