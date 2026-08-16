/**
 * @file examples/ek_ra8d2/hw_pending/ereader_zoom/main.c
 * @brief Viewable tap-to-zoom image viewer: full-screen zoom + panel loupe (#478).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * The other half of the decision not to downscale `.rabook` images: keep every
 * pixel at import, and let the reader magnify into them. This app makes that
 * viewable. It brings up the 1024x600 GLCDC panel, binds a 4096x3072 gray8 page
 * -- 12 MiB, an order of magnitude past what the part can hold -- through an
 * ::ra8_tile_cache, and drives it with the `ra8_zoom` viewport engine:
 *
 *   1. Boot clocks / MSTP / SysTick / LEDs, then SDRAM and the GLCDC panel
 *      (``ra8_display_pal`` with ``k_display_backend_lcd_ra8_glcdc``), then bind
 *      ``ra8_gfx`` to the RGB565 framebuffer in SDRAM.
 *   2. Wire the tile cache (decode-on-miss = the procedural page sampler) and
 *      open two viewports over it: the full content area, and a 320x320 loupe.
 *   3. Run the scripted boot self-check -- 1:1, one pan step, 2x about a fixed
 *      panel point, then the 4x loupe -- printing a framebuffer hash for each
 *      plus the cache's own residency counters.
 *   4. Main loop: poll the GT911, route the tap to a zone, re-render, and flush
 *      *only what changed* with the waveform ``ra8_zoom`` asks for -- A2 during
 *      an interactive burst, GC16 once the gesture settles.
 *
 * The banner is deterministic because every stage is integer arithmetic (the
 * sampler is shifts and masks, the composite is nearest-neighbour plus a
 * `const` blue-noise mask, and the chrome is filled rectangles rather than
 * antialiased glyphs), so it is identical on host, ra8_emulator and silicon:
 *
 *   ``ereader-zoom: page 4096x3072 tile 256 cells 35 z1=<8hex> pan=<8hex>
 *   z2=<8hex> lens=<8hex> hit=<n> miss=<n> evict=<n> warm=<n> ok``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ez_scene.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "ra8_touch.h"

/**
 * @enum ez_boot_t
 * @brief Boot, console and touch knobs.
 * @details Copied from the sibling reader demos so every viewable app on this
 *          board brings the console and the GT911 up the same way.
 * @invariant k_ez_frame_ms is short enough that a tap is never missed.
 * @par Example:
 * @code
 * (void)ra8_board_uart_console_init((uint32_t)k_ez_uart_baud);
 * @endcode
 * @see main
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_uart_baud      = 115200U,   /**< Console baud.                     */
  k_ez_settle_ms      = 500U,      /**< PLL / SDRAM / panel-POR settle.   */
  k_ez_frame_ms       = 25U,       /**< Input poll period, ms.            */
  k_ez_led_every      = 16U,       /**< Heartbeat LED toggle sub-cadence. */
  k_ez_touch_bus_hz   = 400000U,   /**< GT911 fast-mode I2C clock.        */
  k_ez_touch_pclka_hz = 60000000U, /**< IIC_B clock-source rate.          */
  k_ez_touch_addr_7b  = 0x5DU,     /**< GT911 default 7-bit address.      */
} ez_boot_t;

/**
 * @enum ez_fb_t
 * @brief Framebuffer geometry (mirrors the EK-RA8D2 panel).
 * @details Held separately from the panel constants so the scene's layout
 *          asserts have compile-time numbers to check against.
 * @invariant k_ez_fb_align is a multiple of the AXI burst length.
 * @par Example:
 * @code
 * static uint16_t s_framebuffer[(size_t)k_ez_fb_h * (size_t)k_ez_fb_w];
 * @endcode
 * @see main
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ez_fb_w     = k_panel_width_px,  /**< Framebuffer width, pixels.   */
  k_ez_fb_h     = k_panel_height_px, /**< Framebuffer height, pixels.  */
  k_ez_fb_align = 64U,               /**< 64-byte AXI-burst alignment. */
} ez_fb_t;

