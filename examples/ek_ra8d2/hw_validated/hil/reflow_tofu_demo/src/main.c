/**
 * @file main.c
 * @brief Demonstration of missing-glyph Tofu box fallback in the reflow layout engine (#686/#687).
 *
 * @details
 * The baked Literata font carries only 198 codepoints (ASCII + Latin-1 subset).
 * This application lays out and renders text containing characters outside the
 * font's character map (Kanji, Greek, Cyrillic, and math symbols).
 *
 * With the PR #686 / #687 fixes:
 *   1. UTF-8 multibyte sequences are decoded into full Unicode codepoints during
 *      layout and walk.
 *   2. Glyphs missing from the font face cleanly trigger the notdef tofu fallback,
 *      drawing an outlined rectangular box with correct typographic advance
 *      instead of crashing (Hard Fault) or producing corrupt visual artifacts.
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "literata_latin1.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_panel_timing.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "reflow.h"

/* ===========================================================================
 * Geometry & color definitions
 * =========================================================================== */

typedef enum : uint32_t {
  k_tofu_fb_w       = 1024U,
  k_tofu_fb_h       = 600U,
  k_tofu_font_px    = 22U,
  k_tofu_paper_argb = 0xFFFDFBF7U, /* Warm paper background */
  k_tofu_ink_argb   = 0xFF18181CU, /* Dark charcoal ink */
  k_tofu_link_argb  = 0xFF1C3A5EU, /* Slate blue link */
  k_tofu_settle_ms  = 20U,
  k_tofu_frame_ms   = 50U,
} tofu_demo_consts_t;

/** @brief RGB565 framebuffer in external SDRAM, aligned for GLCDC scanout. */
RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);

/** @brief Display configuration using the EK-RA8D2 panel timing and GLCDC backend. */
static const display_cfg_t k_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_tofu_fb_w,
  .height_px         = (uint16_t)k_tofu_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

static display_handle_t* s_display = nullptr;
static display_fb_t      s_fb;
static reflow_t          s_engine;

/** @brief Demonstration chapter text containing both supported Latin-1 and unmapped characters. */
static const char k_demo_xhtml[] =
  "<html><body>"
  "<h1>Tofu Box Fallback (#686/#687)</h1>"
  "<p>Latin-1: Cafe</p>"
  "<p>Missing Kanji: [&#x6F22;&#x5B57;]</p>"
  "<p>Missing Greek: [&Omega; &alpha;]</p>"
  "<p>Missing Math: [&sum; &int;]</p>"
  "</body></html>";

/* ===========================================================================
 * Hardware initialization
 * =========================================================================== */

/**
 * @brief Initialize CGC, MSTP, SysTick, and board LEDs.
 * @return void
 * @since 0.1.0
 */
static void internal_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  (void)ra8_cgc_init();
  (void)ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz);
  (void)ra8_mstp_init();
  (void)ra8_time_init(cpuclk0_hz);
  (void)ra8_board_led_init(k_ra8_board_led_blue);
  (void)ra8_board_led_init(k_ra8_board_led_green);
  ra8_isr_globals_enable();
}

/**
 * @brief Bring up SDRAM, GLCDC display controller, and ra8_gfx.
 * @return void
 * @since 0.1.0
 */
static void internal_bringup_display(void)
{
  ra8_delay_ms((uint32_t)k_tofu_settle_ms);
  (void)ra8_sdramc_init();
  (void)display_init(&k_display_cfg, &s_display);
  (void)display_get_framebuffer(s_display, &s_fb);
  (void)ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565);
  (void)ra8_gfx_clear((uint32_t)k_tofu_paper_argb);
}

/**
 * @brief Layout and render the demo chapter containing missing glyphs.
 * @return void
 * @since 0.1.0
 */
static void internal_render_demo(void)
{
  const ra8_err_t rinit = reflow_init((uint16_t)k_tofu_fb_w,
                                      (uint16_t)k_tofu_fb_h,
                                      g_ra8_font_literata_latin1,
                                      g_ra8_font_literata_latin1_len,
                                      (uint16_t)k_tofu_font_px,
                                      (uint32_t)k_tofu_ink_argb,
                                      (uint32_t)k_tofu_link_argb,
                                      &s_engine);
  if (rinit != k_ra8_ok) {
    return;
  }

  uint32_t pages = 0U;
  (void)reflow_layout_chapter(&s_engine,
                              (const uint8_t*)k_demo_xhtml,
                              (uint32_t)(sizeof(k_demo_xhtml) - 1U),
                              &pages);

  (void)reflow_render_page(&s_engine, 0U, nullptr);
  (void)ra8_board_led_on(k_ra8_board_led_blue);
}

/**
 * @brief Firmware entry point.
 * @since 0.1.0
 */
void main(void)
{
  internal_bringup_clocks();
  internal_bringup_display();
  internal_render_demo();

  while (1) {
    ra8_delay_ms((uint32_t)k_tofu_frame_ms);
  }
}
