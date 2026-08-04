/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/ereader_longstrip/main.c
 * @brief Viewable continuous vertical-scroll (longstrip / manhwa) reader (#289).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * The #289 continuous vertical-scroll engine (`ra8_longstrip`) made VIEWABLE on
 * the stock EK-RA8D2 1024x600 GLCDC panel: the headless sibling scrolled a
 * strip into a 160x120 scratch buffer and printed a CRC; this app binds the
 * engine's band-composite blit sink to the LIVE panel framebuffer, so
 * ra8_emulator's `--view` window (or a `--ppm` capture) shows the reader screen.
 *
 * Pipeline: boot clocks/MSTP/SysTick/console/LEDs, bring up external SDRAM + the
 * 1024x600 RGB565 GLCDC panel via `ra8_display_pal`
 * (`k_display_backend_lcd_ra8_glcdc`), bind `ra8_gfx` to the panel FB, then
 * build a tall colourful strip as a JOF band-tile atlas (`ra8_jof`, one
 * full-width band column). Only the atlas header + index + footer are
 * materialised (~176 bytes); each band's pixels are painted PROCEDURALLY on a
 * cache miss (a vivid palette hue + a top->bottom brightness gradient + a white
 * seam stripe at every band edge, so scrolling is obviously visible), which
 * keeps boot instant and needs no multi-megabyte pixel buffer while still
 * exercising the real streaming path: bands page through a deliberately small
 * `ra8_tile_cache` (fewer cells than bands, so the LRU must evict -- the #147
 * bounded-memory property) and `ra8_longstrip_render` composites the visible
 * range into the panel FB through `ra8_gfx_blit`.
 *
 * Navigation is discrete-tap only (the ra8_emulator GT911 model has no swipe/drag
 * gesture, so this app needs zero ra8_emulator change): a tap in the BOTTOM third
 * pages down, the TOP third pages up, the CENTRE third toggles the chrome
 * (status bar + scroll rail); SW1/SW2 page down/up as a button fallback. A
 * deterministic boot banner (band count, viewport, initial scroll, FNV-1a-32
 * over the panel FB) is emitted over SCI8 so the headless EIL / smoke gate still
 * asserts a stable render, and ``g_ls_loop_ticks`` feeds the jlink liveness
 * probe.
 *
 *
 * @see ra8_longstrip.h  The continuous vertical-scroll engine this drives.
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_jof.h"
#include "ra8_longstrip.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_tile_cache.h"
#include "ra8_time.h"
#include "ra8_touch.h"

/* ===========================================================================
 * Geometry (no magic numbers)
 * ===========================================================================
 */

/**
 * @enum ls_geom_t
 * @brief Panel, chrome and strip geometry in pixels.
 *
 * @details The viewport is the whole panel; the status bar and scroll rail are
 *          chrome OVERLAYS drawn on top of the strip, so toggling them off lets
 *          the strip fill the full 1024x600 panel.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ls_view_w    = k_panel_width_px,  /**< Viewport / strip width  (== panel).   */
  k_ls_view_h    = k_panel_height_px, /**< Viewport / strip-window height.       */
  k_ls_band_h    = 280U,              /**< One band (JOF tile) height, pixels.   */
  k_ls_bands     = 16U,               /**< Bands in the strip (16 palette hues). */
  k_ls_status_h  = 40U,               /**< Status-bar overlay band height.       */
  k_ls_rail_w    = 12U,               /**< Scroll-rail overlay width, pixels.    */
  k_ls_text_pad  = 16U,               /**< Status-bar left text inset.           */
  k_ls_glyph_w   = 8U,                /**< Bundled font cell width.              */
  k_ls_glyph_h   = 16U,               /**< Bundled font cell height.             */
  k_ls_fb_align  = 64U,               /**< 64-byte AXI-burst FB alignment.       */
  k_ls_settle_ms = 500U,              /**< PLL / SDRAM / panel-POR settle.       */
  k_ls_frame_ms  = 20U,               /**< Input-poll period (ms).               */
  k_ls_led_every = 16U,               /**< Toggle heartbeat LED every Nth frame. */
} ls_geom_t;

/**
 * @enum ls_size_t
 * @brief 32-bit strip + cache + scroll magnitudes.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_canvas_w = (uint32_t)k_panel_width_px,                   /**< Strip width, pixels. */
  k_ls_canvas_h = (uint32_t)k_ls_bands * (uint32_t)k_ls_band_h, /**< 4480 px.             */
  k_ls_bpp      = 3U,                                           /**< RGB888 strip pixels. */
  /** One band payload, bytes. */
  k_ls_band_bytes = (uint32_t)k_panel_width_px * (uint32_t)k_ls_band_h * 3U,
  k_ls_cells      = 6U,  /**< Tile-cache cells (< 16 bands -> LRU eviction). */
  k_ls_buckets    = 16U, /**< Tile-cache hash buckets.                       */
  k_ls_image_id   = 1U,  /**< Tile-cache key namespace for this strip.       */
  k_ls_scroll_page =
    ((uint32_t)k_panel_height_px * 9U) / 10U, /**< Page-scroll step (~90% of a viewport), pixels. */
} ls_size_t;

