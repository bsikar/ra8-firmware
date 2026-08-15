/**
 * @file ra8_display_pal_lcd.c
 * @brief LCD backend (ra8_glcdc) implementation for the display PAL
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Wraps the existing ``libs/ra8_hal/src/ra8_glcdc.c`` HAL plus the
 * EK-RA8D2 panel power-on sequence into the
 * ``display_backend_iface_t`` shape the PAL dispatcher consumes.
 * The 6-step bring-up that used to be duplicated in every LCD-using
 * app's ``lcd_bringup_panel`` collapses into a single
 * ``display_init`` call.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_display_pal_lcd.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cache.h"
#include "ra8_check.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_internal.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_log.h"
#include "ra8_time.h"

/* =============================================================================
 * Module-static storage
 * =============================================================================
 */

/** @brief Module log tag. */
static const char* const s_tag = "ra8_display_pal_lcd";

/** @brief RGB565 pixel mask. */
typedef enum : uint32_t {
  k_rgb565_mask = 0xFFFFU, /**< Rgb565 mask. */
} disp_lcd_mask_t;

/**
 * @enum ra8_display_pal_lcd_const_t
 * @brief Numeric constants used by the LCD backend bring-up.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lcd_bringup_settle_ms = 200U, /**< Pin/clock settle after board_glcdc_init. */
  k_lcd_bg_color_black    = 0U,   /**< 24-bit ARGB for BG plane.                */
  k_lcd_fb_align_bytes    = 64U,  /**< AXI burst alignment (HUM Ch 63).         */
  k_lcd_rgb565_bpp        = 2U,   /**< Bytes per RGB565 pixel.                  */
} ra8_display_pal_lcd_const_t;

/**
 * @struct lcd_ctx_t
 * @brief LCD backend private context attached to the PAL handle.
 *
 * @details
 * Carries the bound framebuffer descriptor and a copy of the caps
 * report so ``get_framebuffer`` / ``get_caps`` are O(1) reads.
 *
 * @since 0.1.0
 */
typedef struct {
  display_caps_t caps;    /**< Caps.    */
  display_fb_t   fb;      /**< Fb.      */
  bool           started; /**< Started. */
} lcd_ctx_t;

/** @brief Single LCD backend context -- one display per board. */
static lcd_ctx_t s_lcd_ctx;

/**
 * @brief DSB memory barrier on Cortex-M, no-op on the host test build.
 *
 * @details
 * Guarded with ``RA8_OFF_TARGET`` so the host x86 build does not
 * try to assemble the ARMv8-M ``dsb`` instruction. Matches the
 * pattern used by ``ra8_reset.c`` and ``ra8_lpm.c``.
 *
 * @pre None.
 * @pre None.
 * @post Prior register / memory writes are observable.
 * @post No state mutated.
 *
 * @note Inlined; zero-overhead on the host.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void internal_lcd_dsb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

/* =============================================================================
 * Internal helpers (kept short for NASA P10 Rule 4)
 * =============================================================================
 */

