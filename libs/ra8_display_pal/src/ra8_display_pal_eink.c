/**
 * @file ra8_display_pal_eink.c
 * @brief E-ink (IT8951) backend for the display PAL, wired to ra8_epaper.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Drives an IT8951-compatible e-paper panel through the HAL driver
 * ``libs/ra8_hal/src/ra8_epaper.c``. The app keeps painting the same
 * canonical RGB565 framebuffer it uses on the LCD backend; this backend
 * converts each flushed rectangle to the IT8951's native 8 bpp greyscale
 * (Rec.601 luma) on the way to the controller, then triggers a panel
 * update with the waveform mapped from the caller's
 * ``display_refresh_hint_t``.
 *
 * The IT8951 hardware descriptor (injected SPI bus seam, reset+busy
 * GPIO, native panel size) is **not** hard-coded here -- it is supplied
 * by the board BSP through ``display_cfg_t.panel_timing`` as a
 * ``const ra8_epaper_cfg_t*`` (the same opaque-config seam the GLCDC
 * backend uses for its ``ra8_glcdc_timing_t``). This keeps the PAL
 * accessory-agnostic: the BSP owns the SPI peripheral and binds the
 * seam (typically via ``ra8_io_spi_bus_as_ops()``).
 *
 * Conversion is streamed one framebuffer row at a time through a bounded
 * static line buffer (``s_eink_line``), so no full-panel 8 bpp shadow
 * buffer is needed regardless of panel size (NASA P10 Rule 3: no
 * unbounded allocation). On-panel HIL validation needs an IT8951 panel on
 * the bench; the vtable + conversion are host-verified against the
 * simulator-backed ``ra8_epaper`` (see tests/test_ra8_display_pal.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_display_pal_eink.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_internal.h"
#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_display_pal_eink";

/**
 * @struct eink_ctx_t
 * @brief E-ink backend context attached to the PAL handle.
 *
 * @since 0.1.0
 */
typedef struct {
  display_caps_t caps;        /**< Caps.        */
  display_fb_t   fb;          /**< Fb.          */
  bool           initialised; /**< Initialised. */
} eink_ctx_t;

/** @brief Single e-ink backend context. */
static eink_ctx_t s_eink_ctx;

/**
 * @enum ra8_display_pal_eink_const_t
 * @brief Internal numeric constants (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_eink_rgb565_bpp         = 2U,      /**< Bytes per RGB565 pixel.        */
  k_eink_refresh_quality_us = 450000U, /**< Typical GC16 latency.          */
  k_eink_line_max_px        = 4096U,   /**< Max row width (= panel max).   */
  k_eink_luma_r             = 77U,     /**< Rec.601 R weight (0.299*256).  */
  k_eink_luma_g             = 150U,    /**< Rec.601 G weight (0.587*256).  */
  k_eink_luma_b             = 29U,     /**< Rec.601 B weight (0.114*256).  */
  k_eink_luma_shift         = 8U,      /**< Luma weight denominator shift. */
  k_eink_nibble_shift       = 4U,      /**< 8bpp luma -> 4bpp nibble shift. */
  k_eink_px_per_byte_4bpp   = 2U,      /**< Pixels packed per 4bpp byte.   */
  k_eink_nibble_mask        = 0x0FU,   /**< Low-nibble mask.               */
  k_eink_white_nibble       = 0x0FU,   /**< 4bpp white, used to pad tails. */
} ra8_display_pal_eink_const_t;

/**
 * @enum ra8_display_pal_eink_rgb_t
 * @brief RGB565 field shifts / masks for luma extraction.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_eink_r5_shift = 11U,   /**< Red field shift in RGB565.   */
  k_eink_g6_shift = 5U,    /**< Green field shift in RGB565. */
  k_eink_r5_mask  = 0x1FU, /**< 5-bit red mask.              */
  k_eink_g6_mask  = 0x3FU, /**< 6-bit green mask.            */
  k_eink_b5_mask  = 0x1FU, /**< 5-bit blue mask.             */
} ra8_display_pal_eink_rgb_t;

/**
 * @var s_eink_line
 * @brief Bounded RGB565 -> 4 bpp conversion scratch (one panel row).
 *
 * @details
 * Sized for a full-width row at 8 bpp even though the flush path packs at
 * 4 bpp, so the buffer stays correct if a future caller needs the wider
 * depth. At 4 bpp only the first ``ceil(width / 2)`` bytes are used.
 *
 * @note Written by the flush path only; not reentrant.
 * @warning Do not read this outside a flush -- it holds the last converted
 *          row, not a framebuffer.
 * @since 0.1.0
 */
