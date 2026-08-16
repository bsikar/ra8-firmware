/**
 * @file examples/ek_ra8d2/hw_pending/ereader_manga/main.c
 * @brief Viewable manga reader: pan/zoom a page larger than the panel (Demo B).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * The viewable counterpart of the #231 tile pipeline. Where the old headless
 * gate rendered a 160x120 crop into an internal buffer, this app brings up the
 * real 1024x600 GLCDC panel and lets the user pan and zoom a page far bigger
 * than the screen:
 *
 *   1. Boot clocks / MSTP / SysTick / LEDs, then SDRAM + the GLCDC panel
 *      (``ra8_display_pal`` with ``k_display_backend_lcd_ra8_glcdc``), then
 *      bind ``ra8_gfx`` to the 1024x600 RGB565 framebuffer in SDRAM.
 *   2. Transcode the baked 1536x2048 grayscale PNG page
 *      (``mg_page_fixture.h``) into a JOF tile atlas with
 *      ``ra8_jof_produce`` -- one bounded band at a time, the decoded
 *      page never resident whole -- into an SDRAM memstore.
 *   3. Serve the atlas through an ``ra8_tile_cache`` sized (::k_mg_cells) to
 *      the 1:1 viewport tile demand plus a one-tile pan margin, so panning
 *      evicts only newly exposed tiles instead of thrashing (#338).
 *   4. Main loop: poll the GT911, hit-test the tap-zones (four edges pan, the
 *      centre toggles 1:1 <-> fit-page), redraw the viewport + status bar +
 *      minimap, and present.
 *
 * A deterministic banner is printed once at boot over the SCI console so the
 * headless EIL gate still asserts a fixed render:
 *
 *   ``ereader-manga: page 1536x2048 tiles=48 atlas=<N> view=0,0 zoom=1:1
 *   crc=<8hex> ok``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mg_page_fixture.h"
#include "mg_reader.h"
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
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_tile_cache.h"
#include "ra8_time.h"
#include "ra8_touch.h"

/**
 * @enum mg_boot_t
 * @brief Boot / console / touch knobs.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_uart_baud      = 115200U,   /**< Console baud.                     */
  k_mg_settle_ms      = 500U,      /**< PLL / SDRAM / panel-POR settle.   */
  k_mg_frame_ms       = 25U,       /**< Input poll period, ms.            */
  k_mg_led_every      = 16U,       /**< Heartbeat LED toggle sub-cadence. */
  k_mg_touch_bus_hz   = 400000U,   /**< GT911 fast-mode I2C clock.        */
  k_mg_touch_pclka_hz = 60000000U, /**< IIC_B clock-source rate.          */
  k_mg_touch_addr_7b  = 0x5DU,     /**< GT911 default 7-bit address.      */
} mg_boot_t;

/**
 * @enum mg_atlas_t
 * @brief JOF produce + cache budget (page geometry rides in the fixture).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_tile_edge  = 256U,        /**< Import + cache tile edge, pixels. */
  k_mg_cap_edge   = 2048U,       /**< Produce budget cap, pixels.       */
  k_mg_cell_bytes = 256U * 256U, /**< One gray8 256x256 tile.           */
  /* ::k_mg_view_cols / ::k_mg_view_rows are the tile columns/rows one 1:1
   * viewport frame can straddle: ceil(extent / tile) tiles, plus one for the
   * viewport not being tile-aligned. The content area is the panel minus the
   * status bar (the render clips there). See the derivation note below. */
  k_mg_view_cols =
    (((uint32_t)k_panel_width_px + k_mg_tile_edge - 1U) / k_mg_tile_edge) + 1U, /**< 1:1 cols. */
  k_mg_view_rows =
    ((((uint32_t)k_panel_height_px - (uint32_t)k_mg_statusbar_h) + k_mg_tile_edge - 1U) /
     k_mg_tile_edge) +
    1U,                                                         /**< 1:1 content rows.            */
  k_mg_cells   = (k_mg_view_cols + 1U) * (k_mg_view_rows + 1U), /**< Frame + 1-tile pan margin.   */
  k_mg_buckets = 64U,                                           /**< Hash buckets (>= cells).     */
  k_mg_cell_budget_bytes = 4U * 1024U * 1024U,                  /**< SDRAM tile cache may claim.  */
  k_mg_scratch           = 96U * 1024U,                         /**< read_tile deflate staging.   */
  k_mg_work_bytes        = 3203832U,                            /**< SDRAM producer work arena.   */
  k_mg_store_bytes       = 4U * 1024U * 1024U,                  /**< SDRAM atlas memstore.        */
  k_mg_image_id          = 1U,                                  /**< Tile-cache key image id.     */
  k_mg_hex_nibbles       = 8U,                                  /**< Hex digits per 32-bit value. */
  k_mg_nibble_bits       = 4U,                                  /**< Bits per hex nibble.         */
  k_mg_nibble_mask       = 0x0FU,                               /**< Low-nibble mask.             */
  k_mg_dec_ten           = 10U,                                 /**< Decimal radix / hex split.   */
} mg_atlas_t;