/**
 * @brief Validate the caller's ``display_cfg_t`` against the LCD
 *        backend's expectations.
 *
 * @details
 * Rejects NULL framebuffer, zero / negative dimensions, undersized
 * buffer, and any non-RGB565 pixel format. Run before any hardware
 * call so a failure leaves the panel untouched.
 *
 * @param[in] cfg Caller configuration.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Configuration accepted.
 * @retval k_ra8_err_null_ptr      Framebuffer pointer was NULL.
 * @retval k_ra8_err_invalid_arg   Dimensions or buffer size out of range.
 * @retval k_ra8_err_not_supported Pixel format other than RGB565.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Returned value depends solely on cfg.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_validate_cfg(const display_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->framebuffer, s_tag, "cfg->framebuffer");
  if (cfg->width_px == 0U || cfg->height_px == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->pixfmt != k_display_pixfmt_rgb565) {
    return k_ra8_err_not_supported;
  }
  /* The GLCDC backend needs the panel's RGB timing; the board BSP supplies it
   * (e.g. &s_ra8_panel_ek_ra8d2_timing). Host / e-ink backends leave it null. */
  if (cfg->panel_timing == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t need_bytes =
    (uint32_t)cfg->width_px * (uint32_t)cfg->height_px * (uint32_t)k_lcd_rgb565_bpp;
  if (cfg->framebuffer_bytes < need_bytes) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the GPIO + GLCDC sequence that powers the panel up
 *        and starts scan-out.
 *
 * @details
 * Mirrors the ``lcd_bringup_panel`` helper that used to live in
 * every LCD-using app: panel power-on, board GLCDC routing, settle,
 * GLCDC init, BG clear, ``start(true)``, ``layer1_show``. Returns on
 * the first error -- callers must treat a non-ok return as "panel
 * may be in any state" and not call deinit.
 *
 * @param[in] cfg Caller configuration (validated upstream).
 *
 * @return ra8_err_t Error code from the first failing HAL call, or
 *         ``k_ra8_ok`` once GR1 is showing.
 * @retval k_ra8_ok          Panel is up and GR1 is visible.
 * @retval (from ra8_board_*) Forwarded from the board helpers.
 * @retval (from ra8_glcdc_*) Forwarded from the GLCDC HAL.
 *
 * @pre ``internal_lcd_validate_cfg`` returned ``k_ra8_ok`` for cfg.
 * @pre MSTP and CGC are already initialised by the boot path.
 * @post On success: GLCDC scanning out, GR1 visible over BG black.
 * @post On failure: state is implementation-defined; caller bails.
 *
 * @note Not thread-safe; single-shot init helper.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_bringup_panel(const display_cfg_t* cfg)
{
  ra8_err_t err = ra8_board_lcd_panel_power_on();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb888);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_lcd_bringup_settle_ms);

  const ra8_glcdc_config_t glcdc_cfg = {
    .framebuffer_addr = (uint32_t)(uintptr_t)cfg->framebuffer,
    .width_px         = cfg->width_px,
    .height_px        = cfg->height_px,
    .format           = k_ra8_glcdc_fmt_rgb565,
    .timing           = *(const ra8_glcdc_timing_t*)cfg->panel_timing,
  };
  err = ra8_glcdc_init(&glcdc_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_glcdc_set_background_color((uint32_t)k_lcd_bg_color_black);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_glcdc_start(true);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_glcdc_layer1_show((uintptr_t)cfg->framebuffer);
}

/**
 * @brief Populate ``s_lcd_ctx`` from the validated cfg.
 *
 * @details
 * Snapshotting cfg into the context lets ``get_caps`` /
 * ``get_framebuffer`` answer without touching the cfg pointer
 * again -- the caller may free its cfg after init returns.
 *
 * @param[in] cfg Validated caller configuration.
 *
 * @pre ``internal_lcd_validate_cfg`` returned ``k_ra8_ok``.
 * @pre cfg is alive for the duration of the call (read-only).
 * @post ``s_lcd_ctx`` mirrors cfg's geometry + framebuffer pointer.
 * @post ``s_lcd_ctx.started == true``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_lcd_snapshot(const display_cfg_t* cfg)
{
  const uint32_t stride                  = (uint32_t)cfg->width_px * (uint32_t)k_lcd_rgb565_bpp;
  s_lcd_ctx.caps.width_px                = cfg->width_px;
  s_lcd_ctx.caps.height_px               = cfg->height_px;
  s_lcd_ctx.caps.pixfmt                  = k_display_pixfmt_rgb565;
  s_lcd_ctx.caps.stride_bytes            = stride;
  s_lcd_ctx.caps.refresh_latency_us_typ  = 0U;
  s_lcd_ctx.caps.supports_partial_update = true;
  s_lcd_ctx.caps.continuous_refresh      = true;

  s_lcd_ctx.fb.pixels       = cfg->framebuffer;
  s_lcd_ctx.fb.width_px     = cfg->width_px;
  s_lcd_ctx.fb.height_px    = cfg->height_px;
  s_lcd_ctx.fb.stride_bytes = stride;
  s_lcd_ctx.fb.pixfmt       = k_display_pixfmt_rgb565;
  s_lcd_ctx.started         = true;
}

/**
 * @brief Confirm a rectangle stays inside the active framebuffer.
 *
 * @details
 * Compound boolean intentionally split into four sequential checks
 * so MC/DC vectors are trivial.
 *
 * @param[in] caps Backend capabilities.
 * @param[in] r    Candidate rectangle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Rectangle fits.
 * @retval k_ra8_err_invalid_arg  Rectangle would step out of bounds.
 *
 * @pre caps reflects the bound framebuffer.
 * @pre r has been zero-initialised by the caller.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_check_rect(const display_caps_t* caps, display_rect_t r)
{
  if (r.x > caps->width_px) {
    return k_ra8_err_invalid_arg;
  }
  if (r.y > caps->height_px) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)r.x + (uint32_t)r.w > (uint32_t)caps->width_px) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)r.y + (uint32_t)r.h > (uint32_t)caps->height_px) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Vtable callbacks
 * =============================================================================
 */