/**
 * @enum ls_atlas_t
 * @brief JOF metadata-only atlas layout (procedural bands -> no pixel region).
 *
 * @details `ra8_jof_parse` reads only the header + footer and validates
 *          `index_off + tiles*8 + 16 == total`, never the pixel bytes, so the
 *          index is placed immediately after the header and the whole atlas is
 *          just header + index + footer. Band pixels are synthesised on demand
 *          by ::ls_band_decode, so no pixel region is stored.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_hdr_bytes     = 32U, /**< JOF header length.                 */
  k_ls_ftr_bytes     = 16U, /**< JOF footer length.                 */
  k_ls_idx_entry     = 8U,  /**< Index-entry length (offset + len). */
  k_ls_ftr_magic_off = 12U, /**< Footer "JOFE" magic offset.        */
  k_ls_atlas_total   = (uint32_t)k_ls_hdr_bytes + ((uint32_t)k_ls_bands * 8U) +
                       (uint32_t)k_ls_ftr_bytes, /**< 176-byte atlas length. */
  k_ls_byte_mask     = 0xFFU,                    /**< Low-byte mask.         */
  k_ls_shift_8       = 8U,                       /**< One-byte shift.        */
  k_ls_shift_16      = 16U,                      /**< Two-byte shift.        */
  k_ls_shift_24      = 24U,                      /**< Three-byte shift.      */
} ls_atlas_t;

/**
 * @enum ls_paint_t
 * @brief Procedural band-painter constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_grad_lo      = 55U,   /**< Band-top brightness (percent of the hue). */
  k_ls_grad_span    = 45U,   /**< Top->bottom brightness gain (percent).    */
  k_ls_pct_full     = 100U,  /**< Percentage denominator.                   */
  k_ls_seam_h       = 6U,    /**< White seam-stripe rows at each band top.  */
  k_ls_seam_level   = 0xF0U, /**< Seam-stripe grey level (near white).      */
  k_ls_palette_size = 16U,   /**< Entries in the vivid band palette.        */
} ls_paint_t;

/**
 * @enum ls_zone_t
 * @brief Vertical tap-zone thresholds (thirds of the panel), pixels.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ls_zone_top = (uint16_t)(k_panel_height_px / 3U),        /**< y < this: up.   */
  k_ls_zone_mid = (uint16_t)((k_panel_height_px * 2U) / 3U), /**< < this: chrome. */
} ls_zone_t;

/**
 * @enum ls_color_t
 * @brief Chrome palette, 0x00RRGGBB.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_col_bg         = 0x0A0A12U, /**< Content backdrop (behind bands). */
  k_ls_col_status_bg  = 0x101826U, /**< Status-bar fill.                 */
  k_ls_col_text       = 0xF0F4FFU, /**< Status-bar primary text.         */
  k_ls_col_text_dim   = 0x8FA0C0U, /**< Status-bar secondary text.       */
  k_ls_col_rail_bg    = 0x263042U, /**< Scroll-rail track.               */
  k_ls_col_rail_thumb = 0xF5C542U, /**< Scroll-rail thumb (amber).       */
} ls_color_t;

/**
 * @enum ls_rail_t
 * @brief Scroll-rail thumb geometry, pixels.
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ls_rail_min_thumb = 32, /**< Minimum thumb height so it stays grabbable. */
} ls_rail_t;

/**
 * @enum ls_console_t
 * @brief Console baud + FNV-1a-32 + decimal/hex print knobs.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_uart_baud   = 115200U,     /**< SCI8 console baud.            */
  k_ls_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.       */
  k_ls_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.              */
  k_ls_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value. */
  k_ls_nibble_bits = 4U,          /**< Bits per hex nibble.          */
  k_ls_nibble_mask = 0x0FU,       /**< Low-nibble mask.              */
  k_ls_dec_ten     = 10U,         /**< Decimal base / hex split.     */
  k_ls_dec_digits  = 10U,         /**< Max decimal digits (u32).     */
} ls_console_t;

/**
 * @enum ls_touch_cfg_t
 * @brief GT911 touch-controller wiring (IIC_B channel 0, polled).
 *
 * @details ra8_emulator feeds `--click` / `--touch-seq` / window taps through this
 *          same `ra8_touch` -> I2C -> GT911 path unchanged.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ls_touch_channel    = 0U,    /**< IIC_B channel 0.          */
  k_ls_touch_addr_7b    = 0x5DU, /**< GT911 default 7-bit addr. */
  k_ls_touch_max_points = 1U,    /**< One contact = one tap.    */
} ls_touch_cfg_t;