/**
 * @enum ez_print_t
 * @brief Decimal / hexadecimal formatting constants for the banner.
 * @details The banner is emitted without printf (no float, no heap), so the
 *          radix and nibble geometry are named here rather than inlined.
 * @invariant k_ez_nibble_mask == (1 << k_ez_nibble_bits) - 1.
 * @par Example:
 * @code
 * ez_print_hex(st.crc_1x);
 * @endcode
 * @see ez_print_banner
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ez_hex_nibbles = 8U,    /**< Hex digits in a 32-bit value. */
  k_ez_nibble_bits = 4U,    /**< Bits per hex nibble.          */
  k_ez_nibble_mask = 0x0FU, /**< Low-nibble mask.              */
  k_ez_dec_ten     = 10U,   /**< Decimal radix.                */
} ez_print_t;

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned.
 * @details The single surface the GLCDC scans out and the zoom composite paints.
 * @note Bound into ra8_gfx at boot; also hashed by the self-check.
 * @warning Written by the scene render; do not alias.
 * @since 0.1.0
 */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_ez_fb_align)]] static uint16_t s_framebuffer[(size_t)k_ez_fb_h * (size_t)k_ez_fb_w];

/**
 * @var s_cell_mem
 * @brief Tile-cache cell storage: ::k_ez_cells gray8 256x256 tiles, in SDRAM.
 * @details The viewer's large working set. It lives here, not in the zoom
 *          engine, which is the whole point of the strip composite: the
 *          magnifier's own scratch is 25 KiB of SRAM.
 * @note Owned by the tile cache once ::ez_scene_init has run.
 * @warning Never addressed directly; the cache hands out pinned cells.
 * @since 0.1.0
 */
[[gnu::section(
  ".sdram_data")]] static uint8_t s_cell_mem[(size_t)k_ez_cells * (size_t)k_ez_cell_bytes];

/**
 * @var s_meta
 * @brief Tile-cache link metadata, one entry per cell.
 * @details Small and hot, so it stays in internal SRAM rather than SDRAM.
 * @note Owned by the tile cache.
 * @warning Do not write outside ra8_tile_cache.
 * @since 0.1.0
 */
static ra8_keycache_cell_t s_meta[k_ez_cells];

/**
 * @var s_keys
 * @brief Tile-cache key storage, one entry per cell.
 * @details Compared byte-wise, so the cache zero-fills it at init.
 * @note Owned by the tile cache.
 * @warning Do not write outside ra8_tile_cache.
 * @since 0.1.0
 */
static ra8_tile_key_t s_keys[k_ez_cells];

/**
 * @var s_dims
 * @brief Tile-cache decoded-extent descriptors, one entry per cell.
 * @details Cross-checked against the declared tile geometry on every fetch.
 * @note Owned by the tile cache.
 * @warning Do not write outside ra8_tile_cache.
 * @since 0.1.0
 */
static ra8_tile_dims_t s_dims[k_ez_cells];

/**
 * @var s_buckets
 * @brief Tile-cache hash-bucket heads.
 * @details Sized above the cell count so lookups stay near O(1).
 * @note Owned by the tile cache.
 * @warning Do not write outside ra8_tile_cache.
 * @since 0.1.0
 */
static int32_t s_buckets[k_ez_buckets];

/**
 * @var s_row
 * @brief Composite scratch: one source row at the widest viewport (SRAM).
 * @details Part of the viewer's whole 25 KiB footprint; see ez_scene.h.
 * @note Shared by both viewports (only one renders at a time).
 * @warning Overwritten by every ra8_zoom render.
 * @since 0.1.0
 */
static uint8_t s_row[k_ez_row_bytes];

/**
 * @var s_strip
 * @brief Composite scratch: the gray8 destination strip (SRAM).
 * @details 16 destination rows at 1024 px; the strip height is derived from
 *          this capacity by ra8_zoom_view_open().
 * @note Shared by both viewports.
 * @warning Overwritten by every ra8_zoom render.
 * @since 0.1.0
 */
static uint8_t s_strip[k_ez_strip_bytes];

/**
 * @var s_packed
 * @brief Composite scratch: the dithered strip packed to 4 bpp (SRAM).
 * @details Handed straight to ra8_gfx_blit_gray4_zoom.
 * @note Shared by both viewports.
 * @warning Overwritten by every ra8_zoom render.
 * @since 0.1.0
 */
