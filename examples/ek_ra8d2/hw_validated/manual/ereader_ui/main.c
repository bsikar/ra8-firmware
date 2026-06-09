/**
 * @file examples/ek_ra8d2/hw_validated/manual/ereader_ui/main.c
 * @brief E-reader device chrome -- Reading screen (non-GUIX, ra_gfx)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * First milestone of the e-reader UI (issue #80): render the device
 * "chrome" -- the application shell around book content -- without GUIX
 * (which is being retired, #81). This app paints the Reading screen
 * directly into the GLCDC framebuffer through ``libs/ra_gfx`` primitives,
 * in the flat 16-level-grayscale / fixed-type-scale visual language of
 * the verified browser proof-of-concept ("PAPYR").
 *
 * Boot phases mirror the other full-panel manual demos:
 *   1. ``app_bringup_clocks``  -- CGC + MSTP + SysTick + board LEDs.
 *   2. ``app_bringup_panel``   -- ``ra_sdramc_init`` (the 1024x600 RGB565
 *      framebuffer lives in external SDRAM) then ``display_init`` (panel
 *      power + GLCDC routing, scanning out ``s_framebuffer``).
 *   3. ``app_bringup_gfx``     -- bind ``ra_gfx`` to that framebuffer.
 *
 * The chrome layout is resolution-adaptive: every region is derived from
 * the framebuffer dimensions the backend reports, so a different panel
 * descriptor reflows the shell without code changes.
 *
 * Scope of THIS file (Phase A): the Reading screen chrome -- status bar,
 * body text area, footer with page count + reading-progress bar. Body
 * text is rendered with the bundled bitmap font here; the next milestone
 * routes real paginated book text through ``libs/ra_reflow`` (stb_truetype
 * glyphs at the 48/34/24/18 type scale) and adds the Library screen plus
 * the page-turn / navigation controller.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_sdramc.h"
#include "ra_time.h"

/* ===========================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =========================================================================== */

/**
 * @enum er_fb_dim_t
 * @brief Framebuffer dimensions, sourced from the BSP panel descriptor.
 *
 * @details
 * A full 1024x600 RGB565 framebuffer is 1024 * 600 * 2 = 1.2 MiB, which
 * does not fit alongside .data/.bss/stack in SRAM, so it lives in
 * external SDRAM (see ``s_framebuffer``). Geometry comes from
 * ``ra_panel.h`` -- a different panel swaps in its own descriptor.
 */
typedef enum : uint16_t {
  k_er_fb_w = k_panel_width_px,  /**< Framebuffer width  (pixels). */
  k_er_fb_h = k_panel_height_px, /**< Framebuffer height (pixels). */
} er_fb_dim_t;

/**
 * @enum er_fb_misc_t
 * @brief Framebuffer byte-math and pacing constants.
 */
typedef enum : uint16_t {
  k_er_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.       */
  k_er_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle.    */
  k_er_frame_ms  = 100U, /**< Heartbeat / flush pacing (ms).     */
} er_fb_misc_t;

/**
 * @enum er_color_t
 * @brief PAPYR 16-level grayscale ramp, semantic roles (0xRRGGBB).
 *
 * @details
 * The e-ink design language is grayscale only -- no gradients, shadows,
 * blur, or transparency. These are the working tones from the POC token
 * sheet: paper (g15), ink (g0), muted ink (g5), hairline rule (g10),
 * soft rule (g12), flat fill (g13), deep fill (g8).
 */
typedef enum : uint32_t {
  k_er_paper     = 0xFFFFFFU, /**< Page background (g15).             */
  k_er_ink       = 0x000000U, /**< Primary text / glyphs (g0).        */
  k_er_ink_muted = 0x555555U, /**< Secondary text / metadata (g5).    */
  k_er_rule      = 0xAAAAAAU, /**< Hairline dividers (g10).           */
  k_er_rule_soft = 0xCCCCCCU, /**< Faintest hairlines / track (g12).  */
  k_er_fill      = 0xDDDDDDU, /**< Flat fills (g13).                  */
  k_er_fill_deep = 0x888888U, /**< Heavier fill / progress (g8).      */
} er_color_t;

/**
 * @enum er_layout_t
 * @brief Reading-screen layout metrics (8 px grid), pixels.
 *
 * @details
 * Adapted from the POC token sheet for the landscape dev panel. The
 * bundled font is a fixed 8x16 cell (``ra_gfx_font_8x16``); ``k_er_line_h``
 * gives it generous reading leading. Real type-scale rendering arrives
 * with the ra_reflow body-text milestone.
 */