/*
 * ::k_mg_cells is DERIVED from the panel + tile geometry, never hand-picked
 * (#338). The reader pages the atlas one tile at a time (get / blit / put), so
 * the cache does not need every on-screen tile pinned at once -- but it must
 * still be able to *hold* the tiles a frame touches, or a one-tile pan evicts
 * tiles that are still on screen and re-inflates them next frame. That is the
 * thrash: with a sub-frame budget every pan re-decodes ~a full screen of
 * 64 KiB DEFLATE tiles.
 *
 * At 1:1 a 1024x600 panel with a 48 px status bar shows a 1024x552 content
 * window over 256x256 tiles, straddling at most ::k_mg_view_cols x
 * ::k_mg_view_rows = 5 x 4 = 20 tiles. Sizing the cache to that frame plus one
 * tile of margin on each axis (::k_mg_cells = 6 x 5 = 30, ~1.9 MiB of the
 * 64 MB SDRAM) means the union of any single-step pan's before/after frames
 * stays resident, so a pan re-decodes only the newly exposed row/column, never
 * a tile still on screen. `tests/test_app_ereader_manga.c`
 * (test_manga_pan_no_thrash) measures the eviction/decode count for a
 * representative pan against the old 4-cell budget and asserts it collapses.
 *
 * Two compile-time tripwires keep the budget honest:
 *   - the first static_assert fails the build if ::k_mg_cells is ever set below
 *     the frame demand (a manual override, or a geometry change that outgrows
 *     it), the condition that reintroduces the thrash;
 *   - the second bounds the resident-tile arena by ::k_mg_cell_budget_bytes so
 *     the derivation cannot silently balloon the SDRAM working set.
 * The 4-cell budget lives on only as the explicit eviction-stress case in the
 * host test, which is what it was always right for.
 */
static_assert((uint64_t)k_mg_cells >= (uint64_t)k_mg_view_cols * (uint64_t)k_mg_view_rows,
              "tile cache is smaller than the 1:1 viewport tile demand: re-derive k_mg_cells from "
              "the panel + tile geometry so a single-step pan cannot thrash (#338)");
static_assert((uint64_t)k_mg_cells * (uint64_t)k_mg_cell_bytes <= (uint64_t)k_mg_cell_budget_bytes,
              "resident tile arena exceeds its SDRAM budget: the tile-cache derivation grew past "
              "k_mg_cell_budget_bytes");

/*
 * ::k_mg_work_bytes is DERIVED, never hand-picked: it is exactly
 * `ra8_jof_work_bytes(k_mg_cap_edge, k_mg_cap_edge, k_mg_tile_edge,
 * k_mg_tile_edge)` -- the producer's own worst-case arena requirement for the
 * cap this app advertises, taken at the format's maximum 4 bpp. A round number
 * here is how the advertised cap and the real capability drift apart: a 2 MiB
 * arena backs a cap of only 1016 px, so the app used to advertise 2048 and
 * fail-closed (`k_ra8_err_invalid_size`) on any in-budget source wider than
 * that. Three layers keep the two in agreement:
 *   - the static_assert below is a compile-time tripwire on the band term
 *     (`cap * tile_h * bpp_max`), a strict lower bound on the full
 *     requirement, so raising ::k_mg_cap_edge without re-deriving the arena
 *     fails the build;
 *   - `tests/test_app_ereader_manga.c` asserts the arena against the real
 *     `ra8_jof_work_bytes()` return, which is the exact and
 *     authoritative check, and runs in CI;
 *   - `mg_build_atlas()` re-checks the same call at boot, so a mismatch that
 *     somehow reaches silicon reports `FAIL atlas` instead of silently
 *     under-delivering the advertised cap.
 * The band term alone is a lower bound, not the whole requirement, so the
 * static_assert can only catch an under-sized arena, never certify a
 * sufficient one -- that is the test's and the boot check's job.
 */
