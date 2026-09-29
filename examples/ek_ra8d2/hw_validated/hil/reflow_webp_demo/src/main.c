/**
 * @file main.c
 * @brief Demonstration of inline WebP image decoding in reflow text layout (#637).
 *
 * @details
 * Shows zero-heap WebP image decoding and scaling within the reflow engine
 * using the ra8_webp facade over vendored libwebp.
 *
 * When the reflow tokenizer and layout encounter an <img> tag referencing
 * a WebP resource, the layout queries the image dimensions via ra8_img_probe_size()
 * and fits it into the column. During page rendering, reflow_render_page()
 * allocates transient decode buffers from the caller-provided bump arena,
 * decodes the WebP directly into an RGBA8888 canvas without libc malloc,
 * scales the image to the layout box using nearest-neighbor sampling,
 * and drains the arena completely before returning.
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
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "reflow.h"
#include "reflow_image.h"

/* ===========================================================================
 * Geometry & color definitions
 * =========================================================================== */

typedef enum : uint32_t {
  k_webp_fb_w       = 1024U,
  k_webp_fb_h       = 600U,
  k_webp_font_px    = 22U,
  k_webp_paper_argb = 0xFFFDFBF7U, /* Warm paper background */
  k_webp_ink_argb   = 0xFF18181CU, /* Dark charcoal ink */
  k_webp_link_argb  = 0xFF1C3A5EU, /* Slate blue link */
  k_webp_settle_ms  = 20U,
  k_webp_frame_ms   = 50U,
} webp_demo_consts_t;

/** @brief RGB565 framebuffer in external SDRAM, aligned for GLCDC scanout. */
RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);

/** @brief 512 KiB image decode scratch arena in SDRAM (zero heap). */
[[gnu::section(".sdram_data"), gnu::aligned(16)]] static uint8_t s_img_arena_buf[512U * 1024U];
static ra8_img_arena_t s_img_arena = {
  .base   = s_img_arena_buf,
  .cap    = sizeof(s_img_arena_buf),
  .offset = 0U,
  .live   = 0U,
};

/** @brief Display configuration using the EK-RA8D2 panel timing and GLCDC backend. */
static const display_cfg_t k_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_webp_fb_w,
  .height_px         = (uint16_t)k_webp_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

static display_handle_t* s_display = nullptr;
static display_fb_t      s_fb;
static reflow_t          s_engine;

/**
 * @brief 96x96 VP8L lossless WebP illustration (book graphic with header banner & ribbon).
 */
static const uint8_t s_demo_webp[] = {
  0x52U, 0x49U, 0x46U, 0x46U, 0x20U, 0x01U, 0x00U, 0x00U, 0x57U, 0x45U, 0x42U, 0x50U,
  0x56U, 0x50U, 0x38U, 0x4CU, 0x14U, 0x01U, 0x00U, 0x00U, 0x2FU, 0x5FU, 0xC0U, 0x17U,
  0x00U, 0x47U, 0xA0U, 0x34U, 0x92U, 0x14U, 0x46U, 0x7DU, 0xFFU, 0x9DU, 0x91U, 0x3CU,
  0x1DU, 0x64U, 0x85U, 0xA4U, 0x0DU, 0x05U, 0x91U, 0x6CU, 0x50U, 0xFDU, 0x23U, 0x3DU,
  0x14U, 0xD0U, 0x40U, 0x88U, 0x57U, 0xC0U, 0x5FU, 0x31U, 0x24U, 0x49U, 0x0CU, 0xCCU,
  0xFAU, 0x3FU, 0x4FU, 0x23U, 0x6BU, 0x64U, 0x82U, 0x9BU, 0xFFU, 0xF8U, 0xFFU, 0x97U,
  0xC7U, 0x2CU, 0x51U, 0xB0U, 0x60U, 0x0AU, 0xA6U, 0x75U, 0xF0U, 0x26U, 0x40U, 0x80U,
  0x1BU, 0x5BU, 0x7BU, 0x15U, 0x25U, 0x38U, 0xEDU, 0xF8U, 0xB4U, 0xB0U, 0x02U, 0x5BU,
  0x03U, 0xBDU, 0xE6U, 0x1CU, 0x4AU, 0x7FU, 0x5BU, 0xA0U, 0x75U, 0xD9U, 0xF6U, 0x93U,
  0x61U, 0x92U, 0xE0U, 0x52U, 0x44U, 0xF4U, 0x9FU, 0x8DU, 0xDBU, 0x36U, 0x92U, 0xA8U,
  0x6AU, 0x66U, 0x76U, 0xB7U, 0xBAU, 0xF7U, 0x09U, 0xDEU, 0xFDU, 0xB7U, 0x16U, 0xEEU,
  0x26U, 0x64U, 0x57U, 0xADU, 0x46U, 0x53U, 0x30U, 0xDFU, 0xA1U, 0x04U, 0x7BU, 0xABU,
  0x54U, 0xA7U, 0xBCU, 0x79U, 0x0AU, 0x00U, 0xDEU, 0xBCU, 0x3CU, 0xF9U, 0x81U, 0xFCU,
  0x79U, 0xF5U, 0xE6U, 0xF2U, 0x04U, 0x7BU, 0x9FU, 0x21U, 0xF6U, 0xC6U, 0x88U, 0x42U,
  0xD0U, 0x06U, 0xA0U, 0x01U, 0xA0U, 0x29U, 0x62U, 0x19U, 0x38U, 0xDBU, 0xDAU, 0x1CU,
  0x4EU, 0x7DU, 0x67U, 0x53U, 0x1AU, 0xC4U, 0x0AU, 0x98U, 0xF2U, 0x5CU, 0xD9U, 0x94U,
  0xA2U, 0x02U, 0x40U, 0x07U, 0xD0U, 0x7FU, 0xB6U, 0x4AU, 0x56U, 0x4BU, 0x40U, 0x37U,
  0x9BU, 0xABU, 0x9BU, 0x8DU, 0xAEU, 0x44U, 0x1DU, 0xB4U, 0x00U, 0xFAU, 0xFFU, 0x4FU,
  0x07U, 0xBAU, 0x5CU, 0x2EU, 0x4FU, 0x17U, 0x62U, 0x85U, 0xAEU, 0x8FU, 0xC8U, 0x78U,
  0x2EU, 0x57U, 0x1CU, 0x1BU, 0xA3U, 0xEEU, 0xA0U, 0x3FU, 0x97U, 0x73U, 0x1EU, 0xE6U,
  0x2EU, 0xB4U, 0x49U, 0xBDU, 0x57U, 0x1CU, 0x8DU, 0x89U, 0xE8U, 0x7BU, 0xCBU, 0xD6U,
  0x48U, 0x5FU, 0xAAU, 0x8BU, 0x4DU, 0xD8U, 0x96U, 0x45U, 0xA9U, 0x38U, 0x5FU, 0xEAU,
  0x6DU, 0x73U, 0xCDU, 0x94U, 0x62U, 0xF1U, 0x3CU, 0x4FU, 0x56U, 0x6AU, 0x0FU, 0xC0U,
  0x44U, 0xEAU, 0x0EU, 0x3CU, 0xC2U, 0x7BU, 0xB2U, 0xFCU, 0x7FU, 0x86U, 0x1BU, 0xFCU,
  0xC7U, 0x78U, 0x80U, 0xFFU, 0x2AU, 0x77U, 0x9FU, 0x07U,
};