typedef enum : uint16_t {
  k_er_statusbar_h  = 64U, /**< Top status-bar band height.         */
  k_er_footer_h     = 56U, /**< Bottom footer band height.          */
  k_er_margin_x     = 64U, /**< Reading left/right margin.           */
  k_er_pad_ui       = 32U, /**< Status-bar / footer side padding.   */
  k_er_body_gap     = 24U, /**< Gap between band and body text.      */
  k_er_line_h       = 26U, /**< Body line advance.                  */
  k_er_glyph_w      = 8U,  /**< Bundled font cell width.            */
  k_er_glyph_h      = 16U, /**< Bundled font cell height.           */
  k_er_text_inset_y = 24U, /**< 16 px text centred in a 64 px band. */
  k_er_hair         = 1U,  /**< Hairline weight.                   */
  k_er_progress_h   = 6U,  /**< Reading-progress bar height.        */
  k_er_progress_gap = 12U, /**< Gap above the progress bar.         */
  k_er_blank_lines  = 2U,  /**< Blank advance after the heading.    */
} er_layout_t;

/**
 * @enum er_progress_t
 * @brief Demo reading position (current / total pages).
 */
typedef enum : uint16_t {
  k_er_page_current = 12U,  /**< Current page (1-based).            */
  k_er_page_total   = 248U, /**< Total pages in the chapter set.    */
} er_progress_t;

/* ===========================================================================
 * Static chrome strings (ASCII; book content is public-domain H.G. Wells).
 * =========================================================================== */

/** @brief Wordmark shown at the status-bar left. */
static const char k_er_wordmark[] = "PAPYR";
/** @brief Current book title (status-bar centre / footer left). */
static const char k_er_book_title[] = "The Time Machine";
/** @brief Status-bar right cluster: clock + battery. */
static const char k_er_status_right[] = "10:24   98%";
/** @brief Footer page indicator. */
static const char k_er_page_label[] = "Page 12 of 248";
/** @brief Reading-view chapter heading. */
static const char k_er_chapter[] = "I";

/**
 * @brief Body paragraph lines (pre-wrapped for the bundled font).
 *
 * @details
 * Opening of "The Time Machine" (H.G. Wells, 1895, public domain). These
 * stand in for ra_reflow-paginated EPUB content until the body-text
 * milestone lands; they exercise the full body region with realistic
 * prose at the reading margin.
 */
static const char* const k_er_body_lines[] = {
  "The Time Traveller (for so it will be convenient to speak of him)",
  "was expounding a recondite matter to us. His pale grey eyes shone",
  "and twinkled, and his usually pale face was flushed and animated.",
  "",
  "The fire burnt brightly, and the soft radiance of the incandescent",
  "lights in the lilies of silver caught the bubbles that flashed and",
  "passed in our glasses. Our chairs, being his patents, embraced and",
  "caressed us rather than submitted to be sat upon, and there was that",
  "luxurious after-dinner atmosphere when thought roams gracefully free",
  "of the trammels of precision. And he put it to us in this way --",
  "marking the points with a lean forefinger -- as we sat and lazily",
  "admired his earnestness over this new paradox (as we thought it).",
};

/**
 * @enum er_body_count_t
 * @brief Number of pre-wrapped body lines.
 */
typedef enum : uint16_t {
  k_er_body_line_count = (uint16_t)(sizeof(k_er_body_lines) / sizeof(k_er_body_lines[0])),
} er_body_count_t;

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned.
 *
 * @details
 * Placed in the linker's ``.sdram_data`` (NOLOAD) section so it lands at
 * 0x68000000. ``display_init`` programmes GLCDC GR1 to scan it out; this
 * app paints into it directly via ra_gfx.
 */
static uint16_t s_framebuffer[(size_t)k_er_fb_h * (size_t)k_er_fb_w]
  __attribute__((section(".sdram_data"), aligned(k_er_fb_align)));

/**
 * @var k_er_display_cfg
 * @brief Display PAL config -- LCD/GLCDC backend over the SDRAM buffer.
 */