static_assert((uint64_t)k_mg_work_bytes >=
                ((uint64_t)k_mg_cap_edge * (uint64_t)k_mg_tile_edge * (uint64_t)k_ra8_jof_bpp_max),
              "work arena is below the producer band term for the advertised cap: re-derive "
              "k_mg_work_bytes from ra8_jof_work_bytes() for k_mg_cap_edge");

/**
 * @enum mg_fb_t
 * @brief Framebuffer geometry (mirrors the EK-RA8D2 panel).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_mg_fb_w     = k_panel_width_px,  /**< Framebuffer width, pixels.   */
  k_mg_fb_h     = k_panel_height_px, /**< Framebuffer height, pixels.  */
  k_mg_fb_align = 64U,               /**< 64-byte AXI-burst alignment. */
} mg_fb_t;

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned.
 * @note Scanned out continuously by the GLCDC.
 * @warning Written by the reader render; do not alias.
 * @since 0.1.0
 */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_mg_fb_align)]] static uint16_t s_framebuffer[(size_t)k_mg_fb_h * (size_t)k_mg_fb_w];

/** @brief Producer work arena (SDRAM: the transcode working set). */
[[gnu::section(".sdram_data")]] static uint8_t s_work[k_mg_work_bytes];
/** @brief Atlas memstore backing (SDRAM: the produced JOF atlas). */
[[gnu::section(".sdram_data")]] static uint8_t s_store_buf[k_mg_store_bytes];
/** @brief Tile-cache cell storage (::k_mg_cells gray8 256x256 tiles, SDRAM). */
[[gnu::section(
  ".sdram_data")]] static uint8_t s_cell_mem[(size_t)k_mg_cells * (size_t)k_mg_cell_bytes];
/** @brief read_tile deflate stored-stream staging (SDRAM). */
[[gnu::section(".sdram_data")]] static uint8_t s_scratch[k_mg_scratch];

/** @brief Display PAL config -- LCD/GLCDC backend over the SDRAM framebuffer. */
static const display_cfg_t k_mg_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_mg_fb_w,
  .height_px         = (uint16_t)k_mg_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;
/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/** @brief The atlas memstore (sink + pread pair over s_store_buf). */
static ra8_jof_memstore_t s_store;
/** @brief Parsed atlas geometry after produce. */
static ra8_jof_info_t s_info;
/** @brief Tile-cache state paging the atlas. */
static ra8_tile_cache_t s_cache;
/** @brief Decode-on-miss context bound into the cache. */
static mg_tile_src_t s_tile_src;
/** @brief The pan/zoom viewer over the tiled page. */
static mg_reader_t s_reader;

/** @brief Tile-cache key storage (one per cell). */
static ra8_tile_key_t s_keys[k_mg_cells];
/** @brief Tile-cache dim descriptors (one per cell). */
static ra8_tile_dims_t s_dims[k_mg_cells];
/** @brief Tile-cache link metadata (one per cell). */
static ra8_keycache_cell_t s_meta[k_mg_cells];
/** @brief Tile-cache hash buckets. */
static int32_t s_buckets[k_mg_buckets];

/**
 * @struct mg_png_cursor_t
 * @brief Forward cursor over the baked PNG for the produce pull seam.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Baked PNG bytes.        */
  uint32_t       len;  /**< Total length.           */
  uint32_t       pos;  /**< Bytes delivered so far. */
} mg_png_cursor_t;

/** @brief PNG pull cursor for the produce source. */
static mg_png_cursor_t s_png_cursor;

/** @brief Bound I2C bus handle the touch driver's injected seam points at. */
static ra8_io_i2c_bus_t s_touch_bus;

/* Console banner fragments. */
static const uint8_t k_msg_boot[]  = "ereader-manga: boot\r\n";
static const uint8_t k_msg_fail[]  = "ereader-manga: FAIL init\r\n";
static const uint8_t k_msg_atl[]   = "ereader-manga: FAIL atlas\r\n";
static const uint8_t k_msg_rend[]  = "ereader-manga: FAIL render\r\n";
static const uint8_t k_msg_pre[]   = "ereader-manga: page ";
static const uint8_t k_msg_x[]     = "x";
static const uint8_t k_msg_tiles[] = " tiles=";
static const uint8_t k_msg_atlas[] = " atlas=";
static const uint8_t k_msg_view[]  = " view=0,0 zoom=1:1 crc=";
static const uint8_t k_msg_ok[]    = " ok\r\n";

