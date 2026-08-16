/**
 * @file examples/ek_ra8d2/hw_validated/manual/display_pal_animation/main.c
 * @brief Canonical display PAL example -- animated scrolling colour bars
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Reference example for ``libs/ra8_display_pal``. Paints six
 * horizontal RGB565 colour bars that scroll vertically once per
 * frame, demonstrating every public PAL entry point:
 *
 *   - ``display_init``           -- one call, no GLCDC details.
 *   - ``display_get_caps``       -- query the bound backend.
 *   - ``display_get_framebuffer``-- grab the FB the backend uses.
 *   - ``display_clear``          -- whole-screen pre-paint clear.
 *   - ``display_flush``          -- commit + refresh hint.
 *   - ``display_full_rect``      -- one-shot full-screen rect.
 *
 * No call to ``ra8_glcdc_*`` or ``ra8_board_lcd_panel_power_on`` --
 * everything goes through the PAL. Swap to the IT8951 e-ink
 * backend by changing one line of ``k_app_display_cfg`` below
 * (point ``iface`` at ``&k_display_backend_eink_it8951`` from
 * ``ra8_display_pal_eink.h``); the rest of this file does not
 * change. On the stub e-ink backend ``display_flush`` returns
 * ``k_ra8_err_not_supported`` until silicon arrives.
 *
 * The app branches on ``caps.continuous_refresh`` to decide
 * whether to call ``display_flush`` every frame (e-ink) or only
 * once per full repaint (LCD scans continuously, so flush is a
 * cheap memory barrier).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_time.h"

/* ===========================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =========================================================================== */

/**
 * @brief Framebuffer dimensions for this demo.
 *
 * @details Same 512 x 512 size as ``lcd_draw_x``: 0.5 MiB RGB565
 *          fits in the on-chip SRAM, and the GLCDC BG plane fills
 *          the rest of the 1024 x 600 panel.
 */
typedef enum : uint16_t {
  k_app_fb_w = 512U, /**< Framebuffer width  (pixels). */
  k_app_fb_h = 512U, /**< Framebuffer height (pixels). */
} app_fb_dim_t;

/**
 * @brief Number of horizontal colour bars rendered per frame.
 */
typedef enum : uint8_t {
  k_app_bar_count = 6U, /**< App bar count. */
} app_bar_count_t;

/**
 * @brief RGB565 palette for the bar colours.
 *
 * @details Six high-saturation colours so the scrolling pattern is
 *          easy to verify on the panel: red, yellow, green, cyan,
 *          blue, magenta.  Wraps every ``k_app_bar_count`` rows.
 */
typedef enum : uint16_t {
  k_app_color_red     = 0xF800U, /**< App color red.     */
  k_app_color_yellow  = 0xFFE0U, /**< App color yellow.  */
  k_app_color_green   = 0x07E0U, /**< App color green.   */
  k_app_color_cyan    = 0x07FFU, /**< App color cyan.    */
  k_app_color_blue    = 0x001FU, /**< App color blue.    */
  k_app_color_magenta = 0xF81FU, /**< App color magenta. */
} app_color_t;

/**
 * @brief Frame pacing in milliseconds.
 *
 * @details 60 ms = ~16.7 fps. Slow enough to see the bars scroll;
 *          fast enough that the panel feels responsive.
 */
typedef enum : uint16_t {
  k_app_frame_period_ms = 60U,  /**< App frame period ms.         */
  k_app_powerup_ms      = 500U, /**< PLL / panel power-on settle. */
} app_pace_t;

typedef enum : uint16_t {
  k_app_fb_align_bytes = 64U, /**< AXI-burst alignment for clean GLCDC fetches. */
} app_fb_align_t;

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in SRAM, 64-byte AXI-burst aligned.
 *
 * @details The PAL hands this pointer to the GLCDC HAL via
 *          ``display_init``. Apps that target e-ink reuse this
 *          same RGB565 buffer -- the backend converts to greyscale
 *          on flush.
 */
[[gnu::aligned(k_app_fb_align_bytes)]] static uint16_t
  s_framebuffer[(uint32_t)k_app_fb_w * (uint32_t)k_app_fb_h];

/**
 * @var k_app_display_cfg
 * @brief Display PAL config -- the one-line backend swap lives here.
 *
 * @details
 * To target the IT8951-compatible e-ink panel, change ``iface`` to
 * ``&k_display_backend_eink_it8951`` (from
 * ``ra8_display_pal_eink.h``). Everything else in this file -- the
 * paint loop, the heartbeat, the flush-on-frame -- is identical
 * regardless of backend.
 */
static const display_cfg_t k_app_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_app_fb_w,
  .height_px         = (uint16_t)k_app_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the backend caps; populated at boot. */