static uint8_t s_eink_line[k_eink_line_max_px];

/* =============================================================================
 * Conversion + mapping helpers
 * =============================================================================
 */

uint8_t ra8_display_pal_eink_luma_from_rgb565(uint16_t px)
{
  const uint32_t r5 = (uint32_t)((px >> (uint16_t)k_eink_r5_shift) & (uint16_t)k_eink_r5_mask);
  const uint32_t g6 = (uint32_t)((px >> (uint16_t)k_eink_g6_shift) & (uint16_t)k_eink_g6_mask);
  const uint32_t b5 = (uint32_t)(px & (uint16_t)k_eink_b5_mask);
  /* Expand 5/6-bit channels to 8-bit by bit-replication (0->0, max->255). */
  const uint32_t r8   = (r5 << 3U) | (r5 >> 2U);
  const uint32_t g8   = (g6 << 2U) | (g6 >> 4U);
  const uint32_t b8   = (b5 << 3U) | (b5 >> 2U);
  const uint32_t luma = ((r8 * (uint32_t)k_eink_luma_r) + (g8 * (uint32_t)k_eink_luma_g) +
                         (b8 * (uint32_t)k_eink_luma_b)) >>
                        (uint32_t)k_eink_luma_shift;
  return (uint8_t)luma;
}

