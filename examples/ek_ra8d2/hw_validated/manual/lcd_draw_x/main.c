/**
 * @file examples/ek_ra8d2/hw_validated/manual/lcd_draw_x/main.c
 * @brief Draw a coloured X to a SRAM-backed GR1 layer on the EK-RA8D2 panel
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Fills a 512 x 512 RGB565 framebuffer in SRAM with a dark-blue
 * background and a thick yellow X drawn corner-to-corner, then
 * enables GLCDC graphics layer 1 to composite the framebuffer over
 * the BG plane.  The framebuffer is anchored at the panel's
 * top-left of the active area; the rest of the 1024 x 600 panel is
 * filled by the BG plane (BG_BGC = black).  The blue board LED
 * toggles every 500 ms as a heartbeat.
 *
 * Why SRAM and not SDRAM: the on-board 64 MiB SDRAM at 0x68000000
 * is not yet brought up -- ``ra_sdramc_init`` returns OK but writes
 * to the SDRAM region are silently dropped (pin routing + BSC
 * bring-up are TODO).  A 1024 x 600 RGB565 framebuffer (1.2 MiB)
 * doesn't fit in the 1 MiB on-chip SRAM, so this demo uses a
 * smaller 512 x 512 (0.5 MiB) layer in SRAM and lets the BG plane
 * paint the rest.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_time.h"

typedef enum : uint16_t {
  k_fb_w = 512U,
  k_fb_h = 512U,
} lcd_fb_dim_t;

typedef enum : uint16_t {
  k_color_bg = 0x001FU, /**< RGB565 dark blue. */
  k_color_x  = 0xFFE0U, /**< RGB565 yellow.    */
} lcd_color_t;

typedef enum : uint8_t {
  k_lcd_x_thickness = 4U,
} lcd_x_param_t;

typedef enum : uint32_t {
  k_lcd_heartbeat_ms = 500U,
} lcd_pace_t;

/* Static framebuffer in SRAM, 64-byte AXI-burst aligned so the GLCDC
 * fetches are clean. */
static uint16_t s_framebuffer[(uint32_t)k_fb_w * (uint32_t)k_fb_h] __attribute__((aligned(64)));

static void lcd_panic_halt(void)
{
  (void)ra_board_led_on(k_ra_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void lcd_fb_fill(uint16_t color)
{
  for (uint32_t i = 0U; i < (uint32_t)k_fb_w * (uint32_t)k_fb_h; i++) {
    s_framebuffer[i] = color;
  }
  __asm__ volatile("dsb" ::: "memory");
}

static void lcd_draw_x(uint16_t color)
{
  for (uint16_t x = 0U; x < (uint16_t)k_fb_w; x++) {
    const int32_t y_down = ((int32_t)x * (int32_t)k_fb_h) / (int32_t)k_fb_w;
    const int32_t y_up   = ((int32_t)k_fb_h - 1) - y_down;
    for (int32_t dy = -(int32_t)k_lcd_x_thickness; dy <= (int32_t)k_lcd_x_thickness; dy++) {
      const int32_t yy_down = y_down + dy;
      const int32_t yy_up   = y_up + dy;
      if ((yy_down >= 0) && (yy_down < (int32_t)k_fb_h)) {
        s_framebuffer[((uint32_t)yy_down * (uint32_t)k_fb_w) + (uint32_t)x] = color;
      }
      if ((yy_up >= 0) && (yy_up < (int32_t)k_fb_h)) {
        s_framebuffer[((uint32_t)yy_up * (uint32_t)k_fb_w) + (uint32_t)x] = color;
      }
    }
  }
  __asm__ volatile("dsb" ::: "memory");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_blue) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_red) != k_ra_ok) {
    lcd_panic_halt();
  }
  ra_isr_globals_enable();

  ra_delay_ms(500U);

  lcd_fb_fill(k_color_bg);
  lcd_draw_x(k_color_x);

  if (ra_board_lcd_panel_power_on() != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb888) != k_ra_ok) {
    lcd_panic_halt();
  }
  ra_delay_ms(200U);

  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)(uintptr_t)s_framebuffer,
    .width_px         = (uint16_t)k_fb_w,
    .height_px        = (uint16_t)k_fb_h,
    .format           = k_ra_glcdc_fmt_rgb565,
  };
  if (ra_glcdc_init(&cfg) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_glcdc_set_background_color(0x000000U) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_glcdc_start(true) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_glcdc_layer1_show((uintptr_t)s_framebuffer) != k_ra_ok) {
    lcd_panic_halt();
  }

  while (1) {
    (void)ra_board_led_toggle(k_ra_board_led_blue);
    ra_delay_ms(k_lcd_heartbeat_ms);
  }
  return 0;
}
#pragma GCC diagnostic pop