/**
 * @enum ls_touch_bus_t
 * @brief Clocking for the app-owned touch I2C bus.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ls_touch_bus_hz   = 400000U,   /**< Fast-mode I2C clock.     */
  k_ls_touch_pclka_hz = 60000000U, /**< IIC_B clock-source rate. */
} ls_touch_bus_t;

/* ===========================================================================
 * Static storage
 * ===========================================================================
 */

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ereader_longstrip";

/**
 * @brief Vivid 16-hue band palette (0x00RRGGBB) -- distinct so scroll is obvious.
 * @note Read-only; indexed by `band % k_ls_palette_size`.
 * @since 0.1.0
 */
static const uint32_t k_ls_palette[k_ls_palette_size] = {
  0xE6194BU,
  0xF58231U,
  0xFFE119U,
  0xBFEF45U,
  0x3CB44BU,
  0x42D4F4U,
  0x4363D8U,
  0x911EB4U,
  0xF032E6U,
  0xFABEBEU,
  0x469990U,
  0x9A6324U,
  0x800000U,
  0x808000U,
  0x000075U,
  0xA9A9A9U,
};

/**
 * @var s_framebuffer
 * @brief RGB565 panel framebuffer in external SDRAM, AXI-burst aligned.
 * @warning Scanned out continuously by the GLCDC; write only through ra8_gfx.
 * @since 0.1.0
 */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_ls_fb_align)]] static uint16_t s_framebuffer[(size_t)k_ls_view_h * (size_t)k_ls_view_w];

/** @brief Tile-cache cell storage in SDRAM (one decoded band per cell). */
[[gnu::section(
  ".sdram_data")]] static uint8_t s_cells[(size_t)k_ls_cells * (size_t)k_ls_band_bytes];

/** @brief Metadata-only JOF atlas backing (header + index + footer). */
static uint8_t s_atlas[k_ls_atlas_total];

static ra8_tile_key_t      s_keys[(size_t)k_ls_cells];      /**< Cache key storage.   */
static ra8_tile_dims_t     s_dims[(size_t)k_ls_cells];      /**< Cache dim storage.   */
static ra8_keycache_cell_t s_meta[(size_t)k_ls_cells];      /**< Cache link metadata. */
static int32_t             s_buckets[(size_t)k_ls_buckets]; /**< Cache hash buckets.  */

static ra8_jof_memstore_t s_store; /**< Atlas backing-read seam.        */
static ra8_tile_cache_t   s_cache; /**< Band cache (procedural decode). */
static ra8_longstrip_t    s_strip; /**< Opened longstrip scroll state.  */

/**
 * @struct ls_paint_ctx_t
 * @brief Geometry the procedural band painter needs (tile-cache decode ctx).
 * @since 0.1.0
 */
typedef struct {
  uint32_t width;    /**< Strip / band width, pixels.   */
  uint32_t canvas_h; /**< Total strip height, pixels.   */
  uint16_t band_h;   /**< Nominal band height, pixels.  */
  uint8_t  bpp;      /**< Bytes per pixel (3 = RGB888). */
} ls_paint_ctx_t;

/** @brief Painter context bound as the tile cache's decode ctx. */
static ls_paint_ctx_t s_paint;

/** @brief Display-PAL config: LCD/GLCDC backend over the SDRAM framebuffer. */
static const display_cfg_t k_ls_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_ls_view_w,
  .height_px         = (uint16_t)k_ls_view_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;
/** @brief Mutable copy of the FB descriptor populated at boot. */
static display_fb_t s_fb;
/** @brief Bound I2C bus handle the touch driver's injected seam references. */
static ra8_io_i2c_bus_t s_touch_bus;

/** @brief True while the status bar + scroll rail chrome is shown. */
static bool s_chrome = true;
/** @brief Skipped-band count from the last render (must stay 0: #289 contract). */
static uint16_t s_last_skipped = 0U;
/** @brief Edge-detect: was SW1 held on the previous button poll. */
static bool s_was_sw1 = false;
/** @brief Edge-detect: was SW2 held on the previous button poll. */
static bool s_was_sw2 = false;

/** @brief Free-running main-loop counter -- HIL/liveness probe symbol. */
volatile uint32_t g_ls_loop_ticks = 0U;
/** @brief Current scroll position (px) mirrored for `--dump-sym` telemetry. */
volatile uint32_t g_ls_scroll_y = 0U;
/** @brief Count of full redraws since boot -- render-progress telemetry. */
volatile uint32_t g_ls_redraws = 0U;
/** @brief FNV-1a-32 over the panel FB at the boot banner (render fingerprint). */
volatile uint32_t g_ls_frame_crc = 0U;