/**
 * @brief Map a PAL refresh hint onto an IT8951 waveform mode.
 *
 * @details ``fast`` -> A2 (1bpp, fastest), ``quality`` -> GC16 (16-grey,
 *          cleanest), ``init`` -> the full INIT flush-to-white.
 *
 * @param[in] hint Caller refresh intent.
 *
 * @return The IT8951 waveform to pass to ``ra8_epaper_display_area``.
 * @retval k_ra8_epaper_wf_a2   For ``k_display_refresh_fast``.
 * @retval k_ra8_epaper_wf_gc16 For ``k_display_refresh_quality`` / default.
 *
 * @pre  None.
 * @pre  None.
 * @post No state mutated.
 * @post Return depends solely on ``hint``.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_epaper_waveform_t internal_eink_waveform(display_refresh_hint_t hint)
{
  if (hint == k_display_refresh_fast) {
    return k_ra8_epaper_wf_a2;
  }
  if (hint == k_display_refresh_init) {
    return k_ra8_epaper_wf_init;
  }
  return k_ra8_epaper_wf_gc16;
}

/**
 * @brief Validate the caller cfg against the e-ink backend's rules.
 *
 * @details
 * Same canonical-format rules as the LCD backend, plus: the BSP must
 * supply the IT8951 descriptor through ``panel_timing``, and the
 * framebuffer width must fit the bounded conversion line buffer.
 *
 * @param[in] cfg Caller configuration (non-NULL; checked by caller).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Cfg accepted.
 * @retval k_ra8_err_null_ptr      ``framebuffer`` was NULL.
 * @retval k_ra8_err_invalid_arg   Dimensions / buffer / descriptor invalid.
 * @retval k_ra8_err_not_supported Pixel format not RGB565.
 *
 * @pre  cfg != NULL.
 * @pre  cfg fields are readable.
 * @post No state mutated.
 * @post Return depends solely on cfg.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_eink_validate_cfg(const display_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->framebuffer, s_tag, "cfg->framebuffer");
  if (cfg->width_px == 0U || cfg->height_px == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->pixfmt != k_display_pixfmt_rgb565) {
    return k_ra8_err_not_supported;
  }
  if (cfg->width_px > (uint16_t)k_eink_line_max_px) {
    return k_ra8_err_invalid_arg;
  }
  /* The IT8951 descriptor is BSP-supplied through panel_timing. */
  if (cfg->panel_timing == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t need_bytes =
    (uint32_t)cfg->width_px * (uint32_t)cfg->height_px * (uint32_t)k_eink_rgb565_bpp;
  if (cfg->framebuffer_bytes < need_bytes) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Snapshot the validated cfg into ``s_eink_ctx``.
 *
 * @details
 * Records geometry, the RGB565 framebuffer pointer, and the known GC16
 * latency so ``get_caps`` / ``get_framebuffer`` are O(1) reads.
 *
 * @param[in] cfg Validated caller configuration.
 *
 * @pre  ``internal_eink_validate_cfg`` returned ``k_ra8_ok``.
 * @pre  cfg is alive for the call.
 * @post ``s_eink_ctx`` mirrors cfg.
 * @post ``s_eink_ctx.initialised == true``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * @brief Confirm a rectangle stays inside the active framebuffer.
 *
 * @details Split into four simple decisions so MC/DC stays trivial.
 *
 * @param[in] caps Backend capabilities.
 * @param[in] r    Candidate rectangle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Rectangle fits.
 * @retval k_ra8_err_invalid_arg Rectangle steps out of bounds.
 *
 * @pre  caps reflects the bound framebuffer.
 * @pre  r is initialised by the caller.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_eink_check_rect(const display_caps_t* caps, display_rect_t r)
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

/**
 * @brief Pack one RGB565 span into ``s_eink_line`` as 4 bpp greyscale.
 *
 * @details
 * Converts each pixel to 8 bpp Rec.601 luma, keeps its high nibble, and
 * packs two pixels per byte -- first pixel in the high nibble. An odd
 * trailing pixel is paired with white so the row still ends on a byte
 * boundary, which is what the controller's row stride assumes.
 *
 * 4 bpp is the panel's real depth: it renders 16 grey levels and the
 * IT8951 discards the low nibble of 8 bpp data anyway, so this halves the
 * bytes on the wire at no optical cost.
 *
 * @param[in] src   First RGB565 pixel of the row; non-NULL, ``width`` long.
 * @param[in] width Pixels to convert; ``1 .. k_eink_line_max_px``.
 *
 * @pre  ``src`` holds at least ``width`` readable pixels.
 * @pre  ``width <= k_eink_line_max_px``.
 * @post ``s_eink_line[0 .. ceil(width/2))`` holds the packed row.
 * @post No panel traffic is generated.
 *
 * @note Not thread-safe -- writes the shared ``s_eink_line`` scratch.
 *
 * @see ra8_display_pal_eink_luma_from_rgb565
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_eink_pack_row_4bpp(const uint16_t* src, uint16_t width)
{
  const uint32_t px_per_byte = (uint32_t)k_eink_px_per_byte_4bpp;
  for (uint32_t col = 0U; col < (uint32_t)width; col += px_per_byte) {
    /* The controller keeps only the high nibble of an 8 bpp byte, so
     * dropping the low nibble here costs nothing optically and halves the
     * bytes on the wire. */
    const uint8_t  hi_px = ra8_display_pal_eink_luma_from_rgb565(src[col]);
    const uint32_t next  = col + 1U;
    const uint8_t  lo_px = (next < (uint32_t)width)
                             ? ra8_display_pal_eink_luma_from_rgb565(src[next])
                             : (uint8_t)(k_eink_white_nibble << k_eink_nibble_shift);
    const uint8_t  hi_n  = (uint8_t)((hi_px >> k_eink_nibble_shift) & k_eink_nibble_mask);
    const uint8_t  lo_n  = (uint8_t)((lo_px >> k_eink_nibble_shift) & k_eink_nibble_mask);
    s_eink_line[col / px_per_byte] =
      (uint8_t)((uint8_t)(hi_n << k_eink_nibble_shift) | lo_n);
  }
}