/** @brief Emit a byte run on the console. */
static void mg_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a fail banner, then trap (ra8_emulator halts on the BKPT). */
static void mg_panic_halt(const uint8_t* msg, uint32_t len)
{
  mg_print(msg, len);
  (void)ra8_board_led_on(k_ra8_board_led_red);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void mg_print_hex(uint32_t value)
{
  uint8_t buf[k_mg_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_mg_hex_nibbles; ++i) {
    const uint32_t shift = ((uint32_t)k_mg_hex_nibbles - 1U - i) * (uint32_t)k_mg_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_mg_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_mg_dec_ten) ? ('0' + nib) : ('A' + (nib - k_mg_dec_ten)));
  }
  mg_print(buf, (uint32_t)k_mg_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void mg_print_uint(uint32_t value)
{
  uint8_t  buf[k_mg_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_mg_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_mg_dec_ten));
    n++;
    value /= (uint32_t)k_mg_dec_ten;
  }
  for (uint32_t i = 0U; i < n; ++i) {
    mg_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Boot helpers
 * =========================================================================== */

/** @brief Bring up clocks, MSTP, system tick, LEDs and the SCI console. */
static void mg_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) ||
      (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok)) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_mg_uart_baud) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  ra8_isr_globals_enable();
}

/** @brief Bring up SDRAM + the GLCDC panel, then bind ra8_gfx to the FB. */
static void mg_bringup_panel(void)
{
  ra8_delay_ms((uint32_t)k_mg_settle_ms);
  if (ra8_sdramc_init() != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (display_init(&k_mg_display_cfg, &s_display) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Open the GT911 touch controller (best-effort, polled). */
static void mg_bringup_touch(void)
{
  const ra8_i3c_cfg_t iic_cfg = {.mode     = k_ra8_i3c_mode_i2c,
                                 .bus_hz   = (uint32_t)k_mg_touch_bus_hz,
                                 .pclka_hz = (uint32_t)k_mg_touch_pclka_hz};
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
                               .target_7b  = (uint8_t)k_mg_touch_addr_7b,
                               .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
                               .max_points = 1U};
  (void)ra8_touch_open(&cfg);
}

/* ===========================================================================
 * Atlas produce + cache + reader setup
 * =========================================================================== */

/** @brief produce pull seam: stream the baked PNG bytes to the transcoder. */
static ra8_err_t mg_png_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  mg_png_cursor_t* c     = (mg_png_cursor_t*)ctx;
  const uint32_t   avail = c->len - c->pos;
  size_t           take  = ((uint32_t)cap < avail) ? cap : (size_t)avail;
  (void)memcpy(buf, &c->data[c->pos], take);
  c->pos += (uint32_t)take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Transcode the baked page into a JOF atlas + parse its geometry. */
static bool mg_build_atlas(void)
{
  /* The advertised cap must be one this arena can actually serve. The producer
   * carves from the *decoded* geometry, so an under-sized arena would not show
   * up until some in-budget source happened to be wide enough or deep enough in
   * bpp to exhaust it; asking the producer for its own requirement up front
   * turns that latent capability lie into a deterministic boot failure. */
  const uint32_t need = ra8_jof_work_bytes((uint16_t)k_mg_cap_edge,
                                           (uint16_t)k_mg_cap_edge,
                                           (uint16_t)k_mg_tile_edge,
                                           (uint16_t)k_mg_tile_edge);
  if ((need == 0U) || (need > (uint32_t)sizeof(s_work))) {
    return false;
  }
  s_png_cursor = (mg_png_cursor_t){.data = k_mg_png, .len = k_mg_png_len, .pos = 0U};
  s_store      = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull     = mg_png_pull,
    .pull_ctx = &s_png_cursor,
    .sink     = ra8_jof_memstore_sink,
    .sink_ctx = &s_store,
    .tile_w   = (uint16_t)k_mg_tile_edge,
    .tile_h   = (uint16_t)k_mg_tile_edge,
    /* Deflate tiles (codec 1): each tile is an intra-coded raw-DEFLATE stream,
     * so the 3 MiB page normalizes to a ~20 KiB atlas and every cache miss does
     * real decode-on-demand work (one bounded inflate into the pinned cell) --
     * the streaming-larger-than-RAM property the tile cache exists for. On the
     * 1 GHz M85 a 64 KiB-tile inflate is microseconds; the cost only shows under
     * the ra8_emulator CPU emulator, where boot still completes well inside budget. */
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_mg_cap_edge,
    .max_height = (uint16_t)k_mg_cap_edge,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .webp_work  = nullptr,
  };
  if (ra8_jof_produce(&cfg, &s_info) != k_ra8_ok) {
    return false;
  }
  return ra8_jof_parse(ra8_jof_memstore_pread, &s_store, s_store.len, &s_info) == k_ra8_ok;
}

/** @brief Wire the tile cache (decode-on-miss = read_tile) over the atlas. */
static bool mg_setup_cache(void)
{
  s_tile_src = (mg_tile_src_t){.info        = &s_info,
                               .pread       = ra8_jof_memstore_pread,
                               .pread_ctx   = &s_store,
                               .scratch     = s_scratch,
                               .scratch_cap = (uint32_t)sizeof(s_scratch)};
  const ra8_tile_cache_cfg_t cache_cfg = {.cell_mem     = s_cell_mem,
                                          .cell_bytes   = (uint32_t)k_mg_cell_bytes,
                                          .cell_count   = (uint32_t)k_mg_cells,
                                          .meta         = s_meta,
                                          .keys         = s_keys,
                                          .dims         = s_dims,
                                          .buckets      = s_buckets,
                                          .bucket_count = (uint32_t)k_mg_buckets,
                                          .decode       = mg_tile_decode,
                                          .decode_ctx   = &s_tile_src};
  return ra8_tile_cache_init(&s_cache, &cache_cfg) == k_ra8_ok;
}

/** @brief Present the framebuffer (clean full update). */
static void mg_present(void)
{
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_quality);
}

