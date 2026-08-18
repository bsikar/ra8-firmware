/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/main.c
 * @brief Hybrid baked + SD e-reader: shelf, cover, TOC, full-book reader.
 *
 * @details
 * The front end of the compiled-book pipeline on the EK-RA8D2 parallel TFT.
 * Books come from two sources tested side by side in one app: a few baked into
 * MRAM (library.h, chunked RBKC containers) and the rest read from a FAT SD
 * card. Either way `.rabook` books are demand-paged: sh_paged.c binds the
 * chunked reader + ra8_vmem page cache and single chunks inflate into cache
 * frames as they are touched (never the whole book -- see #204/#205). The same
 * source-agnostic screens render everything:
 *
 *   shelf (cover-thumbnail grid) -> cover/title page -> table of contents ->
 *   reader (every chapter, paginated, page-turn crosses chapter boundaries).
 *
 * This file owns boot (clocks, console, panel, touch, optional SD), the single
 * ::g_sh state, the miniz inflate callback, and the input loop that polls the
 * GT911 panel + SW1/SW2 and dispatches to the active screen. Under ra8_emulator,
 * `--click X Y` drives it, `--sd img` attaches the card, and `--ppm` captures
 * a frame.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>

#include "miniz.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_touch.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "ra8_touch.h"
#include "ra8_wdt.h"
#include "sh_app.h"

/** @enum sh_main_const_t @brief Boot + banner formatting constants. */
typedef enum : uint32_t {
  k_sh_thirds      = 3U,          /**< Reader edge-tap split.              */
  k_sh_demo_steps  = 8U,          /**< Idle-demo sequence length.          */
  k_sh_demo_period = 30U,         /**< Input polls between demo steps.     */
  k_sh_idle_dim_ms = 30000U,      /**< No-input time before backlight off. */
  k_sh_fnv_offset  = 2166136261U, /**< FNV-1a 32-bit offset basis.         */
  k_sh_fnv_prime   = 16777619U,   /**< FNV-1a 32-bit prime.                */
  k_sh_hex_digits  = 8U,          /**< Hex digits in the framebuffer hash. */
  k_sh_nib_bits    = 4U,          /**< Bits per hex digit.                 */
  k_sh_nib_mask    = 0xFU,        /**< Low-nibble mask.                    */
} sh_main_const_t;

/** @brief The single whole-app state instance. */
sh_state_t g_sh;

/** @brief 1024x600 RGB565 framebuffer in external SDRAM (GLCDC scans this). */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_sh_fb_align)]] static uint16_t s_framebuffer[(size_t)k_sh_fb_h * (size_t)k_sh_fb_w];

static const display_cfg_t k_sh_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_sh_fb_w,
  .height_px         = (uint16_t)k_sh_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};
static display_handle_t* s_display;

static const uint8_t k_msg_fail[] = "ereader-shelf: FAIL init\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void sh_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief FNV-1a hash of the whole framebuffer (deterministic render digest). */
static uint32_t sh_fb_hash(void)
{
  uint32_t       h     = (uint32_t)k_sh_fnv_offset;
  const uint8_t* p     = (const uint8_t*)s_framebuffer;
  const size_t   bytes = sizeof s_framebuffer;
  for (size_t i = 0U; i < bytes; ++i) {
    h = (h ^ p[i]) * (uint32_t)k_sh_fnv_prime;
  }
  return h;
}

/**
 * @brief Emit the gate banner: book count, SD flag, framebuffer hash, comic digests.
 * @details The `fb=` hash digests the rendered SHELF (unchanged by this feature),
 *          so the ra8_emulator uart_scrape gate still catches cover-decode / layout
 *          regressions. The appended `cbz=`/`cbr=`/`rtl=` fields pin the newly
 *          integrated comic decode + RTL path (#236) -- a deterministic,
 *          toolchain-independent digest of page 0 of the baked CBZ + CBR
 *          fixtures decoded through the shelf's own ::ra8_comic + image pipeline.
 * @param[in] cbz    CBZ page-0 self-check result.
 * @param[in] cbr    CBR page-0 self-check result.
 * @param[in] rtl_ok RTL edge-mapping verdict.
 */
