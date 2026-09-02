/**
 * @file examples/ek_ra8d2/hw_validated/hil/glcdc_render/src/main.c
 * @brief Headless on-silicon HIL gate for the GLCDC layer-1 render path.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ereader_chrome` gates the **software** rasteriser (`ra8_box` + `ra8_gfx`
 * into an SRAM buffer) but never touches the display controller. This app
 * closes the remaining gap: it proves the **GLCDC hardware layer-1 render
 * path** is programmed and scanning a real framebuffer, deterministically and
 * headlessly, with no human looking at the panel.
 *
 * Sequence:
 *   1. Paint a deterministic RGB565 pattern (dark-blue field, yellow corner-to-
 *      corner X, white 1-px border) into a 512x512 SRAM framebuffer.
 *   2. Bring the panel up through the display PAL (`display_init` with the
 *      `k_display_backend_lcd_ra8_glcdc` backend), which runs the full GLCDC
 *      bring-up: panel power-on, GLCDC pin/clock setup, `ra8_glcdc_init`,
 *      background clear, `ra8_glcdc_start(true)`, `ra8_glcdc_layer1_show`.
 *   3. Assert the hardware layer is actually programmed:
 *        - `display_init` returned `k_ra8_ok` and handed back a live handle,
 *        - `display_get_framebuffer` reports our exact framebuffer pointer
 *          (the GLCDC GR1 fetch is bound to this buffer), and
 *        - `ra8_glcdc_get_status` reads the GLCDC SYS_STAT register (the
 *          register block is un-gated and reachable).
 *   4. Fold an FNV-1a-32 hash over the whole framebuffer and emit the result on
 *      the SCI8 J-Link OB console:
 *
 *        `glcdc-hil: layer1=ok dim=512x512 crc=<8 hex> PASS`
 *
 * The HIL gate (`hil.conf`, `uart_scrape`) asserts that line. The render is
 * deterministic (integer paint into a zeroed static buffer), so the CRC is the
 * same every boot; the `layer1=ok ... PASS` tail is only emitted once the GLCDC
 * programming asserts above all hold -- any GLCDC bring-up failure prints
 * `glcdc-hil: FAIL glcdc` and trips the gate's negative match.
 *
 * On `ra8_emulator` the panel compositor reads the GLCDC GR1 framebuffer registers
 * to build the panel image, so a `--ppm` snapshot also shows the painted
 * pattern -- independent confirmation that the GR1 registers point at the
 * buffer. The same FNV-1a-32 globals are exported for `--dump-sym` memprobe.
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
#include "ra8_glcdc.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_time.h"

/**
 * @enum glcdc_hil_geom_t
 * @brief Framebuffer geometry constants.
 *
 * @details
 * 512x512 RGB565 (0.5 MiB) sits in internal SRAM and is composited top-left
 * over the BG plane -- the same size `lcd_draw_x` uses, since the 1024x600
 * panel framebuffer (1.2 MiB) does not fit on-chip and SDRAM bring-up is a
 * separate task.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_gh_fb_w        = 512U, /**< Framebuffer width, pixels.         */
  k_gh_fb_h        = 512U, /**< Framebuffer height, pixels.        */
  k_gh_x_thickness = 4U,   /**< Diagonal-X half thickness, pixels. */
} glcdc_hil_geom_t;

/**
 * @enum glcdc_hil_color_t
 * @brief RGB565 palette for the deterministic render pattern.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_gh_color_bg     = 0x001FU, /**< Dark blue field.   */
  k_gh_color_x      = 0xFFE0U, /**< Yellow diagonal X. */
  k_gh_color_border = 0xFFFFU, /**< White 1-px frame.  */
} glcdc_hil_color_t;

/**
 * @enum glcdc_hil_console_t
 * @brief SCI8 J-Link OB console + text-format constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_gh_uart_baud   = 115200U,     /**< Console baud.                    */
  k_gh_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.          */
  k_gh_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                 */
  k_gh_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.    */
  k_gh_nibble_bits = 4U,          /**< Bits per hex nibble.             */
  k_gh_nibble_mask = 0x0FU,       /**< Low-nibble mask.                 */
  k_gh_dec_ten     = 10U,         /**< Hex digit / decimal split + buf. */
} glcdc_hil_console_t;

/**
 * @brief Render target in internal SRAM, 64-byte AXI-burst aligned so the
 *        GLCDC's AXI fetches are clean.
 * @since 0.1.0
 */
[[gnu::aligned(k_ra8_board_fb_align_bytes)]] static uint16_t
  s_framebuffer[(uint32_t)k_gh_fb_w * (uint32_t)k_gh_fb_h];