/** @brief Boot / banner literals (SCI8). */
static const uint8_t k_msg_boot[]   = "ereader-longstrip: boot\r\n";
static const uint8_t k_msg_pre[]    = "ereader-longstrip: bands=";
static const uint8_t k_msg_view[]   = " view=";
static const uint8_t k_msg_x[]      = "x";
static const uint8_t k_msg_scroll[] = " scroll=";
static const uint8_t k_msg_crc[]    = " crc=";
static const uint8_t k_msg_eol[]    = "\r\n";

/* ===========================================================================
 * Console helpers
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void ls_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void ls_print_hex(uint32_t value)
{
  uint8_t buf[k_ls_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ls_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_ls_hex_nibbles - 1U - i) * (uint32_t)k_ls_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ls_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ls_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ls_dec_ten)));
  }
  ls_print(buf, (uint32_t)k_ls_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal on the console. */
static void ls_print_uint(uint32_t value)
{
  uint8_t  buf[k_ls_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ls_dec_digits)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_ls_dec_ten));
    n++;
    value /= (uint32_t)k_ls_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    ls_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Format an unsigned integer as a NUL-terminated ASCII decimal string.
 * @param[out] buf   Destination (at least ::k_ls_dec_digits + 1 bytes).
 * @param[in]  value Value to format.
 * @return @p buf.
 * @since 0.1.0
 */
static const char* ls_utoa(char* buf, uint32_t value)
{
  char     tmp[k_ls_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    tmp[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ls_dec_digits)) {
    tmp[n] = (char)('0' + (value % (uint32_t)k_ls_dec_ten));
    n++;
    value /= (uint32_t)k_ls_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  buf[n] = '\0';
  return buf;
}

/* ===========================================================================
 * Boot helpers
 * ===========================================================================
 */

/** @brief Halt forever with the red board LED on (visual sign of fatal init). */
static void app_panic_halt(void)
{
  (void)ra8_board_led_on(k_ra8_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring up clocks, MSTP, SysTick, the SCI8 console and both LEDs. */
static void app_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    app_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ls_uart_baud) != k_ra8_ok) {
    app_panic_halt();
  }
  if ((ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) ||
      (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok)) {
    app_panic_halt();
  }
  ra8_isr_globals_enable();
}

/** @brief Bring up external SDRAM + the GLCDC panel, then cache the FB. */
static void app_bringup_panel(void)
{
  ra8_delay_ms((uint32_t)k_ls_settle_ms);
  if (ra8_sdramc_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_init(&k_ls_display_cfg, &s_display) != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    app_panic_halt();
  }
}

/** @brief Bind ra8_gfx to the panel framebuffer. */
static void app_bringup_gfx(void)
{
  if (ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    app_panic_halt();
  }
}

/** @brief Open the GT911 touch controller (best-effort, polled; non-fatal). */
static void app_bringup_touch(void)
{
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_ls_touch_bus_hz,
    .pclka_hz = (uint32_t)k_ls_touch_pclka_hz,
  };
  if (ra8_i3c_init((uint8_t)k_ls_touch_channel, &iic_cfg) != k_ra8_ok) {
    return;
  }
  ra8_i2c_bus_ops_t bus_ops = {};
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_touch_bus, (uint8_t)k_ls_touch_channel) != k_ra8_ok) {
    return;
  }
  if (ra8_io_i2c_bus_as_ops(&s_touch_bus, &bus_ops) != k_ra8_ok) {
    return;
  }
  const ra8_touch_cfg_t cfg = {
    .bus        = bus_ops,
    .target_7b  = (uint8_t)k_ls_touch_addr_7b,
    .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
    .max_points = (uint8_t)k_ls_touch_max_points,
  };
  (void)ra8_touch_open(&cfg);
}

/* ===========================================================================
 * Metadata-only JOF atlas builder
 * ===========================================================================
 */

/** @brief Write a little-endian u16 at @p p. */
static void ls_put_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_ls_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_ls_shift_8) & (uint16_t)k_ls_byte_mask);
}

/** @brief Write a little-endian u32 at @p p. */
static void ls_put_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_ls_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_ls_shift_8) & (uint32_t)k_ls_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_ls_shift_16) & (uint32_t)k_ls_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_ls_shift_24) & (uint32_t)k_ls_byte_mask);
}

/**
 * @brief Build the metadata-only JOF band-tile atlas; return its byte length.
 *
 * @details Emits only the header, the tile index (immediately after the header,
 *          so there is no pixel region) and the footer. `ra8_jof_parse`
 *          validates `index_off + tiles*8 + 16 == total` and never reads the
 *          pixel bytes, so the strip parses with band pixels supplied later by
 *          ::ls_band_decode.
 * @return The atlas length in bytes (::k_ls_atlas_total).
 * @note Not thread-safe (mutates `s_atlas`).
 * @since 0.1.0
 */