/**
 * @brief Convert + stream each row of ``rect`` into the IT8951 frame RAM.
 *
 * @details
 * For every framebuffer row in ``rect``: pack the RGB565 span to 4 bpp
 * greyscale in the bounded ``s_eink_line`` buffer, then
 * ``ra8_epaper_load_image`` that 1-row sub-area (the controller places it
 * via the area's x/y). The byte count comes from
 * ``ra8_epaper_image_bytes`` rather than being recomputed here, so the
 * PAL's packing and the driver's expectation cannot drift apart. The panel
 * is refreshed once by the caller after all rows are loaded.
 *
 * @param[in] c    Backend context (validated).
 * @param[in] rect Region to push (already bounds-checked).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            All rows loaded into frame RAM.
 * @retval (from ra8_epaper)   Forwarded from ``ra8_epaper_image_bytes`` or
 *                             ``ra8_epaper_load_image``.
 *
 * @pre  ``ra8_epaper_init`` succeeded; ``rect`` lies in the framebuffer.
 * @pre  ``rect.w <= k_eink_line_max_px``.
 * @post Frame RAM holds the converted pixels; panel not yet refreshed.
 * @post ``s_eink_line`` holds the last converted row.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_eink_load_rect(const eink_ctx_t* c, display_rect_t rect)
{
  const uint16_t* fb    = (const uint16_t*)c->fb.pixels;
  const uint32_t  width = (uint32_t)c->fb.width_px;
  for (uint16_t row = 0U; row < rect.h; ++row) {
    const uint16_t* src = &fb[(((uint32_t)rect.y + row) * width) + rect.x];
    internal_eink_pack_row_4bpp(src, rect.w);
    const ra8_epaper_area_t area = {.x      = rect.x,
                                    .y      = (uint16_t)(rect.y + row),
                                    .width  = rect.w,
                                    .height = 1U};
    size_t                  need = 0U;
    const ra8_err_t         serr =
      ra8_epaper_image_bytes(&area, k_ra8_epaper_pf_4bpp, &need);
    if (serr != k_ra8_ok) {
      return serr;
    }
    const ra8_err_t err = ra8_epaper_load_image(&area,
                                                s_eink_line,
                                                need,
                                                k_ra8_epaper_pf_4bpp,
                                                k_ra8_epaper_endian_little);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Vtable callbacks
 * =============================================================================
 */