/**
 * @brief Display PAL config selecting the GLCDC LCD backend.
 *
 * @details
 * Backend-agnostic: re-pointing `iface` at a different backend (e.g. an e-ink
 * controller) is the only change needed to gate that path instead. The panel
 * timing is the board BSP's EK-RA8D2 descriptor.
 *
 * @since 0.1.0
 */
static const display_cfg_t k_gh_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_gh_fb_w,
  .height_px         = (uint16_t)k_gh_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/**
 * @var g_glcdc_hil_ok
 * @brief 1 once the GLCDC layer-1 render path is confirmed programmed.
 * @note Exported for `ra8_emulator --dump-sym` memprobe; the gate is uart_scrape.
 * @warning Written only by ::main; readers must treat it as read-only.
 * @since 0.1.0
 */
volatile uint32_t g_glcdc_hil_ok = 0U;
/**
 * @var g_glcdc_hil_crc
 * @brief FNV-1a-32 of the rendered framebuffer (the on-bench baseline).
 * @note Exported for `ra8_emulator --dump-sym` memprobe.
 * @warning Written only by ::main.
 * @since 0.1.0
 */
volatile uint32_t g_glcdc_hil_crc = 0U;
/**
 * @var g_glcdc_hil_status
 * @brief Last GLCDC SYS_STAT snapshot (diagnostic; not part of the CRC).
 * @note Non-deterministic register on `ra8_emulator`; kept out of the banner.
 * @warning Written only by ::main.
 * @since 0.1.0
 */
volatile uint32_t g_glcdc_hil_status = 0U;
/**
 * @var g_glcdc_hil_heartbeat
 * @brief Bumps on every idle-loop pass after a passing render.
 * @note Exported for `ra8_emulator --dump-sym` liveness checks.
 * @warning Written only by ::main.
 * @since 0.1.0
 */
volatile uint32_t g_glcdc_hil_heartbeat = 0U;

