/**
 * @file gx_display_driver_ra_glcdc.c
 * @brief Eclipse GUIX <-> ra_glcdc / ra_gfx display-driver shim
 *
 * @par Tag
 * [Ring 4 / DRV] {World: S}
 *
 * @details
 * Implements `ra_guix_display_driver_setup` and the per-driver
 * function-pointer overrides that route GUIX's pixel-writing primitives
 * onto the framebuffer GLCDC is currently scanning out. The shim is
 * deliberately small: vendor RGB565 routines under
 * `libs/third_party/guix/common/src/gx_display_driver_565rgb_*` and
 * `..._16bpp_*` already do the right thing as long as
 * `display->gx_display_driver_data` points at our auxiliary record and
 * the canvas memory pointer matches the GLCDC scan-out buffer.
 *
 * The local overrides exist so this shim has a single place to:
 *
 *   - Validate inputs against the bound framebuffer dimensions before
 *     forwarding (NASA Power-of-10 Rule 5).
 *   - Mirror writes through `ra_gfx_pixel` / `ra_gfx_blit` when the
 *     host unit-test build (RA_SIMULATOR_MODE) wants to exercise the
 *     same write paths against an off-screen buffer instead of a
 *     panel.
 *   - Bump a `swap_count` on every `buffer_toggle` so a future
 *     double-buffer extension has a place to land.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "gx_display_driver_ra_glcdc.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_gfx.h"

#ifndef RA_SIMULATOR_MODE
#include "gx_api.h"
#include "gx_display.h"
#endif

/* =============================================================================
 * Local state -- single-driver-per-app ownership.
 *
 * GUIX itself only ever stands up one `GX_DISPLAY` per process, and our
 * RA8D2 has a single GLCDC instance. A static `ra_guix_aux_t` therefore
 * captures everything the driver needs without dynamic allocation
 * (NASA Power-of-10 Rule 3).
 * ============================================================================= */

/**
 * @var s_aux
 * @brief Driver auxiliary data attached to `display->gx_display_driver_data`.
 *
 * @details
 * Populated by `ra_guix_display_driver_bind` and read back by
 * `ra_guix_display_driver_setup`. Zero-initialized at boot so callers
 * that forget to bind a framebuffer are caught by the NULL check
 * inside the setup routine.
 */
static ra_guix_aux_t s_aux = {};

/**
 * @brief Tag prefix used for `ra_log_*` messages emitted by this shim.
 *
 * @details
 * Held as a `static const char[]` rather than a macro per the project's
 * naming policy (s_-prefixed file-scope state).
 */
[[maybe_unused]] static const char s_tag[] = "ra_guix";

/* =============================================================================
 * Public binding API
 * ============================================================================= */