static void sh_print_banner(const sh_comic_probe_t* cbz, const sh_comic_probe_t* cbr, bool rtl_ok)
{
  char        b[k_sh_linebuf];
  size_t      p   = 0U;
  const char* pre = "ereader-shelf: books=";
  for (const char* s = pre; *s != '\0'; ++s) {
    b[p++] = *s;
  }
  p               = sh_fmt_uint(b, p, g_sh.book_count);
  const char* mid = " sd=";
  for (const char* s = mid; *s != '\0'; ++s) {
    b[p++] = *s;
  }
  p               = sh_fmt_uint(b, p, g_sh.sd_ready ? 1U : 0U);
  const char* fbp = " fb=";
  for (const char* s = fbp; *s != '\0'; ++s) {
    b[p++] = *s;
  }
  const uint32_t h = sh_fb_hash();
  for (uint32_t d = 0U; d < (uint32_t)k_sh_hex_digits; ++d) {
    const uint32_t nib =
      (h >> (((uint32_t)k_sh_hex_digits - 1U - d) * (uint32_t)k_sh_nib_bits)) & k_sh_nib_mask;
    b[p++] = (char)((nib < k_sh_dec_base) ? ('0' + nib) : ('A' + (nib - (uint32_t)k_sh_dec_base)));
  }
  sh_comic_append_banner(b, &p, cbz, cbr, rtl_ok);
  const char* end = " ok\r\n";
  for (const char* s = end; *s != '\0'; ++s) {
    b[p++] = *s;
  }
  sh_print((const uint8_t*)b, (uint32_t)p);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void sh_panic_halt(void)
{
  sh_print(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Static DEFLATE decompressor state (~11 KiB) kept off the small stack.
 * @note Single-threaded use only; reset by tinfl_init() on every inflate.
 */
static tinfl_decompressor s_tinfl;

ra8_err_t sh_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  tinfl_init(&s_tinfl);
  size_t             in_n  = src_len;
  size_t             out_n = dst_cap;
  const tinfl_status st    = tinfl_decompress(
    &s_tinfl,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (st != TINFL_STATUS_DONE) {
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

uint16_t* sh_fb_pixels(void)
{
  return s_framebuffer;
}

/** @brief Seed the shelf with the MRAM-baked library books. */
static void sh_seed_baked(void)
{
  g_sh.book_count = 0U;
  for (uint16_t i = 0U;
       (i < (uint16_t)k_library_count) && (g_sh.book_count < (uint16_t)k_sh_max_books);
       ++i) {
    sh_entry_t* e = &g_sh.entry[g_sh.book_count];
    e->from_sd    = false;
    e->blob       = k_library[i].blob;
    e->blob_len   = k_library[i].len;
    e->thumb      = k_library[i].thumb;
    e->thumb_w    = k_library[i].thumb_w;
    e->thumb_h    = k_library[i].thumb_h;
    (void)strncpy(e->title, k_library[i].title, sizeof e->title - 1U);
    e->title[sizeof e->title - 1U] = '\0';
    (void)strncpy(e->author, k_library[i].author, sizeof e->author - 1U);
    e->author[sizeof e->author - 1U] = '\0';
    g_sh.book_count++;
  }
}

/** @brief Centre a string horizontally for the bitmap font. */
static int32_t sh_center_x(const char* s)
{
  return ((int32_t)k_sh_fb_w - ((int32_t)strlen(s) * (int32_t)k_sh_glyph_w)) / 2;
}

/**
 * @brief Paint a "Loading from SD card..." panel before a (blocking) SD open.
 * @details The chunk reads + inflates behind an SD open are synchronous; over
 *          ra8_emulator's byte-emulated SPI the first faults (header, metadata,
 *          cover chunks) take a while, so this gives immediate feedback
 *          instead of a frozen-looking shelf. On hardware the reads are
 *          instant.
 */
static void sh_loading_overlay(const char* name)
{
  (void)ra8_gfx_clear((uint32_t)k_sh_col_bg);
  const int32_t my = ((int32_t)k_sh_fb_h / 2) - (int32_t)k_sh_glyph_h;
  (void)ra8_gfx_text_out(sh_center_x("Loading from SD card..."),
                         my,
                         "Loading from SD card...",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_card,
                         (uint32_t)k_sh_col_bg);
  (void)ra8_gfx_text_out(sh_center_x(name),
                         my + (int32_t)k_sh_line_h,
                         name,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_sub,
                         (uint32_t)k_sh_col_bg);
  const display_rect_t full = {.x = 0U,
                               .y = 0U,
                               .w = (uint16_t)k_sh_fb_w,
                               .h = (uint16_t)k_sh_fb_h};
  (void)display_flush(s_display, full, k_display_refresh_quality);
}

/** @brief Open the selected book and move to the cover screen; ignore on failure. */
static bool sh_select_book(uint16_t idx)
{
  if (g_sh.entry[idx].from_sd) {
    sh_loading_overlay(g_sh.entry[idx].title);
  }
  if (!sh_book_open(idx)) {
    return false;
  }
  g_sh.selected   = idx;
  g_sh.toc_scroll = 0;
  /* Comics open straight into the full-page image reader; text books land on
   * the cover / title page as before. */
  g_sh.screen = sh_fmt_is_comic(g_sh.open_fmt) ? k_sh_screen_comic : k_sh_screen_cover;
  return true;
}

/** @brief Reader edge-tap to direction: -1 left third, +1 right third, else 0. */
static int32_t sh_edge_dir(int32_t x)
{
  const int32_t third = (int32_t)k_sh_fb_w / (int32_t)k_sh_thirds;
  if (x < third) {
    return -1;
  }
  if (x >= ((int32_t)k_sh_fb_w - third)) {
    return 1;
  }
  return 0;
}

/** @brief Start the reader at the book's first prose chapter (past front matter). */
static void sh_start_reading(void)
{
  sh_reader_load_chapter(sh_reader_first_content());
  g_sh.screen = k_sh_screen_reader;
}

/** @brief Handle a cover-screen tap; returns whether to repaint. */
static bool sh_tap_cover(int32_t x, int32_t y, bool header)
{
  if (header) {
    g_sh.screen = k_sh_screen_shelf;
    return true;
  }
  const sh_cover_act_t act = sh_cover_action(x, y);
  if (act == k_sh_cover_read) {
    sh_start_reading();
    return true;
  }
  if (act == k_sh_cover_toc) {
    g_sh.screen = k_sh_screen_toc;
    return true;
  }
  return false;
}

/** @brief Handle a TOC-screen tap; returns whether to repaint. */
static bool sh_tap_toc(int32_t x, int32_t y, bool header)
{
  if (header) {
    g_sh.screen = k_sh_screen_cover;
    return true;
  }
  const int32_t ci = sh_toc_hit(x, y);
  if (ci < 0) {
    return false;
  }
  sh_reader_load_chapter((uint32_t)ci);
  g_sh.screen = k_sh_screen_reader;
  return true;
}

/** @brief Handle a reader-screen tap (header back / edge page turn). */
static bool sh_tap_reader(int32_t x, int32_t y, bool header)
{
  (void)y;
  if (header) {
    g_sh.screen = k_sh_screen_toc;
    return true;
  }
  const int32_t dir = sh_edge_dir(x);
  return (dir != 0) && sh_reader_turn(dir);
}

/** @brief Route a touch tap to the active screen; returns whether to repaint. */
static bool sh_handle_tap(int32_t x, int32_t y)
{
  const bool header = (y < (int32_t)k_sh_bar_h);
  switch (g_sh.screen) {
    case k_sh_screen_cover:
      return sh_tap_cover(x, y, header);
    case k_sh_screen_toc:
      return sh_tap_toc(x, y, header);
    case k_sh_screen_reader:
      return sh_tap_reader(x, y, header);
    case k_sh_screen_comic:
      return sh_comic_tap(x, header);
    case k_sh_screen_shelf:
    default: {
      const int32_t card = sh_shelf_hit(x, y);
      return (card >= 0) && sh_select_book((uint16_t)card);
    }
  }
}

/**
 * @brief Held-finger step of the cover loupe gesture: pan an open lens, or open
 *        one once the press has been held for ::k_sh_loupe_hold_ms.
 * @param[in]  x             Current touch X in framebuffer pixels.
 * @param[in]  y             Current touch Y in framebuffer pixels.
 * @param[out] out_lens_only Set true when only the fixed lens window changed.
 * @return true if the screen must be re-presented.
 */
static bool sh_loupe_held(int32_t x, int32_t y, bool* out_lens_only)
{
  if (g_sh.loupe_active) {
    int32_t cx = g_sh.loupe_cx;
    int32_t cy = g_sh.loupe_cy;
    if (sh_cover_loupe_map(x, y, &cx, &cy)) {
      g_sh.loupe_cx = cx;
      g_sh.loupe_cy = cy;
    }
    *out_lens_only = true; /* lens is a fixed window; only its contents pan */
    return true;
  }
  if ((ra8_time_ms() - g_sh.touch_down_ms) < (uint32_t)k_sh_loupe_hold_ms) {
    return false; /* not held long enough yet */
  }
  int32_t cx = 0;
  int32_t cy = 0;
  if (!sh_cover_loupe_map(g_sh.touch_down_x, g_sh.touch_down_y, &cx, &cy)) {
    return false; /* press did not start over a magnifiable cover image */
  }
  g_sh.loupe_active = true;
  g_sh.loupe_cx     = cx;
  g_sh.loupe_cy     = cy;
  return true;
}

/**
 * @brief Cover-screen press-hold loupe gesture: a hold opens the magnifier, drag
 *        pans it, release exits; a short press falls through to the cover tap.
 * @details Deferring the cover tap to release is what tells a long press apart
 *          from a tap without ever firing both; the reader's page-turn thirds are
 *          a different screen and keep their unchanged tap-on-press path.
 * @param[in]  down          true if a finger is currently down.
 * @param[in]  x             Current touch X (press-start X when released).
 * @param[in]  y             Current touch Y (press-start Y when released).
 * @param[in]  was_down      true if a finger was down on the previous poll.
 * @param[out] out_lens_only Receives true when only the lens window needs a repaint.
 * @return true if the screen must be re-presented.
 */
static bool sh_cover_gesture(bool down, int32_t x, int32_t y, bool was_down, bool* out_lens_only)
{
  *out_lens_only = false;
  if (down && !was_down) {
    g_sh.touch_down_ms = ra8_time_ms();
    g_sh.touch_down_x  = x;
    g_sh.touch_down_y  = y;
    return false; /* defer: could still become a tap or a hold */
  }
  if (down) {
    return sh_loupe_held(x, y, out_lens_only);
  }
  if (was_down && g_sh.loupe_active) {
    g_sh.loupe_active = false;
    return true; /* release closes the lens */
  }
  if (was_down) {
    return sh_tap_cover(g_sh.touch_down_x,
                        g_sh.touch_down_y,
                        g_sh.touch_down_y < (int32_t)k_sh_bar_h);
  }
  return false; /* idle: no finger down this poll or last */
}

/** @brief Route an SW1/SW2 edge to the active screen; returns whether to repaint. */
static bool sh_handle_button(bool is_sw2)
{
  switch (g_sh.screen) {
    case k_sh_screen_shelf:
      if (g_sh.book_count == 0U) {
        return false;
      }
      if (is_sw2) {
        return sh_select_book(g_sh.selected);
      }
      g_sh.selected = (uint16_t)((g_sh.selected + g_sh.book_count - 1U) % g_sh.book_count);
      return true;
    case k_sh_screen_cover:
      if (is_sw2) {
        sh_start_reading();
        return true;
      }
      g_sh.screen = k_sh_screen_shelf;
      return true;
    case k_sh_screen_toc:
      sh_toc_scroll(is_sw2 ? 1 : -1);
      return true;
    case k_sh_screen_comic:
      /* SW2 = next page, SW1 = previous page (in the active reading order). */
      return sh_comic_turn(is_sw2 ? 1 : -1);
    case k_sh_screen_reader:
    default:
      return sh_reader_turn(is_sw2 ? 1 : -1);
  }
}

/** @brief Re-render the active screen and push it to the panel. */
static void sh_present(void)
{
  switch (g_sh.screen) {
    case k_sh_screen_cover:
      sh_cover_render();
      if (g_sh.loupe_active) {
        sh_cover_loupe_render();
      }
      break;
    case k_sh_screen_toc:
      sh_toc_render();
      break;
    case k_sh_screen_reader:
      sh_reader_render();
      break;
    case k_sh_screen_comic:
      sh_comic_render();
      break;
    case k_sh_screen_shelf:
    default:
      sh_shelf_render();
      break;
  }
  const display_rect_t full = {.x = 0U,
                               .y = 0U,
                               .w = (uint16_t)k_sh_fb_w,
                               .h = (uint16_t)k_sh_fb_h};
  (void)display_flush(s_display, full, k_display_refresh_quality);
  /* Flush issued: spend the panel-refresh idle window (before the next input
   * poll) warming the adjacent chapters' first content frames so a
   * chapter-crossing page turn finds them resident (#207). Reader screen only,
   * so it never runs before the boot banner; best-effort and output-transparent. */
  if (g_sh.screen == k_sh_screen_reader) {
    sh_reader_prefetch_adjacent();
  }
}

/**
 * @brief Repaint only the fixed lens window during a loupe pan and flush just it.
 * @details The lens is a fixed on-screen rectangle; panning changes only the
 *          sampled source sub-rect, not the lens position, so the cover beneath
 *          is unchanged and never re-decoded -- one bounded window read per frame.
 */
static void sh_present_loupe(void)
{
  sh_cover_loupe_render();
  const display_rect_t lens = {.x = (uint16_t)(((int32_t)k_sh_fb_w - (int32_t)k_sh_loupe_w) / 2),
                               .y = (uint16_t)(((int32_t)k_sh_fb_h - (int32_t)k_sh_loupe_h) / 2),
                               .w = (uint16_t)k_sh_loupe_w,
                               .h = (uint16_t)k_sh_loupe_h};
  (void)display_flush(s_display, lens, k_display_refresh_quality);
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void sh_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    sh_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sh_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sh_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sh_uart_baud) != k_ra8_ok) {
    sh_panic_halt();
  }
}

/** @brief SDRAM + GLCDC panel bring-up; bind ra8_gfx to the live framebuffer. */
static void sh_panel_or_halt(void)
{
  display_fb_t fb = {};
  if ((ra8_sdramc_init() != k_ra8_ok) ||
      (display_init(&k_sh_display_cfg, &s_display) != k_ra8_ok) ||
      (display_get_framebuffer(s_display, &fb) != k_ra8_ok)) {
    sh_panic_halt();
  }
  if (ra8_gfx_init(fb.pixels, (uint16_t)k_sh_fb_w, (uint16_t)k_sh_fb_h, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    sh_panic_halt();
  }
}

/**
 * @brief Arm WDT0 to reset a wedged reader loop; fatal on failure.
 * @details Configures the M85 WWDT with the longest available count
 *          (::k_ra8_wdt_timeout_16384 at PCLKB/8192, on the order of a second)
 *          and no refresh window (::k_ra8_wdt_window_start_100 /
 *          ::k_ra8_wdt_window_end_0) so a heartbeat is legal on any loop
 *          iteration. ::k_ra8_wdt_sleep_keep_count keeps the counter running
 *          through the loop's ::ra8_delay_ms WFI naps, so a spin that never
 *          leaves WFI is still caught, and expiry drives an internal reset to
 *          reboot a hung reader. Armed once, immediately before the superloop,
 *          so the bounded boot (panel + SD + first render) never risks a
 *          spurious reset; the first ::ra8_wdt_refresh_for lands on the next
 *          iteration. A bring-up failure is fatal because a reader running
 *          without its declared watchdog is a worse state than a clean halt.
 * @return Nothing.
 * @pre ::sh_setup_or_halt has run, so PCLKB (the WWDT count clock) is live.
 * @pre Called exactly once (WWDT control registers are write-once after reset).
 * @post WDT0 is counting; a missed refresh triggers an internal reset.
 * @post Control returns only on success; otherwise the CPU is parked.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void sh_wdt_arm_or_halt(void)
{
  const ra8_wdt_cfg_t cfg = {
    .timeout       = k_ra8_wdt_timeout_16384,    /* longest count ...       */
    .clock_div     = k_ra8_wdt_clkdiv_8192,      /* ... x longest divisor.  */
    .window_start  = k_ra8_wdt_window_start_100, /* no upper bound.         */
    .window_end    = k_ra8_wdt_window_end_0,     /* no lower bound.         */
    .on_expiry     = k_ra8_wdt_on_expiry_reset,  /* reboot a hung reader.   */
    .stop_in_sleep = k_ra8_wdt_sleep_keep_count, /* count through WFI naps. */
  };
  if (ra8_wdt_init(&cfg) != k_ra8_ok) {
    sh_panic_halt();
  }
}

/** @brief Open touch + the two user buttons (best-effort; input is optional). */
static void sh_input_init(void)
{
  const ra8_board_touch_cfg_t cfg = {.max_points = (uint8_t)k_sh_poll_pts,
                                     .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset};
  (void)ra8_board_touch_open(&cfg);
  (void)ra8_board_sw_init(k_ra8_board_sw1);
  (void)ra8_board_sw_init(k_ra8_board_sw2);
}

/** @brief Poll touch + buttons once; re-present on any change. Returns user-acted. */
static bool
sh_pump_input(uint8_t* prev_touch, ra8_board_sw_state_t* prev1, ra8_board_sw_state_t* prev2)
{
  bool changed   = false;
  bool acted     = false;
  bool lens_only = false;

  ra8_touch_point_t pts[k_sh_poll_pts] = {};
  uint8_t           got                = 0U;
  if (ra8_touch_read(pts, (uint8_t)k_sh_poll_pts, &got) == k_ra8_ok) {
    const bool down     = (got > 0U);
    const bool was_down = (*prev_touch > 0U);
    /* The cover screen defers its tap so a press-hold can open the loupe; every
     * other screen keeps the unchanged tap-on-press path (page-turn thirds). */
    if (g_sh.screen == k_sh_screen_cover) {
      const int32_t x = down ? (int32_t)pts[0].x : g_sh.touch_down_x;
      const int32_t y = down ? (int32_t)pts[0].y : g_sh.touch_down_y;
      changed         = sh_cover_gesture(down, x, y, was_down, &lens_only);
      acted           = down || was_down;
    } else if (down && !was_down) {
      acted   = true;
      changed = sh_handle_tap((int32_t)pts[0].x, (int32_t)pts[0].y);
    }
    *prev_touch = got;
  }

  ra8_board_sw_state_t s1 = k_ra8_board_sw_released;
  ra8_board_sw_state_t s2 = k_ra8_board_sw_released;
  if ((ra8_board_sw_read(k_ra8_board_sw1, &s1) == k_ra8_ok) && (s1 == k_ra8_board_sw_pressed) &&
      (*prev1 == k_ra8_board_sw_released)) {
    acted   = true;
    changed = sh_handle_button(false) || changed;
  }
  if ((ra8_board_sw_read(k_ra8_board_sw2, &s2) == k_ra8_ok) && (s2 == k_ra8_board_sw_pressed) &&
      (*prev2 == k_ra8_board_sw_released)) {
    acted   = true;
    changed = sh_handle_button(true) || changed;
  }
  *prev1 = s1;
  *prev2 = s2;

  if (changed) {
    if (lens_only) {
      sh_present_loupe();
    } else {
      sh_present();
    }
  }
  return acted;
}

/** @enum sh_demo_t @brief Idle-demo step indices (one screen transition each). */
typedef enum : uint32_t {
  k_sh_demo_cover0 = 0U, /**< Open book 0 on its cover. */
  k_sh_demo_toc0   = 1U, /**< Book 0 table of contents. */
  k_sh_demo_read0  = 2U, /**< Read book 0.              */
  k_sh_demo_turn   = 3U, /**< Turn one page.            */
  k_sh_demo_cover1 = 4U, /**< Open book 1 cover.        */
  k_sh_demo_toc1   = 5U, /**< Book 1 table of contents. */
  k_sh_demo_cover2 = 6U, /**< Open book 2 cover.        */
  k_sh_demo_read2  = 7U, /**< Read book 2.              */
} sh_demo_t;

/**
 * @brief Advance the idle self-demo one step (shelf -> cover -> TOC -> read ...).
 * @details Runs only until the user touches the panel; lets a headless
 *          ra8_emulator --record walk every screen with no input. Wraps to the
 *          shelf so the loop is closed.
 */
static void sh_demo_step(uint32_t step)
{
  if (g_sh.book_count == 0U) {
    return; /* nothing to demo; also guards the % book_count below */
  }
  switch ((sh_demo_t)(step % k_sh_demo_steps)) {
    case k_sh_demo_cover0:
      (void)sh_select_book(0U);
      break;
    case k_sh_demo_toc0:
      g_sh.screen = k_sh_screen_toc;
      break;
    case k_sh_demo_read0:
      sh_start_reading();
      break;
    case k_sh_demo_turn:
      (void)sh_reader_turn(1);
      break;
    case k_sh_demo_cover1:
      (void)sh_select_book((uint16_t)(1U % g_sh.book_count));
      break;
    case k_sh_demo_toc1:
      g_sh.screen = k_sh_screen_toc;
      break;
    case k_sh_demo_cover2:
      (void)sh_select_book((uint16_t)(2U % g_sh.book_count));
      break;
    case k_sh_demo_read2:
    default:
      sh_start_reading();
      break;
  }
}

/**
 * @brief Run the reader superloop -- input pump, opt-in self-demo, watchdog, backlight idle-dim.
 *
 * @details Arms the WWDT, then loops forever: refreshes the watchdog each
 * iteration (a wedged loop underflows the WWDT into a reset), pumps touch/button
 * input, advances the opt-in self-demo (hold SW1 at boot to enable), and blanks
 * or relights the backlight around `k_sh_idle_dim_ms` of idle. `prev1` starts at
 * the boot SW1 state so a held SW1 does not also fire as a button edge; the loop
 * idles in WFI between taps so ra8_emulator fast-forwards.
 *
 * @pre The shelf has been presented and boot is complete (thumbs built, banner printed).
 * @pre The board switch/touch inputs are initialised.
 * @post Does not return -- drives the reader until reset.
 * @post The watchdog is armed and refreshed on every iteration.
 * @note Not thread-safe; the single application superloop.
 * @since 0.1.0
 */
static void sh_run(void)
{
  ra8_board_sw_state_t sw1_boot = k_ra8_board_sw_released;
  (void)ra8_board_sw_read(k_ra8_board_sw1, &sw1_boot);
  uint8_t              prev_touch  = 0U;
  ra8_board_sw_state_t prev1       = sw1_boot;
  ra8_board_sw_state_t prev2       = k_ra8_board_sw_released;
  bool                 demo        = (sw1_boot == k_ra8_board_sw_pressed);
  uint32_t             demo_ticks  = 0U;
  uint32_t             demo_step   = 0U;
  uint32_t             idle_ref_ms = ra8_time_ms(); /* timestamp of last activity. */
  bool                 backlit     = true;          /* backlight currently lit.    */
  sh_wdt_arm_or_halt();
  while (1) {
    (void)ra8_wdt_refresh_for(k_ra8_wdt0); /* heartbeat: loop is alive. */
    const bool acted = sh_pump_input(&prev_touch, &prev1, &prev2);
    if (acted) {
      demo = false; /* a real touch / button takes over */
    } else if (demo && (++demo_ticks >= (uint32_t)k_sh_demo_period)) {
      demo_ticks = 0U;
      sh_demo_step(demo_step++);
      sh_present();
    }
    if (acted || demo) {
      idle_ref_ms = ra8_time_ms();
      if (!backlit) {
        (void)ra8_board_backlight_set(true);
        backlit = true;
      }
    } else if (backlit && ((ra8_time_ms() - idle_ref_ms) >= (uint32_t)k_sh_idle_dim_ms)) {
      (void)ra8_board_backlight_set(false);
      backlit = false;
    }
    (void)ra8_delay_ms((uint32_t)k_sh_poll_ms);
  }
}

/**
 * @brief App entry: bring up panel + optional SD, build the shelf, pump input.
 * @pre Reset_Handler copied .data and zeroed .bss; SystemInit set VTOR/FPU.
 * @post The shelf scans on the panel; taps open books, browse, and read.
 * @since 0.1.0
 */
void main(void)
{
  sh_setup_or_halt();
  ra8_isr_globals_enable();
  sh_panel_or_halt();
  sh_input_init();

  g_sh.screen   = k_sh_screen_shelf;
  g_sh.selected = 0U;
  sh_seed_baked();
  g_sh.sd_ready = sh_sd_mount();
  if (g_sh.sd_ready) {
    sh_sd_scan();
  }
  sh_shelf_build_thumbs();

  sh_present();

  /* Prove the integrated comic path (#236) headlessly: decode page 0 of the
   * baked CBZ + CBR fixtures into an off-screen scratch (rebinding ra8_gfx to it
   * and back), so the shelf render that `sh_present` just left in the live
   * framebuffer -- and its pinned `fb=` hash -- is untouched. */
  sh_comic_probe_t cbz    = {};
  sh_comic_probe_t cbr    = {};
  bool             rtl_ok = false;
  sh_comic_selfcheck(&cbz, &cbr, &rtl_ok);
  sh_print_banner(&cbz, &cbr, rtl_ok);

  sh_run(); /* never returns */
}