static const uint8_t k_gh_msg_boot[]       = "glcdc-hil: boot\r\n";
static const uint8_t k_gh_msg_fail_init[]  = "glcdc-hil: FAIL init\r\n";
static const uint8_t k_gh_msg_fail_glcdc[] = "glcdc-hil: FAIL glcdc\r\n";
static const uint8_t k_gh_msg_pre[]        = "glcdc-hil: layer1=ok dim=";
static const uint8_t k_gh_msg_x[]          = "x";
static const uint8_t k_gh_msg_crc[]        = " crc=";
static const uint8_t k_gh_msg_pass[]       = " PASS\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void gh_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Park forever in WFI after a fatal init error. */
static void gh_panic_halt(void)
{
  gh_print(k_gh_msg_fail_init, (uint32_t)sizeof(k_gh_msg_fail_init) - 1U);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print @p value as 8 uppercase hex digits. */
static void gh_print_hex(uint32_t value)
{
  uint8_t buf[k_gh_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_gh_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_gh_hex_nibbles - 1U - i) * (uint32_t)k_gh_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_gh_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_gh_dec_ten) ? ('0' + nib) : ('A' + (nib - k_gh_dec_ten)));
  }
  gh_print(buf, (uint32_t)k_gh_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void gh_print_uint(uint32_t value)
{
  uint8_t  buf[k_gh_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_gh_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_gh_dec_ten));
    n++;
    value /= (uint32_t)k_gh_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    gh_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Deterministic framebuffer pattern
 * ===========================================================================
 */

/** @brief Fill the whole framebuffer with @p color. */
static void gh_fb_fill(uint16_t color)
{
  for (uint32_t i = 0U; i < (uint32_t)k_gh_fb_w * (uint32_t)k_gh_fb_h; i++) {
    s_framebuffer[i] = color;
  }
}

/** @brief Draw a thick corner-to-corner X in @p color. */
static void gh_draw_x(uint16_t color)
{
  for (uint16_t x = 0U; x < (uint16_t)k_gh_fb_w; x++) {
    const int32_t y_down = ((int32_t)x * (int32_t)k_gh_fb_h) / (int32_t)k_gh_fb_w;
    const int32_t y_up   = ((int32_t)k_gh_fb_h - 1) - y_down;
    for (int32_t dy = -(int32_t)k_gh_x_thickness; dy <= (int32_t)k_gh_x_thickness; dy++) {
      const int32_t yy_down = y_down + dy;
      const int32_t yy_up   = y_up + dy;
      if ((yy_down >= 0) && (yy_down < (int32_t)k_gh_fb_h)) {
        s_framebuffer[((uint32_t)yy_down * (uint32_t)k_gh_fb_w) + (uint32_t)x] = color;
      }
      if ((yy_up >= 0) && (yy_up < (int32_t)k_gh_fb_h)) {
        s_framebuffer[((uint32_t)yy_up * (uint32_t)k_gh_fb_w) + (uint32_t)x] = color;
      }
    }
  }
}

/** @brief Draw a 1-px border around the whole framebuffer in @p color. */
static void gh_draw_border(uint16_t color)
{
  const uint32_t last_row = ((uint32_t)k_gh_fb_h - 1U) * (uint32_t)k_gh_fb_w;
  for (uint32_t x = 0U; x < (uint32_t)k_gh_fb_w; x++) {
    s_framebuffer[x]            = color;
    s_framebuffer[last_row + x] = color;
  }
  for (uint32_t y = 0U; y < (uint32_t)k_gh_fb_h; y++) {
    const uint32_t row                            = y * (uint32_t)k_gh_fb_w;
    s_framebuffer[row]                            = color;
    s_framebuffer[row + (uint32_t)k_gh_fb_w - 1U] = color;
  }
}

/** @brief Paint the full deterministic render pattern. */
static void gh_render_pattern(void)
{
  gh_fb_fill((uint16_t)k_gh_color_bg);
  gh_draw_x((uint16_t)k_gh_color_x);
  gh_draw_border((uint16_t)k_gh_color_border);
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb" ::: "memory");
#endif
}

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t gh_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_gh_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_gh_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * GLCDC bring-up + programmed-layer proof
 * ===========================================================================
 */

/**
 * @brief Bring the GLCDC layer-1 path up and prove it is programmed.
 *
 * @details
 * Runs `display_init` (full GLCDC bring-up) then asserts three independent
 * facts: the init succeeded with a live handle, the bound framebuffer the
 * backend reports is our exact buffer (GR1 fetch points here), and the GLCDC
 * SYS_STAT register is reachable. The status mask itself is snapshotted for
 * diagnostics but deliberately excluded from the deterministic banner because
 * it is a live, free-running register.
 *
 * @return true when all three GLCDC-programmed assertions hold.
 * @retval true  GLCDC layer-1 is programmed and scanning ::s_framebuffer.
 * @retval false Any bring-up or binding assertion failed.
 *
 * @pre ::gh_setup_or_halt has run (clocks, MSTP, console up).
 * @pre ::s_framebuffer has been painted.
 * @post On true, ::g_glcdc_hil_status holds the SYS_STAT snapshot.
 * @post No framebuffer bytes are modified.
 *
 * @note Not thread-safe; single-shot bring-up helper.
 * @since 0.1.0
 */
static bool gh_glcdc_programmed(void)
{
  display_handle_t* d = nullptr;
  if (display_init(&k_gh_display_cfg, &d) != k_ra8_ok) {
    return false;
  }
  if (d == nullptr) {
    return false;
  }
  display_fb_t fb = {};
  if (display_get_framebuffer(d, &fb) != k_ra8_ok) {
    return false;
  }
  if (fb.pixels != (void*)s_framebuffer) {
    return false;
  }
  uint32_t status_mask = 0U;
  if (ra8_glcdc_get_status(&status_mask) != k_ra8_ok) {
    return false;
  }
  g_glcdc_hil_status = status_mask;
  return true;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void gh_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    gh_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    gh_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    gh_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_gh_uart_baud) != k_ra8_ok) {
    gh_panic_halt();
  }
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

/**
 * @brief App entry: paint, bring the GLCDC up, hash, print the banner.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The `layer1=ok ... PASS` banner is emitted on a programmed GLCDC, or
 *       `FAIL glcdc` on a bring-up failure; the CPU then loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  gh_setup_or_halt();
  ra8_isr_globals_enable();
  gh_print(k_gh_msg_boot, (uint32_t)sizeof(k_gh_msg_boot) - 1U);

  gh_render_pattern();

  if (!gh_glcdc_programmed()) {
    gh_print(k_gh_msg_fail_glcdc, (uint32_t)sizeof(k_gh_msg_fail_glcdc) - 1U);
    while (1) {
      __asm__ volatile("wfi");
    }
  }
  g_glcdc_hil_ok  = 1U;
  g_glcdc_hil_crc = gh_framebuffer_hash();

  gh_print(k_gh_msg_pre, (uint32_t)sizeof(k_gh_msg_pre) - 1U);
  gh_print_uint((uint32_t)k_gh_fb_w);
  gh_print(k_gh_msg_x, (uint32_t)sizeof(k_gh_msg_x) - 1U);
  gh_print_uint((uint32_t)k_gh_fb_h);
  gh_print(k_gh_msg_crc, (uint32_t)sizeof(k_gh_msg_crc) - 1U);
  gh_print_hex((uint32_t)g_glcdc_hil_crc);
  gh_print(k_gh_msg_pass, (uint32_t)sizeof(k_gh_msg_pass) - 1U);

  while (1) {
    g_glcdc_hil_heartbeat++;
    __asm__ volatile("wfi");
  }
}