static const display_cfg_t k_er_display_cfg = {
  .iface             = &k_display_backend_lcd_ra_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_er_fb_w,
  .height_px         = (uint16_t)k_er_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/* ===========================================================================
 * Boot helpers
 * =========================================================================== */

/**
 * @brief Halt forever with the red board LED on.
 *
 * @details Stuck red LED is the visual sign of a fatal init error.
 *
 * @pre None.
 * @pre None.
 * @post Function never returns; CPU parked in a WFI loop.
 * @post Red LED is on.
 *
 * @note IRQ-safe (no shared state mutated after entry).
 * @since 0.1.0
 */
static void app_panic_halt(void)
{
  (void)ra_board_led_on(k_ra_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, system tick, and the two board LEDs.
 *
 * @details Same boot order the other full-panel manual demos use.
 *
 * @pre Reset_Handler ran .data/.bss init.
 * @pre IRQs are still globally disabled.
 * @post Clocks, MSTP, ra_time and both board LEDs are initialised.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_blue) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_red) != k_ra_ok) {
    app_panic_halt();
  }
  ra_isr_globals_enable();
}

/**
 * @brief Bring up external SDRAM and the GLCDC panel, then cache the FB.
 *
 * @details
 * A settle delay lets the PLLs, SDRAM, and panel POR stabilise.
 * ``ra_sdramc_init`` brings up the 0x68000000 region the framebuffer
 * lives in; ``display_init`` folds panel power-on + GLCDC routing into
 * one call (driving GR1 to scan out ``s_framebuffer``).
 *
 * @pre ``app_bringup_clocks`` has run; IRQs are enabled.
 * @pre ``s_framebuffer`` is reachable (zero-init in SDRAM is fine).
 * @post SDRAM is up; GLCDC scans out ``s_framebuffer`` on GR1.
 * @post ``s_display`` is non-NULL and ``s_fb`` mirrors the backend FB.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_panel(void)
{
  ra_delay_ms((uint32_t)k_er_settle_ms);
  if (ra_sdramc_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (display_init(&k_er_display_cfg, &s_display) != k_ra_ok) {
    app_panic_halt();
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Bind ra_gfx to the panel framebuffer.
 *
 * @details After this, all chrome drawing goes through ra_gfx primitives.
 *
 * @pre ``app_bringup_panel`` has run; ``s_fb.pixels`` is reachable.
 * @pre The framebuffer is RGB565.
 * @post ra_gfx draw calls operate on ``s_framebuffer``.
 * @post On failure the app panic-halts and never returns.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_gfx(void)
{
  if (ra_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra_gfx_format_rgb565) != k_ra_ok) {
    app_panic_halt();
  }
}

/* ===========================================================================
 * Chrome rendering -- Reading screen
 * =========================================================================== */