static uint8_t s_packed[k_ez_packed_bytes];

/**
 * @var k_ez_display_cfg
 * @brief Display PAL configuration: LCD/GLCDC backend over the SDRAM framebuffer.
 * @details Immutable; the PAL copies what it needs at ::display_init.
 * @note Read once at boot.
 * @warning Changing the pixel format here must match the ra8_gfx binding.
 * @since 0.1.0
 */
static const display_cfg_t k_ez_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_ez_fb_w,
  .height_px         = (uint16_t)k_ez_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/**
 * @var s_display
 * @brief Display PAL handle returned by ::display_init.
 * @details NULL until the panel is up.
 * @note Single display; the PAL rejects a second ::display_init.
 * @warning Do not flush through this before bring-up completes.
 * @since 0.1.0
 */
static display_handle_t* s_display = nullptr;

/**
 * @var s_fb
 * @brief Mutable copy of the framebuffer descriptor, populated at boot.
 * @details Used to bind ra8_gfx to whatever the PAL actually handed back.
 * @note Written once during bring-up.
 * @warning Stale until ::display_get_framebuffer has run.
 * @since 0.1.0
 */
static display_fb_t s_fb;

/**
 * @var s_scene
 * @brief The demo scene: tiled page, zoom viewport and loupe.
 * @details All of the app's presentation state; see ez_scene.h.
 * @note Single-threaded reader loop only.
 * @warning Not valid until ::ez_scene_init returns k_ra8_ok.
 * @since 0.1.0
 */
static ez_scene_t s_scene;

/**
 * @var s_touch_bus
 * @brief Bound I2C bus handle the GT911 driver's injected seam points at.
 * @details The I3C peripheral in I2C-compatibility mode.
 * @note Populated by ::ez_bringup_touch; unused if touch is absent.
 * @warning Must outlive the touch driver.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_touch_bus;

/** @brief Console banner fragment: boot line. */
static const uint8_t k_msg_boot[] = "ereader-zoom: boot\r\n";
/** @brief Console banner fragment: bring-up failure. */
static const uint8_t k_msg_fail[] = "ereader-zoom: FAIL init\r\n";
/** @brief Console banner fragment: scene wiring failure. */
static const uint8_t k_msg_scene[] = "ereader-zoom: FAIL scene\r\n";
/** @brief Console banner fragment: render / self-check failure. */
static const uint8_t k_msg_rend[] = "ereader-zoom: FAIL render\r\n";
/** @brief Console banner fragment: geometry prefix. */
static const uint8_t k_msg_pre[] = "ereader-zoom: page ";
/** @brief Console banner fragment: dimension separator. */
static const uint8_t k_msg_x[] = "x";
/** @brief Console banner fragment: tile edge label. */
static const uint8_t k_msg_tile[] = " tile ";
/** @brief Console banner fragment: cache cell-count label. */
static const uint8_t k_msg_cells[] = " cells ";
/** @brief Console banner fragment: 1:1 render hash label. */
static const uint8_t k_msg_z1[] = " z1=";
/** @brief Console banner fragment: panned render hash label. */
static const uint8_t k_msg_pan[] = " pan=";
/** @brief Console banner fragment: 2x render hash label. */
static const uint8_t k_msg_z2[] = " z2=";
/** @brief Console banner fragment: loupe render hash label. */
static const uint8_t k_msg_lens[] = " lens=";
/** @brief Console banner fragment: cache hit counter label. */
static const uint8_t k_msg_hit[] = " hit=";
/** @brief Console banner fragment: cache miss counter label. */
static const uint8_t k_msg_miss[] = " miss=";
/** @brief Console banner fragment: cache eviction counter label. */
static const uint8_t k_msg_evict[] = " evict=";
/** @brief Console banner fragment: prefetch counter label. */
static const uint8_t k_msg_warm[] = " warm=";
/** @brief Console banner fragment: success suffix. */
static const uint8_t k_msg_ok[] = " ok\r\n";

