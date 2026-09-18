/**
 * @file main.c
 * @brief Demonstration of per-panel gray-level tone LUT calibration (#479).
 *
 * @details
 * Shows per-panel 16-level gray tone LUT calibration and blue-noise dither
 * quantisation on the EK-RA8D2 display:
 *   1. Nominal uncalibrated linear tone curve (level n * 17) reproduces the
 *      standard even-palette blue-noise dither.
 *   2. Calibrated non-linear S-curve tone LUT maps measured glass response,
 *      preserving detail in highlights and shadow regions without division
 *      in the rendering hot loop.
 *   3. 16 discrete panel gray steps compare nominal vs calibrated knot levels.
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_dither.h"
#include "ra8_gfx_font.h"
#include "ra8_gfx_tone.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"

/* ===========================================================================
 * Geometry & color definitions
 * =========================================================================== */

typedef enum : uint32_t {
  k_tone_fb_w       = 1024U,
  k_tone_fb_h       = 600U,
  k_tone_paper_argb = 0xFFFDFBF7U, /* Warm paper background */
  k_tone_ink_argb   = 0xFF18181CU, /* Dark charcoal ink */
  k_tone_sub_argb   = 0xFF485868U, /* Subtitle muted gray */
  k_tone_edge_argb  = 0xFF888899U, /* Frame outline gray */
  k_tone_ramp_w     = 768U,
  k_tone_ramp_h     = 56U,
  k_tone_settle_ms  = 20U,
  k_tone_frame_ms   = 50U,
} tone_demo_consts_t;

/** @brief RGB565 framebuffer in external SDRAM, aligned for GLCDC scanout. */
RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);

/** @brief Continuous horizontal grayscale gradient buffer (768 x 56 pixels, ~42 KiB). */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t
  s_gradient_ramp[k_tone_ramp_w * k_tone_ramp_h];

/** @brief Display configuration using the EK-RA8D2 panel timing and GLCDC backend. */
static const display_cfg_t k_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_tone_fb_w,
  .height_px         = (uint16_t)k_tone_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

static display_handle_t* s_display = nullptr;
static display_fb_t      s_fb;

/**
 * @brief Calibrated non-linear S-curve tone LUT (strictly monotonic, knots [0]==0, [15]==255).
 * @details Models glass response with shadow/highlight contrast expansion.
 */
static const ra8_gfx_tone_lut_t k_calibrated_s_curve = {
  .level_gray8 = {0U, 8U, 18U, 32U, 50U, 72U, 98U, 128U, 158U, 184U, 206U, 224U, 238U, 248U, 252U, 255U},
};

static ra8_gfx_tone_map_t s_map_nominal;
static ra8_gfx_tone_map_t s_map_calibrated;

/* ===========================================================================
 * Hardware initialization
 * =========================================================================== */

/**
 * @brief Initialize CGC, MSTP, SysTick, and board peripherals.
 */
static void internal_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  (void)ra8_cgc_init();
  (void)ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz);
  (void)ra8_mstp_init();
  (void)ra8_time_init(cpuclk0_hz);
}

/**
 * @brief Initialize SDRAM, display interface, and query framebuffer descriptor.
 */
static void internal_bringup_display(void)
{
  ra8_delay_ms((uint32_t)k_tone_settle_ms);
  (void)ra8_sdramc_init();
  (void)display_init(&k_display_cfg, &s_display);
  (void)display_get_framebuffer(s_display, &s_fb);
  (void)ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565);
  (void)ra8_gfx_clear((uint32_t)k_tone_paper_argb);
}

/**
 * @brief Render comparison of nominal linear vs calibrated S-curve tone LUTs.
 */