static uint32_t ls_build_atlas(void)
{
  const uint32_t width  = (uint32_t)k_ls_canvas_w;
  const uint32_t height = (uint32_t)k_ls_canvas_h;
  const uint32_t band_h = (uint32_t)k_ls_band_h;
  const uint32_t bands  = (uint32_t)k_ls_bands;
  for (uint32_t i = 0U; i < (uint32_t)k_ls_hdr_bytes; i++) {
    s_atlas[i] = 0U;
  }
  s_atlas[0] = 'J';
  s_atlas[1] = 'O';
  s_atlas[2] = 'F';
  s_atlas[3] = '1';
  ls_put_u16(&s_atlas[4], (uint16_t)width);
  ls_put_u16(&s_atlas[6], (uint16_t)height);
  ls_put_u16(&s_atlas[8], (uint16_t)width); /* tile_w == width (band column) */
  ls_put_u16(&s_atlas[10], (uint16_t)band_h);
  s_atlas[12] = (uint8_t)k_ls_bpp;
  s_atlas[13] = (uint8_t)k_ra8_jof_codec_raw;
  ls_put_u32(&s_atlas[16], bands);

  const uint32_t index_off = (uint32_t)k_ls_hdr_bytes; /* no pixel region */
  for (uint32_t n = 0U; n < bands; n++) {
    const uint32_t y0 = n * band_h;
    const uint32_t th = ((height - y0) < band_h) ? (height - y0) : band_h;
    ls_put_u32(&s_atlas[index_off + (n * (uint32_t)k_ls_idx_entry)], index_off);
    ls_put_u32(&s_atlas[index_off + (n * (uint32_t)k_ls_idx_entry) + 4U],
               width * th * (uint32_t)k_ls_bpp);
  }
  const uint32_t ftr = index_off + (bands * (uint32_t)k_ls_idx_entry);
  ls_put_u32(&s_atlas[ftr], index_off);
  ls_put_u32(&s_atlas[ftr + 4U], bands);
  ls_put_u32(&s_atlas[ftr + 8U], (uint32_t)k_ls_atlas_total);
  const uint32_t mag = ftr + (uint32_t)k_ls_ftr_magic_off;
  s_atlas[mag + 0U]  = 'J';
  s_atlas[mag + 1U]  = 'O';
  s_atlas[mag + 2U]  = 'F';
  s_atlas[mag + 3U]  = 'E';
  return (uint32_t)k_ls_atlas_total;
}

/* ===========================================================================
 * Procedural band painter (tile-cache decode-on-miss seam)
 * ===========================================================================
 */

/** @brief Scale an 8-bit channel by a 0..100 percentage (saturating at 255). */
static uint8_t ls_scale_u8(uint8_t v, uint32_t pct)
{
  return (uint8_t)(((uint32_t)v * pct) / (uint32_t)k_ls_pct_full);
}

/**
 * @brief Compute the RGB888 colour of one strip row inside a band.
 *
 * @details Rows in the seam stripe (`r < k_ls_seam_h`) are a bright neutral so
 *          band boundaries read as crisp lines; the rest is the band's palette
 *          hue on a top(dim)->bottom(full) brightness gradient, so vertical
 *          motion is visible even within one hue.
 * @param[in]  base Band base colour (0x00RRGGBB).
 * @param[in]  r    Row index within the band.
 * @param[in]  th   Band height in rows (>= 1).
 * @param[out] rgb  Receives the row's R,G,B bytes (3 bytes).
 * @note Pure over its inputs.
 * @since 0.1.0
 */
static void ls_row_color(uint32_t base, uint32_t r, uint32_t th, uint8_t* rgb)
{
  if (r < (uint32_t)k_ls_seam_h) {
    rgb[0] = (uint8_t)k_ls_seam_level;
    rgb[1] = (uint8_t)k_ls_seam_level;
    rgb[2] = (uint8_t)k_ls_seam_level;
    return;
  }
  const uint32_t span = (th > 1U) ? (th - 1U) : 1U;
  const uint32_t pct  = (uint32_t)k_ls_grad_lo + (((uint32_t)k_ls_grad_span * r) / span);
  rgb[0] =
    ls_scale_u8((uint8_t)((base >> (uint32_t)k_ls_shift_16) & (uint32_t)k_ls_byte_mask), pct);
  rgb[1] = ls_scale_u8((uint8_t)((base >> (uint32_t)k_ls_shift_8) & (uint32_t)k_ls_byte_mask), pct);
  rgb[2] = ls_scale_u8((uint8_t)(base & (uint32_t)k_ls_byte_mask), pct);
}

