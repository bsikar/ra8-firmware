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
 * is not yet brought up -- ``ra8_sdramc_init`` returns OK but writes
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_time.h"

typedef enum : uint16_t {
  k_fb_w = 512U, /**< Fb w. */
  k_fb_h = 512U, /**< Fb h. */
} lcd_fb_dim_t;

typedef enum : uint16_t {
  k_color_bg = 0x001FU, /**< RGB565 dark blue. */
  k_color_x  = 0xFFE0U, /**< RGB565 yellow.    */
} lcd_color_t;

typedef enum : uint8_t {
  k_lcd_x_thickness = 4U, /**< LCD x thickness. */
} lcd_x_param_t;

typedef enum : uint32_t {
  k_lcd_powerup_delay_ms = 500U, /**< LCD powerup delay ms. */
  k_lcd_heartbeat_ms     = 500U, /**< LCD heartbeat ms.     */
} lcd_pace_t;

/* Static framebuffer in SRAM, 64-byte AXI-burst aligned so the GLCDC
 * fetches are clean.  The display PAL takes a pointer to this and
 * forwards it to the GLCDC HAL via ``display_init``. */
[[gnu::aligned(
  k_ra8_board_fb_align_bytes)]] static uint16_t s_framebuffer[(uint32_t)k_fb_w * (uint32_t)k_fb_h];

/**
 * @brief Display PAL config selecting the LCD backend.
 *
 * @details
 * To target a different backend in the future -- e.g. an
 * IT8951-compatible e-ink panel -- only the ``iface`` line below
 * changes (point it at ``k_display_backend_eink_it8951`` in
 * ``ra8_display_pal_eink.h``).  Everything else in this app
 * (painting, heartbeat, panic loop) stays untouched.
 */
static const display_cfg_t k_lcd_draw_x_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_fb_w,
  .height_px         = (uint16_t)k_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

static void lcd_panic_halt(void)
{
  (void)ra8_board_led_on(k_ra8_board_led_red);
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

/**
 * @brief Bring up clocks, MSTP, system tick and the on-board LEDs.
 *
 * @details
 * First phase of the demo's startup sequence. Any failure halts in the
 * red-LED panic loop -- there is no recovery path for a clock or pin
 * misconfiguration.
 *
 * @pre Reset handler has populated ``.data`` / ``.bss``.
 * @pre Interrupts are still globally disabled.
 * @post Clocks, MSTP, system tick and both board LEDs are initialized.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot startup helper.
 * @since 0.1.0
 */
static void lcd_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    lcd_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lcd_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    lcd_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lcd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    lcd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    lcd_panic_halt();
  }
  ra8_isr_globals_enable();
}

/**
 * @brief Bring up the panel through the display PAL.
 *
 * @details
 * Replaces the previous 6-step GLCDC bring-up sequence with a single
 * ``display_init`` call against ``k_lcd_draw_x_display_cfg``.  The
 * PAL's LCD backend folds panel power-on, GLCDC pin/clock setup,
 * settle delay, controller init, BG-clear, ``start(true)`` and
 * ``layer1_show`` into one call.  Any failure halts in the red-LED
 * panic loop.
 *
 * @pre ::lcd_bringup_clocks has run successfully.
 * @pre The framebuffer has already been painted.
 * @post Display PAL handle is alive; GLCDC is scanning out the FB.
 * @post Panel back-light and 3.3 V rail are on.
 *
 * @note Not thread-safe; single-shot startup helper.
 * @since 0.1.0
 */
static display_handle_t* lcd_bringup_panel(void)
{
  display_handle_t* d = nullptr;
  if (display_init(&k_lcd_draw_x_display_cfg, &d) != k_ra8_ok) {
    lcd_panic_halt();
  }
  return d;
}

void main(void)
{
  lcd_bringup_clocks();
  ra8_delay_ms(k_lcd_powerup_delay_ms);

  lcd_fb_fill(k_color_bg);
  lcd_draw_x(k_color_x);

  (void)lcd_bringup_panel();

  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    ra8_delay_ms(k_lcd_heartbeat_ms);
  }
}