/** @brief Print the deterministic boot banner (geometry + render hash). */
static void mg_print_banner(void)
{
  mg_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  mg_print_uint((uint32_t)s_info.width);
  mg_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  mg_print_uint((uint32_t)s_info.height);
  mg_print(k_msg_tiles, (uint32_t)sizeof(k_msg_tiles) - 1U);
  mg_print_uint(s_info.tile_count);
  mg_print(k_msg_atlas, (uint32_t)sizeof(k_msg_atlas) - 1U);
  mg_print_uint(s_info.total_size);
  mg_print(k_msg_view, (uint32_t)sizeof(k_msg_view) - 1U);
  mg_print_hex(mg_fnv1a(s_framebuffer, (uint32_t)sizeof(s_framebuffer)));
  mg_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);
}

/** @brief Poll the GT911 once; on a fresh tap, mutate + redraw the viewport. */
static void mg_poll_touch(bool* was_touching)
{
  ra8_touch_point_t pt  = {};
  uint8_t           got = 0U;
  if (ra8_touch_read(&pt, 1U, &got) != k_ra8_ok) {
    return;
  }
  const bool touching = (got > 0U);
  if (touching && !(*was_touching)) {
    if (mg_reader_tap(&s_reader, (int32_t)pt.x, (int32_t)pt.y)) {
      if (mg_reader_render(&s_reader) == k_ra8_ok) {
        mg_present();
        /* Idle-window read-ahead: warm the next pan step's tiles so a follow-on
         * pan finds them resident instead of stalling on a cold decode (#341). */
        (void)mg_reader_prefetch(&s_reader);
      }
    }
  }
  *was_touching = touching;
}

/**
 * @brief App entry: panel bring-up -> atlas -> tile cache -> pan/zoom loop.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The boot banner is emitted; the reader loop services taps forever.
 * @since 0.1.0
 */
void main(void)
{
  mg_bringup_clocks();
  mg_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);
  mg_bringup_panel();
  mg_bringup_touch();

  if (!mg_build_atlas()) {
    mg_panic_halt(k_msg_atl, (uint32_t)sizeof(k_msg_atl) - 1U);
  }
  if (!mg_setup_cache()) {
    mg_panic_halt(k_msg_atl, (uint32_t)sizeof(k_msg_atl) - 1U);
  }
  const mg_reader_cfg_t rcfg = {.fb       = s_framebuffer,
                                .fb_w     = (int32_t)k_mg_fb_w,
                                .fb_h     = (int32_t)k_mg_fb_h,
                                .cache    = &s_cache,
                                .info     = &s_info,
                                .image_id = (uint32_t)k_mg_image_id};
  if (mg_reader_init(&s_reader, &rcfg) != k_ra8_ok) {
    mg_panic_halt(k_msg_atl, (uint32_t)sizeof(k_msg_atl) - 1U);
  }
  if (mg_reader_render(&s_reader) != k_ra8_ok) {
    mg_panic_halt(k_msg_rend, (uint32_t)sizeof(k_msg_rend) - 1U);
  }
  mg_present();
  mg_print_banner();

  bool     was_touching = false;
  uint32_t ticks        = 0U;
  while (1) {
    mg_poll_touch(&was_touching);
    if ((ticks % (uint32_t)k_mg_led_every) == 0U) {
      (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    }
    ticks++;
    ra8_delay_ms((uint32_t)k_mg_frame_ms);
  }
}