/**
 * @brief Tile-cache decode-on-miss: paint band @p key->tile_y procedurally.
 *
 * @param[in]  ctx        A ::ls_paint_ctx_t*.
 * @param[in]  key        Band to paint (`tile_y` = band index).
 * @param[out] cell       Destination cell pixels.
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Receives the band width, pixels.
 * @param[out] out_h      Receives the band height, pixels (clamped for the last).
 * @return ra8_err_t
 * @retval k_ra8_ok               Band painted into @p cell.
 * @retval k_ra8_err_null_ptr     A required pointer is NULL.
 * @retval k_ra8_err_out_of_range The band payload exceeds @p cell_bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ls_band_decode(void*                 ctx,
                                const ra8_tile_key_t* key,
                                uint8_t*              cell,
                                uint32_t              cell_bytes,
                                uint16_t*             out_w,
                                uint16_t*             out_h)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "decode ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(cell, s_tag, "cell must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  const ls_paint_ctx_t* pc = (const ls_paint_ctx_t*)ctx;
  const uint32_t        y0 = (uint32_t)key->tile_y * (uint32_t)pc->band_h;
  uint32_t              th = (uint32_t)pc->band_h;
  if ((y0 + th) > pc->canvas_h) {
    th = pc->canvas_h - y0;
  }
  if ((pc->width * th * (uint32_t)pc->bpp) > cell_bytes) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t base = k_ls_palette[(uint32_t)key->tile_y % (uint32_t)k_ls_palette_size];
  uint32_t       off  = 0U;
  for (uint32_t r = 0U; r < th; r++) { /* bounded by band_h */
    uint8_t rgb[k_ls_bpp];
    ls_row_color(base, r, th, rgb);
    for (uint32_t x = 0U; x < pc->width; x++) { /* bounded by canvas width */
      cell[off]      = rgb[0];
      cell[off + 1U] = rgb[1];
      cell[off + 2U] = rgb[2];
      off += (uint32_t)pc->bpp;
    }
  }
  *out_w = (uint16_t)pc->width;
  *out_h = (uint16_t)th;
  return k_ra8_ok;
}

/* ===========================================================================
 * Strip open + render
 * ===========================================================================
 */

static ra8_err_t ls_blit(void*          ctx,
                         const uint8_t* pixels,
                         uint16_t       src_w,
                         uint16_t       src_h,
                         uint8_t        bpp,
                         int32_t        dst_x,
                         int32_t        dst_y);

/** @brief Wire the memstore/painter/cache and open the strip; halt on failure. */
static void ls_open_strip(uint32_t total)
{
  s_store = (ra8_jof_memstore_t){.buf = s_atlas, .cap = (size_t)k_ls_atlas_total, .len = total};
  s_paint = (ls_paint_ctx_t){.width    = (uint32_t)k_ls_canvas_w,
                             .canvas_h = (uint32_t)k_ls_canvas_h,
                             .band_h   = (uint16_t)k_ls_band_h,
                             .bpp      = (uint8_t)k_ls_bpp};
  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cells,
                                     .cell_bytes   = (uint32_t)k_ls_band_bytes,
                                     .cell_count   = (uint32_t)k_ls_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_ls_buckets,
                                     .decode       = ls_band_decode,
                                     .decode_ctx   = &s_paint};
  if (ra8_tile_cache_init(&s_cache, &ccfg) != k_ra8_ok) {
    app_panic_halt();
  }
  const ra8_longstrip_cfg_t lcfg = {.pread      = ra8_jof_memstore_pread,
                                    .pread_ctx  = &s_store,
                                    .atlas_size = total,
                                    .cache      = &s_cache,
                                    .image_id   = (uint32_t)k_ls_image_id,
                                    .viewport_w = (uint16_t)k_ls_view_w,
                                    .viewport_h = (uint16_t)k_ls_view_h,
                                    .blit       = ls_blit,
                                    .blit_ctx   = nullptr};
  if (ra8_longstrip_open(&s_strip, &lcfg) != k_ra8_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Longstrip blit sink -> ra8_gfx software blit (RGB888 -> RGB565).
 * @param[in] ctx    Unused sink context.
 * @param[in] pixels Decoded band pixels (RGB888, row-major).
 * @param[in] src_w  Band width, pixels.
 * @param[in] src_h  Band height, pixels.
 * @param[in] bpp    Bytes per pixel (unused: strip is RGB888).
 * @param[in] dst_x  Destination left in the framebuffer.
 * @param[in] dst_y  Destination top in the framebuffer (may be negative).
 * @return k_ra8_ok on success; k_ra8_err_null_ptr if @p pixels is NULL.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ls_blit(void*          ctx,
                         const uint8_t* pixels,
                         uint16_t       src_w,
                         uint16_t       src_h,
                         uint8_t        bpp,
                         int32_t        dst_x,
                         int32_t        dst_y)
{
  (void)ctx;
  (void)bpp; /* strip is RGB888 by construction */
  RA8_CHECK_NULL_PTR(pixels, s_tag, "blit pixels must not be nullptr");
  return ra8_gfx_blit(pixels, src_w, src_h, k_ra8_gfx_format_rgb888, dst_x, dst_y);
}

/** @brief Draw a text run in the bundled font; return the next x column. */
static int32_t ls_text(int32_t x, int32_t y, const char* str, uint32_t color)
{
  (void)ra8_gfx_text_out(x, y, str, &ra8_gfx_font_8x16, color, (uint32_t)k_ls_col_status_bg);
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &w, &h) != k_ra8_ok) {
    return x;
  }
  return x + (int32_t)w;
}