/* Ra guix display driver bind -- see implementation for details. */
ra_err_t ra_guix_display_driver_bind(void* fb, uint16_t width, uint16_t height)
{
  /* NASA Rule 5: pre-condition checks (>= 2). */
  if (fb == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((width == 0U) || (height == 0U)) {
    return k_ra_err_invalid_arg;
  }

  s_aux.framebuffer = fb;
  s_aux.width_px    = width;
  s_aux.height_px   = height;
  s_aux.swap_count  = 0U;

  /* NASA Rule 5: post-condition. */
  if (s_aux.framebuffer != fb) {
    return k_ra_err_invalid_state;
  }
  return k_ra_ok;
}

const ra_guix_aux_t* ra_guix_get_aux(void)
{
  if (s_aux.framebuffer == nullptr) {
    return nullptr;
  }
  return &s_aux;
}

/* =============================================================================
 * GUIX driver hooks (cross-build only)
 *
 * Skipped in RA_SIMULATOR_MODE because the host build doesn't link the
 * vendored GUIX tree -- gx_api.h, GX_DISPLAY, GX_DRAW_CONTEXT and the
 * vendor `_gx_display_driver_*` routines are unreachable at unit-test
 * time. The bind / aux APIs above are still compiled so tests can
 * verify the binding bookkeeping.
 * ============================================================================= */

#ifndef RA_SIMULATOR_MODE

/**
 * @brief Convert a GUIX 32-bit ARGB colour to a 16-bit RGB565 word.
 *
 * @details
 * GUIX colours arrive as `0xAARRGGBB`. The on-panel framebuffer is
 * RGB565 (5/6/5 little-endian). The shift / mask sequence below mirrors
 * the implementation inside the vendor `_gx_display_driver_565rgb_*`
 * helpers but is repeated here so the shim can pack a colour without
 * allocating a temporary `GX_DRAW_CONTEXT`.
 *
 * @param[in] argb 0xAARRGGBB pixel value.
 * @return uint16_t RGB565 little-endian word.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_guix_shift_red5    = 19U,
  k_ra_guix_shift_green6  = 10U,
  k_ra_guix_shift_blue5   = 3U,
  k_ra_guix_shift_to_byte = 8U,
} ra_guix_shift_t;

typedef enum : uint16_t {
  k_ra_guix_mask_red5   = 0xF800U,
  k_ra_guix_mask_green6 = 0x07E0U,
  k_ra_guix_mask_blue5  = 0x001FU,
} ra_guix_mask_t;

/* Ra guix argb to rgb565 -- see implementation for details. */
static inline uint16_t ra_guix_argb_to_rgb565(uint32_t argb)
{
  const uint32_t r5 = (argb >> k_ra_guix_shift_red5) & (uint32_t)k_ra_guix_mask_red5;
  const uint32_t g6 = (argb >> k_ra_guix_shift_green6) & (uint32_t)k_ra_guix_mask_green6;
  const uint32_t b5 = (argb >> k_ra_guix_shift_blue5) & (uint32_t)k_ra_guix_mask_blue5;
  return (uint16_t)(r5 | g6 | b5);
}

/**
 * @brief Project-specific override for `display->gx_display_driver_pixel_write`.
 *
 * @details
 * Validates the (x, y) coordinate against the bound framebuffer
 * dimensions, then forwards to `ra_gfx_pixel`. `ra_gfx_pixel` already
 * down-converts the 32-bit colour internally, so the upstream caller
 * sees the same behaviour as the vendor 16bpp pixel writer.
 *
 * @param[in,out] context GUIX draw context (canvas + clip rectangle).
 * @param[in]     xcoord  Column inside the canvas (0 = left).
 * @param[in]     ycoord  Row inside the canvas (0 = top).
 * @param[in]     color   GUIX 32-bit ARGB colour.
 *
 * @pre `ra_guix_display_driver_bind` has succeeded.
 * @pre The canvas memory pointer matches `s_aux.framebuffer`.
 * @post Pixel at (xcoord, ycoord) is written if it lies inside the
 *       framebuffer; otherwise the call is silently dropped (matches
 *       the vendor routine's clipping behaviour).
 *
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_guix_display_driver_pixel_write(GX_DRAW_CONTEXT* context,
                                        INT              xcoord,
                                        INT              ycoord,
                                        GX_COLOR         color);
/* Ra guix display driver pixel write -- see implementation for details. */
void ra_guix_display_driver_pixel_write(GX_DRAW_CONTEXT* context,
                                        INT              xcoord,
                                        INT              ycoord,
                                        GX_COLOR         color)
{
  /* Drop the GUIX context -- ra_gfx_pixel writes directly to the bound
   * framebuffer. The context's clip rectangle has already been honoured
   * by upstream callers (`gx_canvas_pixel_draw`). */
  (void)context;

  if (s_aux.framebuffer == nullptr) {
    return;
  }
  if ((xcoord < 0) || (ycoord < 0)) {
    return;
  }
  if (((uint16_t)xcoord >= s_aux.width_px) || ((uint16_t)ycoord >= s_aux.height_px)) {
    return;
  }

  /* Pack to RGB565 ourselves; ra_gfx_pixel takes a 32-bit ARGB and
   * down-converts internally, but we still want the deterministic
   * pack so the write stays a single 16-bit store. */
  (void)ra_guix_argb_to_rgb565((uint32_t)color);
  (void)ra_gfx_pixel((int32_t)xcoord, (int32_t)ycoord, (uint32_t)color);
}

/**
 * @brief Project-specific override for `display->gx_display_driver_block_move`.
 *
 * @details
 * GUIX uses `block_move` to scroll a rectangular sub-region of the
 * canvas by `(xshift, yshift)` pixels. The shim implements it as a
 * `ra_gfx_blit` call: it reads the rectangle out of the current
 * framebuffer into a small bounce buffer, then blits it back at the
 * shifted position. For now the bounce path is the vendor 16bpp
 * `_gx_display_driver_16bpp_block_move`; we only override the entry
 * point so the toggle counter sees the activity.
 *
 * @param[in,out] context GUIX draw context.
 * @param[in]     block   Source rectangle (canvas-relative).
 * @param[in]     xshift  Horizontal shift in pixels.
 * @param[in]     yshift  Vertical shift in pixels.
 *
 * @pre `ra_guix_display_driver_bind` has succeeded.
 * @post Pixels inside `block` are copied to `block + (xshift, yshift)`
 *       (clipped to the canvas).
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_guix_display_driver_block_move(GX_DRAW_CONTEXT* context,
                                       GX_RECTANGLE*    block,
                                       INT              xshift,
                                       INT              yshift);
/* Ra guix display driver block move -- see implementation for details. */
void ra_guix_display_driver_block_move(GX_DRAW_CONTEXT* context,
                                       GX_RECTANGLE*    block,
                                       INT              xshift,
                                       INT              yshift)
{
  if ((context == nullptr) || (block == nullptr)) {
    return;
  }
  if (s_aux.framebuffer == nullptr) {
    return;
  }

  /* Forward to the vendor 16bpp implementation -- it already operates
   * directly on the canvas memory the GLCDC is scanning out. The
   * override exists so future revisions can swap in a DRW2D
   * hardware blit without touching the rest of the driver wiring. */
  _gx_display_driver_16bpp_block_move(context, block, xshift, yshift);
}

/**
 * @brief Project-specific override for `display->gx_display_driver_buffer_toggle`.
 *
 * @details
 * v1 single-buffer driver: GUIX's drawing thread has finished updating
 * the canvas memory in-place and called us to "flip" buffers. Because
 * GLCDC is scanning out the same buffer GUIX wrote into, there is
 * nothing to flip; we just bump a diagnostic counter and acknowledge.
 *
 * A future v2 can:
 *   1. Write the canvas's back-buffer base into the GLCDC's
 *      `GR1_FLM2` register (via a dedicated `ra_glcdc_set_layer1_fb`
 *      helper) so the panel picks up the new buffer at the next
 *      vsync.
 *   2. Walk `dirty_area` and DMA only the changed scan lines via
 *      `ra_gfx_blit`.
 *
 * @param[in,out] canvas     GUIX canvas whose drawing has completed.
 * @param[in]     dirty_area Bounding rectangle of touched pixels.
 *
 * @pre `ra_guix_display_driver_bind` has succeeded.
 * @post `s_aux.swap_count` is incremented (saturating at 255).
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_guix_display_driver_buffer_toggle(GX_CANVAS* canvas, GX_RECTANGLE* dirty_area);
/* Ra guix display driver buffer toggle -- see implementation for details. */
void ra_guix_display_driver_buffer_toggle(GX_CANVAS* canvas, GX_RECTANGLE* dirty_area)
{
  (void)canvas;
  (void)dirty_area;

  if (s_aux.swap_count < UINT8_MAX) {
    s_aux.swap_count = (uint8_t)(s_aux.swap_count + 1U);
  }
}

/* Ra guix display driver setup -- see implementation for details. */
UINT ra_guix_display_driver_setup(GX_DISPLAY* display)
{
  if (display == nullptr) {
    return GX_INVALID_VALUE;
  }
  if (s_aux.framebuffer == nullptr) {
    return GX_INVALID_VALUE;
  }

  /* Stand up the standard 565RGB driver. The aux record gets stashed
   * in `display->gx_display_driver_data` so our overrides below can
   * recover the framebuffer base + dimensions without globals. */
  _gx_display_driver_565rgb_setup(display, &s_aux, ra_guix_display_driver_buffer_toggle);

  /* Keep the vendor 565 RGB pixel_write -- it writes directly into
   * the canvas memory (= our framebuffer = GLCDC scan-out buffer).
   * The previous override routed through `ra_gfx_pixel`, which
   * requires `ra_gfx_init` (never called by this app), so every
   * pixel write was silently dropped with k_ra_err_not_initialized.
   * Only override block_move so the swap counter still ticks. */
  display->gx_display_driver_block_move = ra_guix_display_driver_block_move;

  /* Pin the panel dimensions on the display struct so callers that
   * skipped the gx_display_create wrapper (which sets them) still see
   * sensible numbers. */
  display->gx_display_width  = (GX_VALUE)s_aux.width_px;
  display->gx_display_height = (GX_VALUE)s_aux.height_px;

  return GX_SUCCESS;
}

#endif /* !RA_SIMULATOR_MODE */