/** @brief Emit a byte run on the console. */
static void ez_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a fail banner, then trap (ra8_emulator halts on the BKPT). */
static void ez_panic_halt(const uint8_t* msg, uint32_t len)
{
  ez_print(msg, len);
  (void)ra8_board_led_on(k_ra8_board_led_red);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void ez_print_hex(uint32_t value)
{
  uint8_t buf[k_ez_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ez_hex_nibbles; ++i) {
    const uint32_t shift = ((uint32_t)k_ez_hex_nibbles - 1U - i) * (uint32_t)k_ez_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ez_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ez_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ez_dec_ten)));
  }
  ez_print(buf, (uint32_t)k_ez_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void ez_print_uint(uint32_t value)
{
  uint8_t  buf[k_ez_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ez_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_ez_dec_ten));
    n++;
    value /= (uint32_t)k_ez_dec_ten;
  }
  for (uint32_t i = 0U; i < n; ++i) {
    ez_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Bring up clocks, MSTP, system tick, LEDs and the SCI console. */
static void ez_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) ||
      (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok)) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_ez_uart_baud) != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  ra8_isr_globals_enable();
}

/** @brief Bring up SDRAM + the GLCDC panel, then bind ra8_gfx to the FB. */
static void ez_bringup_panel(void)
{
  ra8_delay_ms((uint32_t)k_ez_settle_ms);
  if (ra8_sdramc_init() != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (display_init(&k_ez_display_cfg, &s_display) != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    ez_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Open the GT911 touch controller (best-effort, polled). */
static void ez_bringup_touch(void)
{
  const ra8_i3c_cfg_t iic_cfg = {.mode     = k_ra8_i3c_mode_i2c,
                                 .bus_hz   = (uint32_t)k_ez_touch_bus_hz,
                                 .pclka_hz = (uint32_t)k_ez_touch_pclka_hz};
  if (ra8_i3c_init(0U, &iic_cfg) != k_ra8_ok) {
    return;
  }
  ra8_i2c_bus_ops_t bus_ops = {};
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_touch_bus, 0U) != k_ra8_ok) {
    return;
  }
  if (ra8_io_i2c_bus_as_ops(&s_touch_bus, &bus_ops) != k_ra8_ok) {
    return;
  }
  const ra8_touch_cfg_t cfg = {.bus        = bus_ops,
                               .target_7b  = (uint8_t)k_ez_touch_addr_7b,
                               .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
                               .max_points = 1U};
  (void)ra8_touch_open(&cfg);
}

/** @brief Flush a scene plan through the PAL with the waveform it asked for. */
static void ez_flush(const ez_present_t* plan)
{
  const display_rect_t rect = {
    .x = (uint16_t)plan->rect.x,
    .y = (uint16_t)plan->rect.y,
    .w = (uint16_t)plan->rect.w,
    .h = (uint16_t)plan->rect.h,
  };
  /* The engine's fast/quality split IS the e-ink waveform choice: a pan burst
   * flushes A2 (bi-level, ~10x faster than GC16) and the settle repaints the
   * same rectangle in all 16 levels. On this GLCDC panel the hint is inert;
   * the e-ink backend honours it. */
  (void)display_flush(s_display,
                      rect,
                      plan->quality ? k_display_refresh_quality : k_display_refresh_fast);
}

/** @brief Render, then flush whatever the scene says changed. */
static bool ez_redraw(void)
{
  if (ez_scene_render(&s_scene) != k_ra8_ok) {
    return false;
  }
  ez_present_t plan = {};
  if (ez_scene_present(&s_scene, &plan) != k_ra8_ok) {
    return false;
  }
  if (plan.present) {
    ez_flush(&plan);
  }
  return true;
}

/** @brief Print the deterministic boot banner (geometry + the four hashes). */
static void ez_print_banner(const ez_selftest_t* st)
{
  ez_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ez_print_uint((uint32_t)k_ez_page_w);
  ez_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  ez_print_uint((uint32_t)k_ez_page_h);
  ez_print(k_msg_tile, (uint32_t)sizeof(k_msg_tile) - 1U);
  ez_print_uint((uint32_t)k_ez_tile_edge);
  ez_print(k_msg_cells, (uint32_t)sizeof(k_msg_cells) - 1U);
  ez_print_uint((uint32_t)k_ez_cells);
  ez_print(k_msg_z1, (uint32_t)sizeof(k_msg_z1) - 1U);
  ez_print_hex(st->crc_1x);
  ez_print(k_msg_pan, (uint32_t)sizeof(k_msg_pan) - 1U);
  ez_print_hex(st->crc_pan);
  ez_print(k_msg_z2, (uint32_t)sizeof(k_msg_z2) - 1U);
  ez_print_hex(st->crc_2x);
  ez_print(k_msg_lens, (uint32_t)sizeof(k_msg_lens) - 1U);
  ez_print_hex(st->crc_lens);
  ez_print(k_msg_hit, (uint32_t)sizeof(k_msg_hit) - 1U);
  ez_print_uint(st->hits);
  ez_print(k_msg_miss, (uint32_t)sizeof(k_msg_miss) - 1U);
  ez_print_uint(st->misses);
  ez_print(k_msg_evict, (uint32_t)sizeof(k_msg_evict) - 1U);
  ez_print_uint(st->evictions);
  ez_print(k_msg_warm, (uint32_t)sizeof(k_msg_warm) - 1U);
  ez_print_uint((uint32_t)st->warmed);
  ez_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);
}