/** @brief Draw the status bar overlay: wordmark + band + scroll-position text. */
static void ls_draw_status(void)
{
  (void)ra8_gfx_rect(0,
                     0,
                     (int32_t)k_ls_view_w,
                     (int32_t)k_ls_status_h,
                     (uint32_t)k_ls_col_status_bg,
                     true);
  const int32_t  y = (int32_t)((k_ls_status_h - k_ls_glyph_h) / 2U);
  const uint32_t band =
    (s_strip.band_h > 0U) ? ((uint32_t)s_strip.scroll_y / (uint32_t)s_strip.band_h) + 1U : 1U;
  const uint32_t pct =
    (s_strip.max_scroll > 0)
      ? ((uint32_t)s_strip.scroll_y * (uint32_t)k_ls_pct_full) / (uint32_t)s_strip.max_scroll
      : 0U;
  char    num[k_ls_dec_digits + 1U];
  int32_t x = (int32_t)k_ls_text_pad;
  x         = ls_text(x, y, "LONGSTRIP", (uint32_t)k_ls_col_text);
  x         = ls_text(x, y, "   band ", (uint32_t)k_ls_col_text_dim);
  x         = ls_text(x, y, ls_utoa(num, band), (uint32_t)k_ls_col_text);
  x         = ls_text(x, y, "/", (uint32_t)k_ls_col_text_dim);
  x         = ls_text(x, y, ls_utoa(num, (uint32_t)k_ls_bands), (uint32_t)k_ls_col_text);
  x         = ls_text(x, y, "   pos ", (uint32_t)k_ls_col_text_dim);
  x         = ls_text(x, y, ls_utoa(num, pct), (uint32_t)k_ls_col_text);
  x         = ls_text(x, y, "%   skip ", (uint32_t)k_ls_col_text_dim);
  (void)ls_text(x, y, ls_utoa(num, (uint32_t)s_last_skipped), (uint32_t)k_ls_col_text);
}

/** @brief Draw the right-edge scroll rail overlay (track + proportional thumb). */
static void ls_draw_rail(void)
{
  const int32_t rx = (int32_t)k_ls_view_w - (int32_t)k_ls_rail_w;
  (void)ra8_gfx_rect(rx,
                     0,
                     (int32_t)k_ls_rail_w,
                     (int32_t)k_ls_view_h,
                     (uint32_t)k_ls_col_rail_bg,
                     true);
  int32_t th = (int32_t)(((uint32_t)k_ls_view_h * (uint32_t)k_ls_view_h) / (uint32_t)k_ls_canvas_h);
  if (th < (int32_t)k_ls_rail_min_thumb) {
    th = (int32_t)k_ls_rail_min_thumb;
  }
  const int32_t range = (int32_t)k_ls_view_h - th;
  int32_t       ty    = 0;
  if ((s_strip.max_scroll > 0) && (range > 0)) {
    ty = (int32_t)(((uint32_t)s_strip.scroll_y * (uint32_t)range) / (uint32_t)s_strip.max_scroll);
  }
  (void)ra8_gfx_rect(rx, ty, (int32_t)k_ls_rail_w, th, (uint32_t)k_ls_col_rail_thumb, true);
}

/** @brief Repaint the whole panel: strip bands, then the chrome overlays. */
static void ls_redraw(void)
{
  (void)ra8_gfx_reset_clip();
  (void)ra8_gfx_clear((uint32_t)k_ls_col_bg);
  ra8_longstrip_render_stats_t st = {};
  (void)ra8_longstrip_render(&s_strip, &st);
  s_last_skipped = st.skipped;
  if (s_chrome) {
    ls_draw_rail();
    ls_draw_status();
  }
  g_ls_scroll_y = (uint32_t)s_strip.scroll_y;
  g_ls_redraws++;
}

/** @brief Present the framebuffer to the panel (clean full refresh). */
static void ls_present(void)
{
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_quality);
}

/** @brief Fold an FNV-1a-32 over the whole panel framebuffer. */
static uint32_t ls_hash_fb(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_ls_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_ls_fnv_prime;
  }
  return hsh;
}

