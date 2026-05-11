/**
 * @file examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/main.c
 * @brief GLCDC parallel-RGB BG-plane color cycle on the EK-RA8D2 panel
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * First app proven to drive the 1024x600 parallel TFT on the Renesas
 * "Parallel Graphics Expansion Board 1".  The panel cycles solid
 * colors (red -> green -> blue -> white) every 500 ms by re-writing
 * GLCDC `BG_BGC` and pulsing `BG_EN.VEN` to commit the new color on
 * the next vertical sync.  No framebuffer is used -- the BG plane
 * drives the entire panel area on its own.
 *
 * The blue board LED toggles every cycle as a visual heartbeat so
 * the firmware is observable even if the panel stays dark.
 *
 * Bring-up findings discovered while bringing this app up
 * (worth re-reading before adding new GLCDC apps):
 *
 *   1. `ra_pfs_route_peripheral` only sets PSEL and PMR; it leaves
 *      PDR=0 (input).  Parallel-RGB pins must be PDR=1 (output) for
 *      the GLCDC to drive them.  This file works around that with a
 *      manual PFS write that sets PSEL=0x19, PMR=1, PDR=1 directly.
 *
 *   2. The GLCDC output stage composes `BG x GR2 x GR1`.  Even when
 *      only the BG plane is in use, BOTH GR1 and GR2 must be
 *      configured (dimensions, alpha=0, DISPSEL=transparent,
 *      FLMRD=0) and VEN-asserted.  Without GR2 in a sane state, the
 *      pipeline silently emits solid black.  See ra_glcdc.c
 *      `internal_panel_program` for the GR2 init.
 *
 *   3. The Parallel Graphics Expansion Board's BLEN signal (P514) is
 *      active-HIGH.  Driving it low kills the backlight.
 *
 *   4. `BG_BGC` is shadow-registered: a write only takes effect on
 *      the next VS once `BG_EN.VEN=1` is asserted.  Runtime color
 *      changes must pulse VEN, which the driver does internally in
 *      `ra_glcdc_set_background_color`.
 *
 * Pin map source: EK-RA8D2 v1 User Manual Table 33
 *   ("Parallel Graphics Expansion Port Assignments").
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
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sdramc.h"
#include "ra_time.h"

typedef enum : uint16_t {
  k_lcd_panel_w = 1024U,
  k_lcd_panel_h = 600U,
} lcd_panel_dim_t;

typedef enum : uint32_t {
  k_lcd_cycle_ms       = 500U,
  k_lcd_reset_pulse_ms = 50U,
} lcd_pace_t;

#define LCD_PP(port, pin) ((ra_port_pin_t)(((uint16_t)(port) << 8) | (uint16_t)(pin)))

typedef enum : uint16_t {
  k_lcd_pin_reset_l = (uint16_t)LCD_PP(6U, 6U),  /**< P606 RESET_L  */
  k_lcd_pin_blen    = (uint16_t)LCD_PP(5U, 14U), /**< P514 BLEN (active-high) */
} lcd_gpio_pin_t;

typedef enum : uint8_t {
  k_lcd_glcdc_pin_count = 28U,
} lcd_pin_count_t;

/* Per EK-RA8D2 v1 UM Table 33 ("Parallel Graphics Expansion Port"). */
static const ra_port_pin_t k_lcd_glcdc_pins[k_lcd_glcdc_pin_count] = {
  /* DATA0..DATA7 (B0..B7) */
  LCD_PP(9U, 14U),
  LCD_PP(9U, 15U),
  LCD_PP(9U, 3U),
  LCD_PP(9U, 2U),
  LCD_PP(9U, 10U),
  LCD_PP(9U, 11U),
  LCD_PP(9U, 12U),
  LCD_PP(9U, 13U),
  /* DATA8..DATA15 (G0..G7) */
  LCD_PP(9U, 4U),
  LCD_PP(2U, 7U),
  LCD_PP(11U, 7U),
  LCD_PP(11U, 6U),
  LCD_PP(11U, 5U),
  LCD_PP(11U, 1U),
  LCD_PP(11U, 4U),
  LCD_PP(11U, 3U),
  /* DATA16..DATA23 (R0..R7) */
  LCD_PP(11U, 2U),
  LCD_PP(11U, 0U),
  LCD_PP(7U, 7U),
  LCD_PP(7U, 11U),
  LCD_PP(7U, 12U),
  LCD_PP(7U, 13U),
  LCD_PP(7U, 14U),
  LCD_PP(7U, 15U),
  /* TCON / CLK */
  LCD_PP(8U, 6U),
  LCD_PP(8U, 5U),
  LCD_PP(8U, 7U),
  LCD_PP(5U, 15U),
};

/* BG_BGC format: bits[23:16]=R, [15:8]=G, [7:0]=B. */
typedef enum : uint32_t {
  k_bgc_red   = 0x00FF0000U,
  k_bgc_green = 0x0000FF00U,
  k_bgc_blue  = 0x000000FFU,
  k_bgc_white = 0x00FFFFFFU,
} lcd_bgc_t;

typedef enum : uint8_t {
  k_bgc_cycle_count = 4U,
} lcd_bgc_count_t;

static const uint32_t k_lcd_bgc_cycle[k_bgc_cycle_count] = {
  (uint32_t)k_bgc_red,
  (uint32_t)k_bgc_green,
  (uint32_t)k_bgc_blue,
  (uint32_t)k_bgc_white,
};