/** @brief Poll the GT911 once; on a fresh tap, mutate + redraw the viewport. */
static void ez_poll_touch(bool* was_touching)
{
  ra8_touch_point_t pt  = {};
  uint8_t           got = 0U;
  if (ra8_touch_read(&pt, 1U, &got) != k_ra8_ok) {
    return;
  }
  const bool touching = (got > 0U);
  if (touching && !(*was_touching)) {
    if (ez_scene_tap(&s_scene, (int32_t)pt.x, (int32_t)pt.y, ra8_time_ms())) {
      (void)ez_redraw();
    }
  }
  *was_touching = touching;
}

/** @brief Wire the scene over the SDRAM cache storage and the SRAM scratch. */
static bool ez_setup_scene(void)
{
  const ez_scene_cfg_t cfg = {
    .fb       = s_framebuffer,
    .fb_bytes = (uint32_t)sizeof(s_framebuffer),
    .fb_w     = (int32_t)k_ez_fb_w,
    .fb_h     = (int32_t)k_ez_fb_h,
    .cell_mem = s_cell_mem,
    .meta     = s_meta,
    .keys     = s_keys,
    .dims     = s_dims,
    .buckets  = s_buckets,
    .row      = s_row,
    .strip    = s_strip,
    .packed   = s_packed,
  };
  return ez_scene_init(&s_scene, &cfg) == k_ra8_ok;
}

/**
 * @brief App entry: panel bring-up -> tiled page -> self-check -> zoom loop.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The boot banner is emitted; the reader loop services taps forever.
 * @since 0.1.0
 */
void main(void)
{
  ez_bringup_clocks();
  ez_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);
  ez_bringup_panel();
  ez_bringup_touch();

  if (!ez_setup_scene()) {
    ez_panic_halt(k_msg_scene, (uint32_t)sizeof(k_msg_scene) - 1U);
  }

  ez_selftest_t st = {};
  if (ez_scene_selftest(&s_scene, &st) != k_ra8_ok) {
    ez_panic_halt(k_msg_rend, (uint32_t)sizeof(k_msg_rend) - 1U);
  }
  /* The self-check consumed every pending plan, so publish its final frame
   * explicitly: one clean full-quality flush of the content area. */
  const ez_present_t final_plan = {
    .rect    = ez_content_rect((int32_t)k_ez_fb_w, (int32_t)k_ez_fb_h),
    .quality = true,
    .present = true,
  };
  ez_flush(&final_plan);
  ez_print_banner(&st);

  bool     was_touching = false;
  uint32_t ticks        = 0U;
  while (1) {
    ez_poll_touch(&was_touching);
    /* Settle promotion: once a gesture burst has been still for the view's
     * settle window, repaint the same rectangle at full 16-level quality. */
    if (ez_scene_tick(&s_scene, ra8_time_ms())) {
      (void)ez_redraw();
    }
    if ((ticks % (uint32_t)k_ez_led_every) == 0U) {
      (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    }
    ticks++;
    ra8_delay_ms((uint32_t)k_ez_frame_ms);
  }
}