/**
 * @brief Draw a left-anchored text run in the bundled font.
 *
 * @param[in] x     Left pixel column.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra_gfx is bound; ``str`` is non-NULL.
 * @pre ``str`` holds only codepoints 0x20..0x7E.
 * @post The run is blitted within the framebuffer (clipped if needed).
 * @post Cells behind the glyphs are filled paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_text_left(int32_t x, int32_t y, const char* str, uint32_t color)
{
  (void)ra_gfx_text_out(x, y, str, &ra_gfx_font_8x16, color, (uint32_t)k_er_paper);
}

/**
 * @brief Draw a right-anchored text run ending at column @p right.
 *
 * @param[in] right Right pixel column the run ends at.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra_gfx is bound; ``str`` is non-NULL.
 * @pre ``str`` holds only codepoints 0x20..0x7E.
 * @post The run is right-aligned so its last glyph ends near @p right.
 * @post Cells behind the glyphs are filled paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_text_right(int32_t right, int32_t y, const char* str, uint32_t color)
{
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra_gfx_text_size(str, &ra_gfx_font_8x16, &w, &h) != k_ra_ok) {
    return;
  }
  er_text_left(right - (int32_t)w, y, str, color);
}

/**
 * @brief Paint the top status bar (wordmark, title, clock/battery, rule).
 *
 * @param[in] width Framebuffer width in pixels.
 *
 * @pre ra_gfx is bound.
 * @pre ``width`` matches the bound framebuffer.
 * @post The status band and its bottom hairline are drawn on paper.
 * @post No pixels below ``k_er_statusbar_h`` are touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_draw_status_bar(int32_t width)
{
  const int32_t text_y = (int32_t)k_er_text_inset_y;
  er_text_left((int32_t)k_er_pad_ui, text_y, k_er_wordmark, (uint32_t)k_er_ink);

  const int32_t title_x =
    (int32_t)k_er_pad_ui + ((int32_t)sizeof(k_er_wordmark) * (int32_t)k_er_glyph_w);
  er_text_left(title_x, text_y, k_er_book_title, (uint32_t)k_er_ink_muted);

  er_text_right(width - (int32_t)k_er_pad_ui, text_y, k_er_status_right, (uint32_t)k_er_ink_muted);

  (void)ra_gfx_rect(0,
                    (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                    width,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);
}

/**
 * @brief Paint the reading body: chapter heading then paragraph lines.
 *
 * @param[in] height Framebuffer height in pixels.
 *
 * @pre ra_gfx is bound.
 * @pre ``height`` matches the bound framebuffer.
 * @post Chapter heading and as many body lines as fit are drawn in ink.
 * @post Drawing stops before the footer band.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_draw_body(int32_t height)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  const int32_t body_bot = height - (int32_t)k_er_footer_h - (int32_t)k_er_body_gap;

  er_text_left((int32_t)k_er_margin_x, body_top, k_er_chapter, (uint32_t)k_er_ink);

  int32_t y = body_top + ((int32_t)k_er_blank_lines * (int32_t)k_er_line_h);
  for (uint16_t i = 0U; i < (uint16_t)k_er_body_line_count; ++i) {
    if ((y + (int32_t)k_er_glyph_h) > body_bot) {
      break;
    }
    er_text_left((int32_t)k_er_margin_x, y, k_er_body_lines[i], (uint32_t)k_er_ink);
    y += (int32_t)k_er_line_h;
  }
}

/**
 * @brief Paint the footer: top rule, title, page label, progress bar.
 *
 * @param[in] width  Framebuffer width in pixels.
 * @param[in] height Framebuffer height in pixels.
 *
 * @pre ra_gfx is bound.
 * @pre ``width``/``height`` match the bound framebuffer; total pages > 0.
 * @post Footer rule, labels, and a flat reading-progress bar are drawn.
 * @post No pixels above ``height - k_er_footer_h`` are touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_draw_footer(int32_t width, int32_t height)
{
  const int32_t band_top = height - (int32_t)k_er_footer_h;

  (void)ra_gfx_rect(0, band_top, width, (int32_t)k_er_hair, (uint32_t)k_er_rule, true);

  const int32_t text_y = band_top + (int32_t)k_er_progress_gap;
  er_text_left((int32_t)k_er_pad_ui, text_y, k_er_book_title, (uint32_t)k_er_ink_muted);
  er_text_right(width - (int32_t)k_er_pad_ui, text_y, k_er_page_label, (uint32_t)k_er_ink_muted);

  const int32_t track_x = (int32_t)k_er_pad_ui;
  const int32_t track_w = width - (2 * (int32_t)k_er_pad_ui);
  const int32_t track_y = height - (int32_t)k_er_progress_gap - (int32_t)k_er_progress_h;
  (void)ra_gfx_rect(track_x,
                    track_y,
                    track_w,
                    (int32_t)k_er_progress_h,
                    (uint32_t)k_er_rule_soft,
                    true);

  const int32_t fill_w = (track_w * (int32_t)k_er_page_current) / (int32_t)k_er_page_total;
  (void)
    ra_gfx_rect(track_x, track_y, fill_w, (int32_t)k_er_progress_h, (uint32_t)k_er_fill_deep, true);
}

/**
 * @brief Render the full Reading screen into the framebuffer.
 *
 * @details Clears to paper, then paints status bar, body, and footer.
 *
 * @pre ra_gfx is bound to the panel framebuffer.
 * @pre ``s_fb`` reflects the backend framebuffer geometry.
 * @post Every pixel of the framebuffer holds the Reading screen.
 * @post Caller flushes the panel to make it visible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_reading(void)
{
  const int32_t width  = (int32_t)s_fb.width_px;
  const int32_t height = (int32_t)s_fb.height_px;

  (void)ra_gfx_clear((uint32_t)k_er_paper);
  er_draw_status_bar(width);
  er_draw_body(height);
  er_draw_footer(width, height);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  app_bringup_clocks();
  app_bringup_panel();
  app_bringup_gfx();

  er_render_reading();
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_init);

  while (1) {
    (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_fast);
    (void)ra_board_led_toggle(k_ra_board_led_blue);
    ra_delay_ms((uint32_t)k_er_frame_ms);
  }
  return 0;
}
#pragma GCC diagnostic pop