static void lcd_panic_halt(void)
{
  (void)ra_board_led_on(k_ra_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route all 28 GLCDC pins to PSEL=0x19 with PDR=1.
 *
 * Manual workaround: ra_pfs_route_peripheral only writes PSEL+PMR,
 * leaving PDR=0 (input).  GLCDC needs each pin as an output for the
 * peripheral to drive it.  This function does the route + PDR=1 in a
 * single direct PFS write per pin.
 */
static ra_err_t lcd_glcdc_pins_init(void)
{
  enum : uintptr_t {
    k_pfs_base_addr = 0x40400800UL,
    k_pwpr_addr     = 0x40400D0CUL,
    k_pwprs_addr    = 0x40400D14UL,
  };
  enum : uint8_t {
    k_pwpr_unlock = 0x40U, /* PFSWE=1, B0WI=0 */
    k_pwpr_lock   = 0x80U, /* PFSWE=0, B0WI=1 */
  };
  enum : uint32_t {
    k_pin_stride_bytes = 4U,
    k_port_stride_pins = 16U,
    k_pfs_psel_shift   = 24U,
    k_pfs_pmr_bit      = 1U << 16,
    k_pfs_pdr_bit      = 1U << 2,
  };

  for (uint8_t i = 0U; i < (uint8_t)k_lcd_glcdc_pin_count; i++) {
    const uint16_t           port = (uint16_t)((uint16_t)k_lcd_glcdc_pins[i] >> 8);
    const uint16_t           pin  = (uint16_t)((uint16_t)k_lcd_glcdc_pins[i] & 0xFFU);
    volatile uint32_t* const pfs =
      (volatile uint32_t*)(k_pfs_base_addr +
                           ((uintptr_t)port * k_port_stride_pins + (uintptr_t)pin) *
                             k_pin_stride_bytes);
    volatile uint8_t* const pwpr  = (volatile uint8_t*)k_pwpr_addr;
    volatile uint8_t* const pwprs = (volatile uint8_t*)k_pwprs_addr;

    /* HUM Ch 20.2.5 PWPR unlock sequence. */
    *pwpr  = 0U;
    *pwpr  = k_pwpr_unlock;
    *pwprs = 0U;
    *pwprs = k_pwpr_unlock;
    *pfs   = ((uint32_t)k_ra_psel_glcdc << k_pfs_psel_shift) | k_pfs_pmr_bit | k_pfs_pdr_bit;
    *pwpr  = 0U;
    *pwpr  = k_pwpr_lock;
    *pwprs = 0U;
    *pwprs = k_pwpr_lock;
  }
  return k_ra_ok;
}

/**
 * @brief Pulse the panel's RESET_L line and assert backlight enable.
 */
[[nodiscard]] static ra_err_t lcd_panel_power_on(void)
{
  ra_err_t err = ra_gpio_output_init((ra_port_pin_t)k_lcd_pin_reset_l, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_lcd_reset_pulse_ms);
  err = ra_gpio_write((ra_port_pin_t)k_lcd_pin_reset_l, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_lcd_reset_pulse_ms);
  /* BLEN is ACTIVE-HIGH on EK-RA8D2 v1 (P514). */
  return ra_gpio_output_init((ra_port_pin_t)k_lcd_pin_blen, k_ra_level_high);
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

  /* Stabilization delays: PLLs, SDRAM, and the panel itself all need
   * a few hundred ms after power-on to settle.  Without these, the
   * GLCDC sometimes starts before LCDCLK is stable and the panel
   * comes up in its no-signal "white" state on cold boot. */
  ra_delay_ms(500U);

  /* SDRAM is initialized so the framebuffer region at 0x68000000 is
   * accessible for follow-on apps; this demo doesn't use it. */
  if (ra_sdramc_init() != k_ra_ok) {
    lcd_panic_halt();
  }
  ra_delay_ms(100U);

  if (lcd_panel_power_on() != k_ra_ok) {
    lcd_panic_halt();
  }
  if (lcd_glcdc_pins_init() != k_ra_ok) {
    lcd_panic_halt();
  }
  ra_delay_ms(200U); /* let pins settle in output mode */

  /* GLCDC: framebuffer pointer is irrelevant for this demo (BG plane
   * drives the panel on its own with both graphics layers held
   * invisible by the driver). */
  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = 0x68000000UL,
    .width_px         = (uint16_t)k_lcd_panel_w,
    .height_px        = (uint16_t)k_lcd_panel_h,
    .format           = k_ra_glcdc_fmt_rgb565,
  };
  if (ra_glcdc_init(&cfg) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_glcdc_set_background_color(k_bgc_red) != k_ra_ok) {
    lcd_panic_halt();
  }
  if (ra_glcdc_start(true) != k_ra_ok) {
    lcd_panic_halt();
  }

  uint8_t i = 0U;
  while (1) {
    (void)ra_glcdc_set_background_color(k_lcd_bgc_cycle[i & (k_bgc_cycle_count - 1U)]);
    (void)ra_board_led_toggle(k_ra_board_led_blue);
    ra_delay_ms(k_lcd_cycle_ms);
    i++;
  }
  return 0;
}
#pragma GCC diagnostic pop