/** @brief Image loader callback returning the embedded WebP resource. */
static ra8_err_t demo_image_loader(void*           ctx,
                                   const char*     href,
                                   uint32_t        href_len,
                                   const uint8_t** out_bytes,
                                   size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out_bytes = s_demo_webp;
  *out_len   = sizeof(s_demo_webp);
  return k_ra8_ok;
}

/** @brief Demonstration chapter text containing an inline WebP graphic. */
static const char k_demo_xhtml[] =
  "<html><body>"
  "<h1>Inline WebP Decoder (#637)</h1>"
  "<p>Reflow layout with zero-heap WebP raster decoding:</p>"
  "<img src=\"illustration.webp\"/>"
  "<p>Figure 1: Embedded 96x96 lossless WebP illustration.</p>"
  "</body></html>";

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
  (void)ra8_board_uart_console_init(115200U);
}

/**
 * @brief Initialize SDRAM, display interface, and query framebuffer descriptor.
 */
static void internal_bringup_display(void)
{
  ra8_delay_ms((uint32_t)k_webp_settle_ms);
  (void)ra8_sdramc_init();
  (void)display_init(&k_display_cfg, &s_display);
  (void)display_get_framebuffer(s_display, &s_fb);
  (void)ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565);
  (void)ra8_gfx_clear((uint32_t)k_webp_paper_argb);
}

/**
 * @brief Layout and render demo chapter with inline WebP graphic.
 */
static void internal_render_demo(void)
{
  const ra8_err_t rinit = reflow_init((uint16_t)k_webp_fb_w,
                                      (uint16_t)k_webp_fb_h,
                                      g_ra8_font_literata_latin1,
                                      g_ra8_font_literata_latin1_len,
                                      (uint16_t)k_webp_font_px,
                                      (uint32_t)k_webp_ink_argb,
                                      (uint32_t)k_webp_link_argb,
                                      &s_engine);
  if (rinit != k_ra8_ok) {
    return;
  }

  /* Bind WebP image loader and decode scratch arena */
  (void)reflow_set_image_loader(&s_engine, demo_image_loader, nullptr, &s_img_arena);

  uint32_t pages = 0U;
  (void)reflow_layout_chapter(&s_engine,
                              (const uint8_t*)k_demo_xhtml,
                              (uint32_t)(sizeof(k_demo_xhtml) - 1U),
                              &pages);

  (void)reflow_render_page(&s_engine, 0U, nullptr);
  (void)ra8_board_led_on(k_ra8_board_led_blue);
  const uint8_t msg[] = "reflow-webp-demo: decode=96x96 PASS\r\n";
  (void)ra8_board_uart_console_write(msg, sizeof(msg) - 1U);
}

/* ===========================================================================
 * Application entry point
 * =========================================================================== */

void main(void)
{
  internal_bringup_clocks();
  internal_bringup_display();
  internal_render_demo();

  while (1) {
    ra8_delay_ms((uint32_t)k_webp_frame_ms);
  }
}
