/**
 * @file examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/src/main.c
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
 * Bring-up findings surfaced while landing this app (now handled in
 * the BSP / driver -- documented here so future GLCDC apps can rely
 * on the abstractions instead of repeating the work):
 *
 *   1. `ra8_pfs_route_peripheral` only sets PSEL + PMR; it leaves
 *      PDR=0 (input).  GLCDC outputs need PDR=1.  Handled by
 *      `ra8_board_glcdc_init` in `libs/ra8_board_ek_ra8d2`.
 *
 *   2. The GLCDC output stage composes `BG x GR2 x GR1`.  Both GR1
 *      and GR2 must be configured + VEN-asserted even when only the
 *      BG plane is in use.  Handled by `ra8_glcdc_init` /
 *      `ra8_glcdc_start` in `libs/ra8_hal/src/ra8_glcdc.c`.
 *
 *   3. The Parallel Graphics Expansion Board's BLEN signal (P514)
 *      is active-HIGH.  Exposed as `k_ra8_board_lcd_blen` in the BSP.
 *
 *   4. `BG_BGC` is shadow-registered: writes only take effect at
 *      the next VS once `BG_EN.VEN=1` is asserted.
 *      `ra8_glcdc_set_background_color` pulses VEN internally.
 *
 * Pin map source (BSP): EK-RA8D2 v1 User Manual Table 33
 *   ("Parallel Graphics Expansion Port Assignments").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"

typedef enum : uint16_t {
  k_lcd_panel_w = 1024U, /**< LCD panel w. */
  k_lcd_panel_h = 600U,  /**< LCD panel h. */
} lcd_panel_dim_t;

typedef enum : uint32_t {
  k_lcd_cycle_ms        = 500U, /**< Per-color dwell time in the cycle loop. */
  k_lcd_powerup_ms      = 500U, /**< PLL / SDRAM / panel power-on settle.    */
  k_lcd_sdram_settle_ms = 100U, /**< Post-SDRAM-init settle.                 */
  k_lcd_pin_settle_ms   = 200U, /**< Let pins settle in output mode.         */
} lcd_pace_t;

/* BG_BGC format: bits[23:16]=R, [15:8]=G, [7:0]=B; bits[31:24] reserved. */
typedef enum : uint32_t {
  k_bgc_red   = 0xFF0000U, /**< Bgc red.   */
  k_bgc_green = 0x00FF00U, /**< Bgc green. */
  k_bgc_blue  = 0x0000FFU, /**< Bgc blue.  */
  k_bgc_white = 0xFFFFFFU, /**< Bgc white. */
} lcd_bgc_t;

typedef enum : uint8_t {
  k_bgc_cycle_count = 4U, /**< Bgc cycle count. */
} lcd_bgc_count_t;

/** @brief Ordered GLCDC background colors emitted by the demo loop. */
static const uint32_t s_lcd_bgc_cycle[k_bgc_cycle_count] = {
  (uint32_t)k_bgc_red,
  (uint32_t)k_bgc_green,
  (uint32_t)k_bgc_blue,
  (uint32_t)k_bgc_white,
};

/**
 * @brief Illuminate the red LED and park after an unrecoverable LCD failure.
 *
 * @details Requests the board panic indicator, then remains in a permanent
 *          wait-for-interrupt loop so an attached debugger can inspect the
 *          failed display bring-up state.
 *
 * @return None.
 *
 * @pre Board-level LED access is safe from the current boot context.
 * @pre The caller has no remaining recovery action to perform.
 * @post The red board LED is requested on before the processor parks.
 * @post The function never returns to its caller.
 *
 * @note The LED request is best-effort because this is already a fatal path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lcd_panic_halt(void)
{
  (void)ra8_board_led_on(k_ra8_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, system tick and the on-board LEDs.
 *
 * @details
 * First phase of the demo's startup sequence. Any failure halts in the
 * red-LED panic loop -- there is no recovery path for a clock or pin
 * misconfiguration.
 *
 * @return CPUCLK0 frequency in Hz; the caller passes it on to
 *         ::ra8_time_init for tick generation.
 * @retval nonzero The initialized CPUCLK0 frequency; failures do not return.
 *
 * @pre Reset handler has populated ``.data`` / ``.bss``.
 * @pre Interrupts are still globally disabled.
 * @post Clocks, MSTP, system tick and both board LEDs are initialized.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot startup helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_lcd_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  ra8_isr_globals_enable();
  return cpuclk0_hz;
}

/**
 * @brief Bring up SDRAM, panel power, GLCDC and prime the BG plane.
 *
 * @details
 * Second phase of the demo's startup sequence. Mirrors the original
 * inline sequence exactly: 500 ms PLL/panel settle, SDRAM init for
 * follow-on apps, panel power-on, GLCDC pin/clock setup, then the
 * GLCDC controller itself with an initial red background. Any failure
 * halts in the red-LED panic loop.
 *
 * @pre ::internal_lcd_bringup_clocks has run successfully.
 * @pre Interrupts are globally enabled.
 * @post GLCDC is running and driving the panel with the initial colour.
 * @post Panel back-light and 3.3 V rail are on.
 *
 * @return None.
 *
 * @note Not thread-safe; single-shot startup helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lcd_bringup_panel(void)
{
  /* Stabilization delays: PLLs, SDRAM, and the panel itself all need
   * a few hundred ms after power-on to settle.  Without these, the
   * GLCDC sometimes starts before LCDCLK is stable and the panel
   * comes up in its no-signal "white" state on cold boot. */
  ra8_delay_ms(k_lcd_powerup_ms);

  /* SDRAM is initialized so the framebuffer region at 0x68000000 is
   * accessible for follow-on apps; this demo doesn't use it. */
  if (ra8_sdramc_init() != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  ra8_delay_ms(k_lcd_sdram_settle_ms);

  if (ra8_board_lcd_panel_power_on() != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_board_glcdc_init(k_ra8_board_glcdc_fmt_rgb888) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  ra8_delay_ms(k_lcd_pin_settle_ms); /* let pins settle in output mode */

  /* GLCDC: BG plane drives the panel on its own with both graphics
   * layers held invisible by the driver, so the framebuffer pointer
   * is never dereferenced -- leave it null. */
  const ra8_glcdc_config_t cfg = {
    .framebuffer_addr = 0UL,
    .width_px         = (uint16_t)k_lcd_panel_w,
    .height_px        = (uint16_t)k_lcd_panel_h,
    .format           = k_ra8_glcdc_fmt_rgb565,
    .timing           = s_ra8_panel_ek_ra8d2_timing,
  };
  if (ra8_glcdc_init(&cfg) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_glcdc_set_background_color(k_bgc_red) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
  if (ra8_glcdc_start(true) != k_ra8_ok) {
    internal_lcd_panic_halt();
  }
}

void main(void)
{
  (void)internal_lcd_bringup_clocks();
  internal_lcd_bringup_panel();

  uint8_t i = 0U;
  while (1) {
    (void)ra8_glcdc_set_background_color(s_lcd_bgc_cycle[i & (k_bgc_cycle_count - 1U)]);
    (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    ra8_delay_ms(k_lcd_cycle_ms);
    i++;
  }
}