static display_caps_t s_caps;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/** @brief Scroll offset advanced every frame. */
static uint32_t s_scroll_offset = 0U;

/* ===========================================================================
 * Panic helper -- single-purpose, matches the rest of the lcd_* demos.
 * =========================================================================== */

static const uint16_t s_palette[k_app_bar_count] = {
  (uint16_t)k_app_color_red,
  (uint16_t)k_app_color_yellow,
  (uint16_t)k_app_color_green,
  (uint16_t)k_app_color_cyan,
  (uint16_t)k_app_color_blue,
  (uint16_t)k_app_color_magenta,
};

/**
 * @brief Halt forever with the red board LED on.
 *
 * @details The blue LED heartbeat (see ``main``) stops too, so a
 *          stuck red LED is the visual sign of a fatal init error.
 *
 * @pre None.
 * @pre None.
 * @post Function never returns; CPU is parked in WFI loop.
 * @post Red LED is on.
 *
 * @note IRQ-safe (no shared state mutated after entry).
 * @since 0.1.0
 */
static void app_panic_halt(void)
{
  (void)ra8_board_led_on(k_ra8_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, system tick and the two board LEDs.
 *
 * @details Mirrors the same boot order ``lcd_draw_x`` uses.
 *
 * @pre Reset_Handler ran .data/.bss init.
 * @pre IRQs are still globally disabled.
 * @post Clocks, MSTP, ra8_time and both board LEDs are initialised.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    app_panic_halt();
  }
  ra8_isr_globals_enable();
}

/**
 * @brief Bring up the display through the PAL and cache caps + FB.
 *
 * @details Replaces every direct GLCDC/board call with the PAL's
 *          ``display_init`` + ``get_caps`` + ``get_framebuffer``.
 *          On a successful return the panel is scanning out
 *          ``s_framebuffer`` and the app can paint freely.
 *
 * @pre ``app_bringup_clocks`` has run.
 * @pre ``s_framebuffer`` is alive (zero-init in .bss is fine).
 * @post ``s_display`` is non-NULL.
 * @post ``s_caps`` and ``s_fb`` mirror what the backend reported.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_display(void)
{
  if (display_init(&k_app_display_cfg, &s_display) != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_get_caps(s_display, &s_caps) != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Paint one frame's worth of scrolling colour bars.
 *
 * @details Iterates pixel-by-pixel for clarity; a DMA-driven
 *          variant would obviously be faster. The bar a pixel
 *          belongs to is decided by ``(y + scroll_offset) /
 *          (height / bar_count)`` modulo the palette length.
 *
 * @param[in] scroll_offset Pixel offset added to each row's
 *                          palette lookup (advanced once per frame).
 *
 * @pre ``app_bringup_display`` has run.
 * @pre ``s_fb.pixels`` is reachable.
 * @post Every pixel of the framebuffer has been overwritten.
 * @post DSB barrier issued via the PAL flush below.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_paint_bars(uint32_t scroll_offset)
{
  uint16_t*      pixels     = (uint16_t*)s_fb.pixels;
  const uint16_t bar_height = (uint16_t)(s_fb.height_px / (uint16_t)k_app_bar_count);
  for (uint16_t y = 0U; y < s_fb.height_px; ++y) {
    const uint32_t row     = ((uint32_t)y + scroll_offset) / (uint32_t)bar_height;
    const uint16_t colour  = s_palette[row % (uint32_t)k_app_bar_count];
    const uint32_t row_off = (uint32_t)y * (uint32_t)s_fb.width_px;
    for (uint16_t x = 0U; x < s_fb.width_px; ++x) {
      pixels[row_off + (uint32_t)x] = colour;
    }
  }
}

void main(void)
{
  app_bringup_clocks();
  ra8_delay_ms(k_app_powerup_ms);
  app_bringup_display();

  /* Pick the refresh hint that matches the bound backend.  Continuous-
   * refresh panels (LCD) prefer "fast" because every flush is just a
   * memory barrier; e-ink picks GC16-like quality only on full
   * repaints to avoid ghosting. */
  const display_refresh_hint_t hint =
    s_caps.continuous_refresh ? k_display_refresh_fast : k_display_refresh_quality;

  /* Clear once with black so the first scroll-in is clean. */
  (void)display_clear(s_display, 0x0000U);
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_init);

  while (1) {
    app_paint_bars(s_scroll_offset);
    (void)display_flush(s_display, display_full_rect(s_display), hint);
    (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    s_scroll_offset = (s_scroll_offset + 1U) % (uint32_t)s_fb.height_px;
    ra8_delay_ms((uint32_t)k_app_frame_period_ms);
  }
}