/**
 * @brief LCD backend init -- validate, panel bring-up, snapshot.
 *
 * @details Vtable entry for ``k_display_backend_lcd_ra8_glcdc``. The
 *          body is short because the heavy lifting is in the two
 *          internal helpers above; this matches the NASA P10 Rule 4
 *          per-function size budget.
 *
 * @param[in]  cfg     Caller configuration.
 * @param[out] out_ctx Filled with the backend context pointer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Backend ready.
 * @retval k_ra8_err_null_ptr      Required pointer was NULL.
 * @retval k_ra8_err_invalid_arg   Dimensions / buffer size invalid.
 * @retval k_ra8_err_not_supported Pixel format other than RGB565.
 * @retval k_ra8_err_busy          Backend already started.
 *
 * @pre PAL dispatcher has not bound a backend yet.
 * @pre Clocks and MSTP have been initialised by the caller's boot path.
 * @post Panel is powered and scanning out the caller's framebuffer.
 * @post ``s_lcd_ctx.started == true`` on success.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_init(const display_cfg_t* cfg, void** out_ctx)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(out_ctx, s_tag, "out_ctx");

  const ra8_err_t v = internal_lcd_validate_cfg(cfg);
  if (v != k_ra8_ok) {
    return v;
  }
  if (s_lcd_ctx.started) {
    ra8_log_error(s_tag, "internal_lcd_init: already started");
    return k_ra8_err_busy;
  }
  const ra8_err_t err = internal_lcd_bringup_panel(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_lcd_snapshot(cfg);
  *out_ctx = &s_lcd_ctx;
  return k_ra8_ok;
}

/**
 * @brief Vtable get_caps -- O(1) copy from ``s_lcd_ctx.caps``.
 *
 * @details The caps were snapshotted from cfg at init time.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Caps written.
 * @retval k_ra8_err_null_ptr Either argument was NULL.
 *
 * @pre ``internal_lcd_init`` has succeeded.
 * @pre out is writable.
 * @post ``*out`` equals the stored caps.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_get_caps(const void* ctx, display_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  const lcd_ctx_t* c = (const lcd_ctx_t*)ctx;
  *out               = c->caps;
  return k_ra8_ok;
}

/**
 * @brief Vtable get_framebuffer -- hand back the FB descriptor.
 *
 * @details Returns the same buffer the caller passed in via cfg.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Framebuffer descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Descriptor written.
 * @retval k_ra8_err_null_ptr Either argument was NULL.
 *
 * @pre ``internal_lcd_init`` has succeeded.
 * @pre out is writable.
 * @post ``*out.pixels`` matches ``cfg.framebuffer``.
 * @post No state mutated.
 *
 * @note Thread-safe relative to other PAL calls.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_get_framebuffer(void* ctx, display_fb_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  const lcd_ctx_t* c = (const lcd_ctx_t*)ctx;
  *out               = c->fb;
  return k_ra8_ok;
}

/**
 * @brief Vtable flush -- DSB barrier, validate rect.
 *
 * @details
 * LCD scans continuously so flush is essentially a no-op (the
 * caller's writes will land on the next scan).  We still validate
 * ``rect`` so callers writing backend-agnostic code learn about
 * out-of-bounds rectangles at the LCD path rather than first
 * discovering them on e-ink.  ``hint`` is ignored.
 *
 * @param[in] ctx  Backend context.
 * @param[in] rect Region the caller painted.
 * @param[in] hint Refresh-mode intent (ignored for LCD).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Barrier issued.
 * @retval k_ra8_err_null_ptr    ctx was NULL.
 * @retval k_ra8_err_invalid_arg rect leaves the framebuffer.
 *
 * @pre ``internal_lcd_init`` has succeeded.
 * @pre None.
 * @post Panel state is unchanged until the next scan picks up the writes.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_flush(void* ctx, display_rect_t rect, display_refresh_hint_t hint)
{
  (void)hint; /* LCD scans continuously; hint is purely informational. */
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const lcd_ctx_t* c = (const lcd_ctx_t*)ctx;
  const ra8_err_t  v = internal_lcd_check_rect(&c->caps, rect);
  if (v != k_ra8_ok) {
    return v;
  }