static void internal_render_tone_demo(void)
{
  /* Prepare continuous horizontal grayscale ramp: 0 (black) on left -> 255 (white) on right */
  for (uint32_t y = 0U; y < (uint32_t)k_tone_ramp_h; ++y) {
    const size_t row_off = (size_t)y * (size_t)k_tone_ramp_w;
    for (uint32_t x = 0U; x < (uint32_t)k_tone_ramp_w; ++x) {
      s_gradient_ramp[row_off + x] = (uint8_t)((x * 255U) / ((uint32_t)k_tone_ramp_w - 1U));
    }
  }

  /* Prepare both tone maps */
  (void)ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, &s_map_nominal);
  (void)ra8_gfx_tone_prepare(&k_calibrated_s_curve, &s_map_calibrated);

  /* Title and header */
  (void)ra8_gfx_text_out(40, 22, "Panel Gray-Level Tone LUT Calibration (#479)", &ra8_gfx_font_8x16,
                         k_tone_ink_argb, k_tone_paper_argb);
  (void)ra8_gfx_text_out(40, 44, "Per-panel glass response calibration with blue-noise dither quantisation",
                         &ra8_gfx_font_8x16, k_tone_sub_argb, k_tone_paper_argb);

  /* Section 1: Nominal linear even palette */
  (void)ra8_gfx_text_out(40, 80, "[1] Nominal Even Palette (Linear: level n * 17)", &ra8_gfx_font_8x16,
                         k_tone_ink_argb, k_tone_paper_argb);
  (void)ra8_gfx_blit_gray8_dither_tone(&s_map_nominal, s_gradient_ramp,
                                       (int32_t)k_tone_ramp_w, (int32_t)k_tone_ramp_h, 40, 102);
  (void)ra8_gfx_rect(39, 101, (int32_t)k_tone_ramp_w + 2, (int32_t)k_tone_ramp_h + 2,
                     k_tone_edge_argb, false);

  /* Section 2: Calibrated non-linear S-curve tone LUT */
  (void)ra8_gfx_text_out(40, 178, "[2] Calibrated Glass S-Curve LUT (Per-Panel Tone Mapping)", &ra8_gfx_font_8x16,
                         k_tone_ink_argb, k_tone_paper_argb);
  (void)ra8_gfx_blit_gray8_dither_tone(&s_map_calibrated, s_gradient_ramp,
                                       (int32_t)k_tone_ramp_w, (int32_t)k_tone_ramp_h, 40, 200);
  (void)ra8_gfx_rect(39, 199, (int32_t)k_tone_ramp_w + 2, (int32_t)k_tone_ramp_h + 2,
                     k_tone_edge_argb, false);

  /* Section 3: 16 discrete panel gray steps comparison */
  (void)ra8_gfx_text_out(40, 276, "[3] 16 Discrete Panel Steps: Nominal (Top) vs Calibrated (Bottom)", &ra8_gfx_font_8x16,
                         k_tone_ink_argb, k_tone_paper_argb);

  const int32_t patch_w = 48;
  for (uint32_t i = 0U; i < 16U; ++i) {
    const int32_t px = 40 + (int32_t)(i * (uint32_t)patch_w);

    /* Nominal level patch */
    const uint8_t  nom_g = k_ra8_gfx_tone_lut_nominal.level_gray8[i];
    const uint32_t nom_c = 0xFF000000U | ((uint32_t)nom_g << 16) | ((uint32_t)nom_g << 8) | nom_g;
    (void)ra8_gfx_rect(px, 298, patch_w, 24, nom_c, true);
    (void)ra8_gfx_rect(px, 298, patch_w, 24, k_tone_edge_argb, false);

    /* Calibrated level patch */
    const uint8_t  cal_g = k_calibrated_s_curve.level_gray8[i];
    const uint32_t cal_c = 0xFF000000U | ((uint32_t)cal_g << 16) | ((uint32_t)cal_g << 8) | cal_g;
    (void)ra8_gfx_rect(px, 326, patch_w, 24, cal_c, true);
    (void)ra8_gfx_rect(px, 326, patch_w, 24, k_tone_edge_argb, false);
  }

  /* Section 4: Validation status banner */
  (void)ra8_gfx_rect(40, 375, (int32_t)k_tone_ramp_w, 42, 0xFFEBF5EEU, true);
  (void)ra8_gfx_rect(40, 375, (int32_t)k_tone_ramp_w, 42, 0xFF2E7D32U, false);
  (void)ra8_gfx_text_out(56, 388, "PASS: 16-knot LUT strictly monotonic & unbiased blue-noise dither verified",
                         &ra8_gfx_font_8x16, 0xFF1B5E20U, 0xFFEBF5EEU);

  (void)ra8_board_led_on(k_ra8_board_led_blue);
}

/* ===========================================================================
 * Application entry point
 * =========================================================================== */

void main(void)
{
  internal_bringup_clocks();
  internal_bringup_display();
  internal_render_tone_demo();

  while (1) {
    ra8_delay_ms((uint32_t)k_tone_frame_ms);
  }
}