/** @brief Emit the deterministic boot banner (bands, viewport, scroll, FB CRC). */
static void ls_emit_banner(void)
{
  g_ls_frame_crc = ls_hash_fb();
  ls_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ls_print_uint((uint32_t)k_ls_bands);
  ls_print(k_msg_view, (uint32_t)sizeof(k_msg_view) - 1U);
  ls_print_uint((uint32_t)k_ls_view_w);
  ls_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  ls_print_uint((uint32_t)k_ls_view_h);
  ls_print(k_msg_scroll, (uint32_t)sizeof(k_msg_scroll) - 1U);
  ls_print_uint((uint32_t)s_strip.scroll_y);
  ls_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  ls_print_hex(g_ls_frame_crc);
  ls_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);
}

/* ===========================================================================
 * Input handling (tap zones + buttons)
 * ===========================================================================
 */

/**
 * @brief Apply one navigation action, redrawing + presenting if state changed.
 * @param[in] delta  Scroll delta (px, +down/-up); ignored when @p toggle is set.
 * @param[in] toggle True to toggle the chrome overlay instead of scrolling.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ls_apply_nav(int32_t delta, bool toggle)
{
  if (toggle) {
    s_chrome = !s_chrome;
  } else {
    const int32_t before = s_strip.scroll_y;
    (void)ra8_longstrip_scroll_by(&s_strip, delta);
    if ((s_strip.scroll_y == before) && (delta != 0)) {
      return; /* already pinned at an end -- nothing to repaint */
    }
  }
  ls_redraw();
  ls_present();
}

/**
 * @brief Map a tap Y coordinate to a navigation action (tap-zone hit-test).
 * @param[in] y Panel Y coordinate of the contact (pixels).
 * @note Bottom third pages down, top third pages up, centre toggles chrome.
 * @since 0.1.0
 */
static void ls_handle_tap(int32_t y)
{
  if (y < (int32_t)k_ls_zone_top) {
    ls_apply_nav(-(int32_t)k_ls_scroll_page, false);
  } else if (y < (int32_t)k_ls_zone_mid) {
    ls_apply_nav(0, true);
  } else {
    ls_apply_nav((int32_t)k_ls_scroll_page, false);
  }
}

/**
 * @brief Poll the touch controller; dispatch one tap per delivered GT911 frame.
 *
 * @details Each drained GT911 frame is treated as one discrete tap. The GT911
 *          reports a fresh "ready" frame on every poll while a contact is
 *          present (and ra8_emulator serves one queued `--touch-seq` tap per read
 *          with no interposed release), so a rising-edge gate would collapse a
 *          whole tap sequence into a single action; acting per delivered frame
 *          makes each queued tap page independently. There is no swipe/drag
 *          model, so navigation stays purely discrete-tap.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ls_poll_touch(void)
{
  ra8_touch_point_t pt  = {};
  uint8_t           got = 0U;
  if (ra8_touch_read(&pt, (uint8_t)k_ls_touch_max_points, &got) != k_ra8_ok) {
    return;
  }
  if (got > 0U) {
    ls_handle_tap((int32_t)pt.y);
  }
}

/** @brief Poll SW1/SW2 and page up/down on a fresh press (button fallback). */
static void ls_poll_buttons(void)
{
  ra8_board_sw_state_t s1 = {};
  ra8_board_sw_state_t s2 = {};
  const bool           d1 =
    (ra8_board_sw_read(k_ra8_board_sw1, &s1) == k_ra8_ok) && (s1 == k_ra8_board_sw_pressed);
  const bool d2 =
    (ra8_board_sw_read(k_ra8_board_sw2, &s2) == k_ra8_ok) && (s2 == k_ra8_board_sw_pressed);
  if (d1 && !s_was_sw1) {
    ls_apply_nav((int32_t)k_ls_scroll_page, false);
  }
  if (d2 && !s_was_sw2) {
    ls_apply_nav(-(int32_t)k_ls_scroll_page, false);
  }
  s_was_sw1 = d1;
  s_was_sw2 = d2;
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the panel, build the strip, run the reader loop.
 *
 * @return Never returns.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The panel shows the reader screen and the input loop runs forever.
 * @since 0.1.0
 */
int32_t main(void)
{
  app_bringup_clocks();
  ls_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);
  app_bringup_panel();
  app_bringup_gfx();
  app_bringup_touch();
  (void)ra8_board_sw_init(k_ra8_board_sw1);
  (void)ra8_board_sw_init(k_ra8_board_sw2);

  const uint32_t total = ls_build_atlas();
  ls_open_strip(total);

  ls_redraw();
  ls_present();
  ls_emit_banner();

  while (1) {
    ls_poll_touch();
    ls_poll_buttons();
    if ((g_ls_loop_ticks % (uint32_t)k_ls_led_every) == 0U) {
      (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    }
    g_ls_loop_ticks++;
    ra8_delay_ms((uint32_t)k_ls_frame_ms);
  }
}
#pragma GCC diagnostic pop