/**
 * @brief E-ink backend init -- validate, bring up the IT8951, snapshot.
 *
 * @details
 * Vtable entry for ``k_display_backend_eink_it8951``. Validates the cfg,
 * then brings up the panel via ``ra8_epaper_init`` using the BSP-supplied
 * ``ra8_epaper_cfg_t`` carried in ``cfg->panel_timing``, and snapshots
 * geometry for the O(1) accessors.
 *
 * @param[in]  cfg     Caller configuration.
 * @param[out] out_ctx Filled with the backend context pointer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Backend ready.
 * @retval k_ra8_err_null_ptr      cfg / out_ctx / framebuffer NULL.
 * @retval k_ra8_err_invalid_arg   Dimensions / descriptor invalid.
 * @retval k_ra8_err_not_supported Pixel format not RGB565.
 * @retval k_ra8_err_busy          Backend already initialised.
 * @retval (from ra8_epaper_init)  Panel bring-up failure.
 *
 * @pre PAL dispatcher has not bound a backend yet.
 * @pre Clocks + MSTP initialised; the SPI bus behind the descriptor's
 *      injected seam is up and its pins routed by the boot path.
 * @post On success the IT8951 is ready and ``s_eink_ctx.initialised``.
 * @post On failure ``s_eink_ctx`` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_init(const display_cfg_t* cfg, void** out_ctx)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(out_ctx, s_tag, "out_ctx");
  const ra8_err_t v = internal_eink_validate_cfg(cfg);
  if (v != k_ra8_ok) {
    return v;
  }
  if (s_eink_ctx.initialised) {
    ra8_log_error(s_tag, "eink_init: already initialised");
    return k_ra8_err_busy;
  }
  const ra8_err_t err = ra8_epaper_init((const ra8_epaper_cfg_t*)cfg->panel_timing);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "eink_init: ra8_epaper_init failed");
    return err;
  }
  internal_eink_snapshot(cfg);
  *out_ctx = &s_eink_ctx;
  ra8_log_info(s_tag, "eink_init: IT8951 backend bound");
  return k_ra8_ok;
}

/**
 * @brief Vtable get_caps -- O(1) copy from ``s_eink_ctx.caps``.
 *
 * @details Caps were snapshotted from cfg at init time.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Caps written.
 * @retval k_ra8_err_null_ptr Either argument was NULL.
 *
 * @pre  ``eink_init`` has succeeded.
 * @pre  out is writable.
 * @post ``*out`` equals the stored caps.
 * @post No state mutated.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_get_caps(const void* ctx, display_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  *out = ((const eink_ctx_t*)ctx)->caps;
  return k_ra8_ok;
}

/**
 * @brief Vtable get_framebuffer -- hand back the RGB565 FB descriptor.
 *
 * @details
 * The app paints this canonical RGB565 buffer; ``flush`` converts it to
 * the panel's native depth. Same pointer the caller passed via cfg.
 *
 * @param[in]  ctx Backend context.
 * @param[out] out Framebuffer descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Descriptor written.
 * @retval k_ra8_err_null_ptr Either argument was NULL.
 *
 * @pre  ``eink_init`` has succeeded.
 * @pre  out is writable.
 * @post ``*out`` mirrors the bound framebuffer.
 * @post No state mutated.
 *
 * @note Thread-safe relative to other PAL calls.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_get_framebuffer(void* ctx, display_fb_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  *out = ((const eink_ctx_t*)ctx)->fb;
  return k_ra8_ok;
}

/**
 * @brief Vtable flush -- convert ``rect`` to 8bpp and refresh the panel.
 *
 * @details
 * Bounds-checks ``rect``, streams the converted rows into the IT8951
 * frame RAM (``internal_eink_load_rect``), then issues one
 * ``ra8_epaper_display_area`` with the waveform mapped from ``hint``.
 *
 * @param[in] ctx  Backend context.
 * @param[in] rect Region of the framebuffer to push.
 * @param[in] hint Refresh-mode intent (selects the waveform).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Region pushed and refreshed.
 * @retval k_ra8_err_null_ptr    ctx was NULL.
 * @retval k_ra8_err_invalid_arg rect leaves the framebuffer.
 * @retval (from ra8_epaper)     Forwarded load / display failure.
 *
 * @pre  ``eink_init`` has succeeded.
 * @pre  rect lies inside the framebuffer.
 * @post On success the panel reflects ``rect`` of the framebuffer.
 * @post On failure the panel is left as-is.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_flush(void* ctx, display_rect_t rect, display_refresh_hint_t hint)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  const eink_ctx_t* c = (const eink_ctx_t*)ctx;
  const ra8_err_t   v = internal_eink_check_rect(&c->caps, rect);
  if (v != k_ra8_ok) {
    return v;
  }
  const ra8_err_t lerr = internal_eink_load_rect(c, rect);
  if (lerr != k_ra8_ok) {
    return lerr;
  }
  const ra8_epaper_area_t area = {.x = rect.x, .y = rect.y, .width = rect.w, .height = rect.h};
  return ra8_epaper_display_area(&area, internal_eink_waveform(hint));
}

/**
 * @brief Vtable clear -- fill the RGB565 framebuffer with ``color``.
 *
 * @details
 * Per the PAL contract ``display_clear`` only writes the framebuffer; it
 * does not push to the panel (the caller follows with ``display_flush``).
 * So this mirrors the LCD backend: a tight RGB565 fill loop.
 *
 * @param[in] ctx   Backend context.
 * @param[in] color Pixel value (low 16 bits used as RGB565).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Buffer cleared.
 * @retval k_ra8_err_null_ptr ctx was NULL.
 *
 * @pre  ``eink_init`` has succeeded.
 * @pre  The framebuffer is reachable from the CPU.
 * @post Every framebuffer pixel equals ``color & 0xFFFF``.
 * @post Panel state is unchanged until the next flush.
 *
 * @note Not thread-safe with concurrent paint loops.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_clear(void* ctx, uint32_t color)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  eink_ctx_t*    c            = (eink_ctx_t*)ctx;
  uint16_t*      pixels       = (uint16_t*)c->fb.pixels;
  const uint32_t pixels_total = (uint32_t)c->fb.width_px * (uint32_t)c->fb.height_px;
  const uint16_t rgb565       = (uint16_t)(color & (uint32_t)UINT16_MAX);
  for (uint32_t i = 0U; i < pixels_total; ++i) {
    pixels[i] = rgb565;
  }
  return k_ra8_ok;
}

/**
 * @brief Vtable deinit -- sleep the panel and drop state.
 *
 * @details
 * Issues ``ra8_epaper_sleep`` (which returns the driver to its
 * uninitialised state so a fresh ``eink_init`` works), then clears the
 * context.
 *
 * @param[in] ctx Backend context.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Panel asleep; state dropped.
 * @retval k_ra8_err_null_ptr ctx was NULL.
 * @retval (from ra8_epaper)  Forwarded sleep failure.
 *
 * @pre  ``eink_init`` has succeeded.
 * @pre  No concurrent caller.
 * @post ``s_eink_ctx.initialised == false``.
 * @post ``s_eink_ctx.fb.pixels == nullptr``.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t eink_deinit(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  eink_ctx_t*     c   = (eink_ctx_t*)ctx;
  const ra8_err_t err = ra8_epaper_sleep();
  c->initialised      = false;
  c->fb.pixels        = nullptr;
  return err;
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
};