#ifdef RA8_BOOT_ENABLE_CACHE_MPU
  /* D-cache is enabled for this build: the CPU paints the cacheable SDRAM
   * framebuffer through the write-back L1 D-cache, but the GLCDC scans SDRAM
   * directly over AXI and never sees the cache. Clean (write back) the painted
   * rows so the next scan latches the new pixels instead of stale SDRAM. The
   * clean covers the flushed rect's contiguous row-span [y, y+h) -- a superset
   * of the rect, exact for the full-width flushes the reader issues, and safe
   * for a partial-width rect (whole rows, never a strided w*h slice).
   * ra8_cache_dcache_clean_by_addr rounds to whole cache lines internally. */
  {
    const uint8_t* row_base = (const uint8_t*)c->fb.pixels + (uint32_t)rect.y * c->fb.stride_bytes;
    const uint32_t row_span = (uint32_t)rect.h * c->fb.stride_bytes;
    (void)ra8_cache_dcache_clean_by_addr(row_base, row_span);
  }
#endif
  /* Memory barrier so any pixel writes the caller did with NEON / DMA
   * are visible to the GLCDC's AXI controller before the next scan. */
  internal_lcd_dsb();
  return k_ra8_ok;
}

/**
 * @brief Vtable clear -- fill the framebuffer with a uniform RGB565 value.
 *
 * @details Tight CPU loop; not the fastest possible (DMA would beat
 *          this) but the simplest correct implementation.
 *
 * @param[in] ctx   Backend context.
 * @param[in] color Pixel value (low 16 bits used).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Buffer cleared.
 * @retval k_ra8_err_null_ptr ctx was NULL.
 *
 * @pre ``internal_lcd_init`` has succeeded.
 * @pre Framebuffer is reachable from the CPU.
 * @post Every pixel of the FB equals ``color & 0xFFFF``.
 * @post DSB barrier issued so the GLCDC sees the writes.
 *
 * @note Not thread-safe with concurrent paint loops.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_clear(void* ctx, uint32_t color)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  lcd_ctx_t*     c            = (lcd_ctx_t*)ctx;
  uint16_t*      pixels       = (uint16_t*)c->fb.pixels;
  const uint32_t pixels_total = (uint32_t)c->fb.width_px * (uint32_t)c->fb.height_px;
  const uint16_t rgb565       = (uint16_t)(color & k_rgb565_mask);
  for (uint32_t i = 0U; i < pixels_total; ++i) {
    pixels[i] = rgb565;
  }
#ifdef RA8_BOOT_ENABLE_CACHE_MPU
  /* Write the cleared pixels back from the write-back D-cache to SDRAM so the
   * GLCDC scan sees them (see internal_lcd_flush for the coherency rationale). The whole
   * framebuffer was just touched, so clean the whole contiguous span. */
  (void)ra8_cache_dcache_clean_by_addr(c->fb.pixels,
                                       (uint32_t)c->fb.stride_bytes * (uint32_t)c->fb.height_px);
#endif
  internal_lcd_dsb();
  return k_ra8_ok;
}

/**
 * @brief Vtable deinit -- tear the GLCDC down and drop state.
 *
 * @details Forwards to ``ra8_glcdc_deinit`` which disables scan-out
 *          and clears the registers.  GPIO pins claimed during
 *          ``ra8_board_lcd_panel_power_on`` are left claimed (a
 *          follow-up init either does a chip reset first or stays
 *          bound).
 *
 * @param[in] ctx Backend context.
 *
 * @return ra8_err_t Error code from ``ra8_glcdc_deinit`` (or
 *         ``k_ra8_err_null_ptr`` if ctx was NULL).
 * @retval k_ra8_ok           Tear-down succeeded.
 * @retval k_ra8_err_null_ptr ctx was NULL.
 *
 * @pre ``internal_lcd_init`` has succeeded.
 * @pre No concurrent paint loops.
 * @post ``s_lcd_ctx.started == false``.
 * @post ``s_lcd_ctx.fb.pixels == nullptr``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lcd_deinit(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  lcd_ctx_t*      c   = (lcd_ctx_t*)ctx;
  const ra8_err_t err = ra8_glcdc_deinit();
  c->started          = false;
  c->fb.pixels        = nullptr;
  return err;
}

/* =============================================================================
 * Public iface instance
 * =============================================================================
 */

const display_backend_iface_t k_display_backend_lcd_ra8_glcdc = {
  .init            = internal_lcd_init,
  .get_caps        = internal_lcd_get_caps,
  .get_framebuffer = internal_lcd_get_framebuffer,
  .flush           = internal_lcd_flush,
  .clear           = internal_lcd_clear,
  .deinit          = internal_lcd_deinit,
};
