/**
 * @file examples/ek_ra8d2/hw_validated/hil/widget_chrome_demo/src/main.c
 * @brief Concrete ereader chrome widgets composited on the live GLCDC panel (#145 Phase 2).
 *
 * @details
 * The sibling `widget_compose_demo` proves the ::ra8_widget_panel compositor
 * primitive with generic tiles. This app is the focused demonstration of issue
 * #145 **Phase 2**: the concrete ereader chrome widgets now extracted into the
 * `ra8_widget` library, each a reusable leaf that paints through the injected
 * ::ra8_widget_paint_t backend. It composes them into one panel tree on the GLCDC
 * panel:
 *
 *   root (column panel)
 *   |- status bar    (::ra8_widget_status_bar_t,  fixed) -- title + live clock + rule
 *   |- toolbar       (::ra8_widget_toolbar_t,     fixed) -- search field + count chip
 *   |- book grid     (::ra8_widget_book_grid_t,   flex)  -- 2-column cover / title / bar
 *   |- progress bar  (::ra8_widget_progress_bar_t,fixed) -- an overall read gauge
 *   |- nav strip     (::ra8_widget_nav_bar_t,     fixed) -- Library / Store / Settings
 *
 * The whole tree is composed by one ::ra8_widget_panel_compose call, which lays
 * the children out, computes the minimal damage rect + refresh hint, and renders
 * only the dirty children. The leaf renders go through a single ::ra8_widget_paint_t
 * backend bound to `ra8_gfx` -- the same DI seam the host unit tests bind to a
 * recording mock, so the widgets run byte-for-byte the same logic on the host and
 * on the M85.
 *
 * A deterministic self-check runs before the live loop:
 *   - **Full compose**: mark the whole tree dirty -> all 5 chrome children dirty,
 *     damage covers the full 512x512 frame, quality hint.
 *   - **Partial compose**: advance the clock and invalidate **only** the status
 *     bar -> exactly 1 dirty child, damage is just the status-bar rect with the
 *     fast hint, and the composite CRC changes (the clock advanced). This is the
 *     Phase-1 partial-flush acceptance, now driven by a real chrome widget.
 *
 * It then emits one banner so the app doubles as a ra8_emulator gate:
 *
 *   `widget-chrome-demo: dirty0=5 crc0=<8hex> dirty1=1 crc1=<8hex> flush=512x44 hint=fast PASS`
 *
 * Any failed assertion prints `FAIL ...` and parks. After the banner the loop
 * advances the clock and partial-composes the status bar live, so the panel
 * visibly updates only its top band (the damage-tracked A2 path).
 *
 *
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_box.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_glcdc.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_time.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_book.h"
#include "ra8_widget_nav_bar.h"
#include "ra8_widget_progress_bar.h"
#include "ra8_widget_status_bar.h"
#include "ra8_widget_toolbar.h"

/**
 * @enum wd_geom_t
 * @brief Framebuffer + band geometry (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_fb_w      = 512U, /**< Framebuffer width, pixels.            */
  k_wd_fb_h      = 512U, /**< Framebuffer height, pixels.           */
  k_wd_status_h  = 44U,  /**< Status-bar band height, pixels.       */
  k_wd_toolbar_h = 40U,  /**< Toolbar band height, pixels.          */
  k_wd_prog_h    = 12U,  /**< Overall progress band height, pixels. */
  k_wd_nav_h     = 40U,  /**< Nav-strip band height, pixels.        */
  k_wd_pad       = 8U,   /**< Inner padding, pixels.                */
  k_wd_gap       = 8U,   /**< Grid gap, pixels.                     */
  k_wd_label_h   = 18U,  /**< Card title / author row height.       */
  k_wd_bar_h     = 6U,   /**< Card progress-bar height.             */
  k_wd_count_w   = 96U,  /**< Toolbar count-chip width.             */
} wd_geom_t;

/**
 * @enum wd_color_t
 * @brief 0xRRGGBB palette (ra8_gfx packs to RGB565).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wd_col_clear   = 0x000008U, /**< Whole-frame clear (near black). */
  k_wd_col_paper   = 0xF4F0E8U, /**< Grid / page background.         */
  k_wd_col_status  = 0x1E2A40U, /**< Status-bar fill (slate).        */
  k_wd_col_toolbar = 0x14141AU, /**< Toolbar fill (dark).            */
  k_wd_col_field   = 0x2A2A34U, /**< Search-field fill.              */
  k_wd_col_rule    = 0x50607AU, /**< Hairline / border.              */
  k_wd_col_ink     = 0x101018U, /**< Primary ink.                    */
  k_wd_col_dim     = 0x808494U, /**< Secondary text (gray).          */
  k_wd_col_white   = 0xFFFFFFU, /**< White text.                     */
  k_wd_col_cover_a = 0x806040U, /**< Cover swatch A.                 */
  k_wd_col_cover_b = 0x404060U, /**< Cover swatch B.                 */
  k_wd_col_cover_c = 0x406050U, /**< Cover swatch C.                 */
  k_wd_col_cover_d = 0x604048U, /**< Cover swatch D.                 */
  k_wd_col_track   = 0x303038U, /**< Progress track.                 */
  k_wd_col_fill    = 0xE0A020U, /**< Progress fill (amber).          */
  k_wd_col_nav     = 0x14141AU, /**< Nav-strip fill.                 */
} wd_color_t;

/**
 * @enum wd_child_idx_t
 * @brief Index of each chrome band within the root panel array.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_w_status  = 0U, /**< Root child 0: status bar (fixed). */
  k_wd_w_toolbar = 1U, /**< Root child 1: toolbar (fixed).    */
  k_wd_w_grid    = 2U, /**< Root child 2: book grid (flex).   */
  k_wd_w_prog    = 3U, /**< Root child 3: progress (fixed).   */
  k_wd_w_nav     = 4U, /**< Root child 4: nav strip (fixed).  */
  k_wd_root_n    = 5U, /**< Root child count.                 */
  k_wd_root_cap  = 6U, /**< Root ra8_box scratch (count + 1). */
} wd_child_idx_t;

/**
 * @enum wd_data_t
 * @brief Book count + progress + console constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wd_book_count  = 4U,          /**< Book records in the grid.    */
  k_wd_grid_cols   = 2U,          /**< Grid columns.                */
  k_wd_prog_value  = 3U,          /**< Overall progress value.      */
  k_wd_prog_total  = 10U,         /**< Overall progress full-scale. */
  k_wd_uart_baud   = 115200U,     /**< Console baud.                */
  k_wd_frame_ms    = 500U,        /**< Live status repaint (ms).    */
  k_wd_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.      */
  k_wd_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.             */
  k_wd_hex_nibbles = 8U,          /**< Hex digits per 32-bit.       */
  k_wd_nibble_bits = 4U,          /**< Bits per hex nibble.         */
  k_wd_nibble_mask = 0x0FU,       /**< Low-nibble mask.             */
  k_wd_dec_ten     = 10U,         /**< Decimal radix + buffer size. */
  k_wd_clock_len   = 6U,          /**< "NNNNN" clock buffer + NUL.  */
} wd_data_t;

/**
 * @brief GLCDC-scanned render target in SRAM, AXI-burst aligned.
 * @since 0.1.0
 */
[[gnu::aligned(k_ra8_board_fb_align_bytes)]] static uint16_t
  s_framebuffer[(uint32_t)k_wd_fb_w * (uint32_t)k_wd_fb_h];

/** @brief Live display handle once the GLCDC panel is up. */
static display_handle_t* s_display = nullptr;

/** @brief Status-bar right-side clock string (advances each partial flush). */
static char s_clock[k_wd_clock_len] = "0";

/** @brief Monotonic clock counter shown in the status bar. */
static uint32_t s_clock_val = 0U;

/**
 * @brief Sample reading-progress percentages for the four demo books.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_wd_pct_dune        = 42U,  /**< "Dune" read progress, percent.        */
  k_wd_pct_1984        = 100U, /**< "1984" read progress, percent.        */
  k_wd_pct_solaris     = 12U,  /**< "Solaris" read progress, percent.     */
  k_wd_pct_neuromancer = 70U,  /**< "Neuromancer" read progress, percent. */
} wd_book_pct_t;

/** @brief The four book records shown in the grid. */
static const ra8_widget_book_t s_books[k_wd_book_count] = {
  {.title   = "Dune",
   .author  = "Herbert",
   .cover   = (uint32_t)k_wd_col_cover_a,
   .percent = k_wd_pct_dune},
  {.title   = "1984",
   .author  = "Orwell",
   .cover   = (uint32_t)k_wd_col_cover_b,
   .percent = k_wd_pct_1984},
  {.title   = "Solaris",
   .author  = "Lem",
   .cover   = (uint32_t)k_wd_col_cover_c,
   .percent = k_wd_pct_solaris},
  {.title   = "Neuromancer",
   .author  = "Gibson",
   .cover   = (uint32_t)k_wd_col_cover_d,
   .percent = k_wd_pct_neuromancer},
};

/** @brief Nav-strip destinations. */
static const char* const s_nav_items[] = {"Library", "Store", "Settings"};

/* ===========================================================================
 * ra8_gfx paint backend (the DI seam the widgets paint through)
 * ===========================================================================
 */

/**
 * @brief Paint backend: fill a rectangle through ra8_gfx.
 * @param[in] user  Unused backend handle.
 * @param[in] x     Left edge, pixels.
 * @param[in] y     Top edge, pixels.
 * @param[in] w     Width, pixels.
 * @param[in] h     Height, pixels.
 * @param[in] color Fill colour, 0xRRGGBB.
 * @return Nothing.
 * @pre ra8_gfx is bound to the framebuffer.
 * @pre @p w and @p h are the widget rect extents.
 * @post The rectangle is filled in the framebuffer.
 * @post No other pixels are touched.
 * @note Not thread-safe (single-threaded UI loop).
 * @since 0.1.0
 */
static void wd_fill(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)ra8_gfx_rect(x, y, w, h, color, true);
}

/**
 * @brief Paint backend: draw ASCII text through ra8_gfx.
 * @param[in] user Unused backend handle.
 * @param[in] x    Pen X, pixels.
 * @param[in] y    Pen Y, pixels.
 * @param[in] str  NUL-terminated ASCII string.
 * @param[in] fg   Text colour, 0xRRGGBB.
 * @param[in] bg   Background colour behind the glyphs, 0xRRGGBB.
 * @return Nothing.
 * @pre ra8_gfx is bound to the framebuffer.
 * @pre @p str is NUL-terminated.
 * @post The string is drawn at (@p x, @p y).
 * @post No other pixels are touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void wd_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)ra8_gfx_text_out(x, y, str, &ra8_gfx_font_8x16, fg, bg);
}

/**
 * @brief Paint backend: measure a string's pixel size through ra8_gfx.
 * @param[in]  user  Unused backend handle.
 * @param[in]  str   NUL-terminated ASCII string.
 * @param[out] out_w Receives the pixel width.
 * @param[out] out_h Receives the pixel height.
 * @return Nothing.
 * @pre @p str, @p out_w, @p out_h are non-NULL.
 * @pre The bundled font is available.
 * @post `*out_w` / `*out_h` hold the measured size (0 on a measure error).
 * @post No pixels are touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void wd_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  uint32_t w = 0U;
  uint32_t h = 0U;
  (void)ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &w, &h);
  *out_w = (int32_t)w;
  *out_h = (int32_t)h;
}

/** @brief The single paint backend the chrome widgets share. */
static const ra8_widget_paint_t k_wd_paint = {
  .user      = nullptr,
  .fill_rect = wd_fill,
  .draw_text = wd_text,
  .text_size = wd_size,
};

/* ===========================================================================
 * Chrome widget descriptors + the root panel
 * ===========================================================================
 */

static ra8_widget_status_bar_t s_status = {.paint    = &k_wd_paint,
                                           .left     = "widget-chrome-demo  Library",
                                           .right    = s_clock,
                                           .bg       = (uint32_t)k_wd_col_status,
                                           .fg       = (uint32_t)k_wd_col_white,
                                           .fg_right = (uint32_t)k_wd_col_dim,
                                           .rule     = (uint32_t)k_wd_col_rule,
                                           .pad      = (int16_t)k_wd_pad,
                                           .rule_h   = 1};

static ra8_widget_toolbar_t s_toolbar = {.paint     = &k_wd_paint,
                                         .hint      = "Search",
                                         .count     = "4 books",
                                         .on_search = nullptr,
                                         .bg        = (uint32_t)k_wd_col_toolbar,
                                         .field     = (uint32_t)k_wd_col_field,
                                         .border    = (uint32_t)k_wd_col_rule,
                                         .hint_fg   = (uint32_t)k_wd_col_dim,
                                         .count_fg  = (uint32_t)k_wd_col_dim,
                                         .pad       = (int16_t)k_wd_pad,
                                         .border_w  = 1,
                                         .count_w   = (int16_t)k_wd_count_w};

static ra8_widget_book_grid_t s_grid = {.paint     = &k_wd_paint,
                                        .books     = s_books,
                                        .on_open   = nullptr,
                                        .bg        = (uint32_t)k_wd_col_paper,
                                        .title_fg  = (uint32_t)k_wd_col_ink,
                                        .author_fg = (uint32_t)k_wd_col_dim,
                                        .bar_track = (uint32_t)k_wd_col_track,
                                        .bar_fill  = (uint32_t)k_wd_col_fill,
                                        .count     = (uint16_t)k_wd_book_count,
                                        .cols      = (uint16_t)k_wd_grid_cols,
                                        .pad       = (int16_t)k_wd_pad,
                                        .gap       = (int16_t)k_wd_gap,
                                        .label_h   = (int16_t)k_wd_label_h,
                                        .bar_h     = (int16_t)k_wd_bar_h};

static ra8_widget_progress_bar_t s_prog = {.paint = &k_wd_paint,
                                           .track = (uint32_t)k_wd_col_track,
                                           .fill  = (uint32_t)k_wd_col_fill,
                                           .value = (uint16_t)k_wd_prog_value,
                                           .total = (uint16_t)k_wd_prog_total};

static ra8_widget_nav_bar_t s_nav = {.paint     = &k_wd_paint,
                                     .items     = s_nav_items,
                                     .on_select = nullptr,
                                     .bg        = (uint32_t)k_wd_col_nav,
                                     .fg_active = (uint32_t)k_wd_col_white,
                                     .fg_muted  = (uint32_t)k_wd_col_dim,
                                     .count     = 3U,
                                     .active    = 0U};

static ra8_widget_t s_root_kids[k_wd_root_n];      /**< The five chrome bands. */
static ra8_box_t    s_root_scratch[k_wd_root_cap]; /**< Root layout scratch.   */

/** @brief Root column-panel descriptor tiling the five chrome bands. */
static ra8_widget_panel_t s_root_panel = {.children    = s_root_kids,
                                          .box_scratch = s_root_scratch,
                                          .count       = (uint16_t)k_wd_root_n,
                                          .box_cap     = (uint16_t)k_wd_root_cap,
                                          .gap         = 0,
                                          .pad         = 0,
                                          .axis        = k_ra8_widget_axis_col,
                                          .reserved    = 0U};

/** @brief The root panel widget (composed each frame). */
static ra8_widget_t s_root;

static const uint8_t k_wd_msg_boot[]  = "widget-chrome-demo: boot\r\n";
static const uint8_t k_wd_msg_finit[] = "widget-chrome-demo: FAIL init\r\n";
static const uint8_t k_wd_msg_fpanl[] = "widget-chrome-demo: FAIL panel\r\n";
static const uint8_t k_wd_msg_ftree[] = "widget-chrome-demo: FAIL tree\r\n";
static const uint8_t k_wd_msg_fchk[]  = "widget-chrome-demo: FAIL selfcheck\r\n";
static const uint8_t k_wd_msg_pre[]   = "widget-chrome-demo: dirty0=";
static const uint8_t k_wd_msg_crc0[]  = " crc0=";
static const uint8_t k_wd_msg_d1[]    = " dirty1=";
static const uint8_t k_wd_msg_crc1[]  = " crc1=";
static const uint8_t k_wd_msg_tail[]  = " flush=512x44 hint=fast";
static const uint8_t k_wd_msg_pass[]  = " PASS\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the board console. */
static void wd_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a fail banner and park forever in WFI. */
static void wd_fail_halt(const uint8_t* msg, uint32_t len)
{
  wd_print(msg, len);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print @p value as 8 uppercase hex digits. */
static void wd_print_hex(uint32_t value)
{
  uint8_t buf[k_wd_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_wd_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_wd_hex_nibbles - 1U - i) * (uint32_t)k_wd_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_wd_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_wd_dec_ten) ? ('0' + nib) : ('A' + (nib - k_wd_dec_ten)));
  }
  wd_print(buf, (uint32_t)k_wd_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void wd_print_uint(uint32_t value)
{
  uint8_t  buf[k_wd_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_wd_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_wd_dec_ten));
    n++;
    value /= (uint32_t)k_wd_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    wd_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief FNV-1a-32 over the composited framebuffer bytes. */
static uint32_t wd_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_wd_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_wd_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Clock string + tree assembly + compose
 * ===========================================================================
 */

/** @brief Reformat ::s_clock as the decimal ::s_clock_val (bounded, NUL-term). */
static void wd_clock_fmt(void)
{
  uint32_t v = s_clock_val;
  char     tmp[k_wd_clock_len];
  uint32_t n = 0U;
  if (v == 0U) {
    tmp[n] = '0';
    n++;
  }
  while ((v > 0U) && (n < (uint32_t)(k_wd_clock_len - 1U))) {
    tmp[n] = (char)('0' + (v % (uint32_t)k_wd_dec_ten));
    n++;
    v /= (uint32_t)k_wd_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    s_clock[i] = tmp[n - 1U - i];
  }
  s_clock[n] = '\0';
}

/** @brief Build a leaf widget bound to a vtable + context. */
static void wd_leaf_init(ra8_widget_t*              w,
                         const ra8_widget_vtable_t* vt,
                         void*                      ctx,
                         int16_t                    fixed,
                         uint16_t                   flex)
{
  *w         = (ra8_widget_t){};
  w->vt      = vt;
  w->ctx     = ctx;
  w->fixed   = fixed;
  w->flex    = flex;
  w->visible = true;
}

/** @brief Assemble the chrome panel tree; return false on a bind error. */
static bool wd_build_tree(void)
{
  wd_leaf_init(&s_root_kids[k_wd_w_status],
               ra8_widget_status_bar_vtable(),
               &s_status,
               (int16_t)k_wd_status_h,
               0U);
  wd_leaf_init(&s_root_kids[k_wd_w_toolbar],
               ra8_widget_toolbar_vtable(),
               &s_toolbar,
               (int16_t)k_wd_toolbar_h,
               0U);
  wd_leaf_init(&s_root_kids[k_wd_w_grid], ra8_widget_book_grid_vtable(), &s_grid, 0, 1U);
  wd_leaf_init(&s_root_kids[k_wd_w_prog],
               ra8_widget_progress_bar_vtable(),
               &s_prog,
               (int16_t)k_wd_prog_h,
               0U);
  wd_leaf_init(&s_root_kids[k_wd_w_nav],
               ra8_widget_nav_bar_vtable(),
               &s_nav,
               (int16_t)k_wd_nav_h,
               0U);
  return (ra8_widget_panel_init(&s_root, &s_root_panel) == k_ra8_ok);
}

/** @brief Compose the chrome tree into the framebuffer; report the flush rect. */
static ra8_err_t wd_compose(ra8_ui_rect_t* dmg, ra8_widget_refresh_t* hint, uint16_t* dirty)
{
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_wd_fb_w, .h = (int32_t)k_wd_fb_h};
  return ra8_widget_panel_compose(&s_root, &frame, dmg, hint, dirty);
}

/** @brief Mark every chrome band dirty so the next compose repaints the tree. */
static void wd_invalidate_all(ra8_widget_refresh_t refresh)
{
  for (uint16_t i = 0U; i < (uint16_t)k_wd_root_n; i++) {
    (void)ra8_widget_invalidate(&s_root_kids[i], refresh);
  }
}

/* ===========================================================================
 * GLCDC panel bring-up + flush
 * ===========================================================================
 */

/** @brief Display PAL config: GLCDC LCD backend bound to the SRAM framebuffer. */
static const display_cfg_t k_wd_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_wd_fb_w,
  .height_px         = (uint16_t)k_wd_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/** @brief Bring up the GLCDC panel and bind ra8_gfx to its framebuffer. */
static bool wd_panel_up(void)
{
  if (display_init(&k_wd_display_cfg, &s_display) != k_ra8_ok) {
    return false;
  }
  if (s_display == nullptr) {
    return false;
  }
  display_fb_t fb = {};
  if (display_get_framebuffer(s_display, &fb) != k_ra8_ok) {
    return false;
  }
  if (fb.pixels != (void*)s_framebuffer) {
    return false;
  }
  return (ra8_gfx_init(s_framebuffer,
                       (uint16_t)k_wd_fb_w,
                       (uint16_t)k_wd_fb_h,
                       k_ra8_gfx_format_rgb565) == k_ra8_ok);
}

/** @brief Map a widget refresh hint to the display PAL refresh hint. */
static display_refresh_hint_t wd_map_hint(ra8_widget_refresh_t hint)
{
  if (hint == k_ra8_widget_refresh_fast) {
    return k_display_refresh_fast;
  }
  return k_display_refresh_quality;
}

/** @brief Flush only @p dmg to the panel with the mapped refresh hint. */
static void wd_flush_rect(const ra8_ui_rect_t* dmg, ra8_widget_refresh_t hint)
{
  if (s_display == nullptr) {
    return;
  }
  if ((dmg->w <= 0) || (dmg->h <= 0)) {
    return;
  }
  const display_rect_t r = {.x = (uint16_t)dmg->x,
                            .y = (uint16_t)dmg->y,
                            .w = (uint16_t)dmg->w,
                            .h = (uint16_t)dmg->h};
  (void)display_flush(s_display, r, wd_map_hint(hint));
}

/* ===========================================================================
 * Deterministic self-check
 * ===========================================================================
 */

/** @brief Assert the full compose: all 5 chrome bands dirty, full-frame quality. */
static bool wd_check_full(uint32_t* out_crc, uint16_t* out_dirty)
{
  wd_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wd_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != (uint16_t)k_wd_root_n) {
    return false;
  }
  if ((dmg.w != (int32_t)k_wd_fb_w) || (dmg.h != (int32_t)k_wd_fb_h)) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_quality) {
    return false;
  }
  *out_crc   = wd_framebuffer_hash();
  *out_dirty = nd;
  return true;
}

/** @brief Assert the partial compose: status-only damage, fast hint. */
static bool wd_check_partial(uint32_t* out_crc, uint16_t* out_dirty)
{
  s_clock_val++;
  wd_clock_fmt();
  (void)ra8_widget_invalidate(&s_root_kids[k_wd_w_status], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wd_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != 1U) {
    return false;
  }
  if ((dmg.x != 0) || (dmg.y != 0) || (dmg.w != (int32_t)k_wd_fb_w)) {
    return false;
  }
  if (dmg.h != (int32_t)k_wd_status_h) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_fast) {
    return false;
  }
  *out_crc   = wd_framebuffer_hash();
  *out_dirty = nd;
  return true;
}

/** @brief Run the whole deterministic surface; return both composites' CRCs. */
static bool wd_selfcheck(uint32_t* crc0, uint16_t* d0, uint32_t* crc1, uint16_t* d1)
{
  if (!wd_check_full(crc0, d0)) {
    return false;
  }
  if (!wd_check_partial(crc1, d1)) {
    return false;
  }
  return (*crc0 != *crc1); /* the advanced clock changes the status band */
}

/* ===========================================================================
 * Boot
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time and the console. */
static void wd_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    wd_fail_halt(k_wd_msg_finit, (uint32_t)sizeof(k_wd_msg_finit) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wd_fail_halt(k_wd_msg_finit, (uint32_t)sizeof(k_wd_msg_finit) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wd_fail_halt(k_wd_msg_finit, (uint32_t)sizeof(k_wd_msg_finit) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_wd_uart_baud) != k_ra8_ok) {
    wd_fail_halt(k_wd_msg_finit, (uint32_t)sizeof(k_wd_msg_finit) - 1U);
  }
}

/** @brief Emit the PASS banner once every self-check assertion holds. */
static void wd_print_banner(uint16_t d0, uint32_t crc0, uint16_t d1, uint32_t crc1)
{
  wd_print(k_wd_msg_pre, (uint32_t)sizeof(k_wd_msg_pre) - 1U);
  wd_print_uint((uint32_t)d0);
  wd_print(k_wd_msg_crc0, (uint32_t)sizeof(k_wd_msg_crc0) - 1U);
  wd_print_hex(crc0);
  wd_print(k_wd_msg_d1, (uint32_t)sizeof(k_wd_msg_d1) - 1U);
  wd_print_uint((uint32_t)d1);
  wd_print(k_wd_msg_crc1, (uint32_t)sizeof(k_wd_msg_crc1) - 1U);
  wd_print_hex(crc1);
  wd_print(k_wd_msg_tail, (uint32_t)sizeof(k_wd_msg_tail) - 1U);
  wd_print(k_wd_msg_pass, (uint32_t)sizeof(k_wd_msg_pass) - 1U);
}

/** @brief Advance the clock and partial-flush only the status band. */
static void wd_tick_live(void)
{
  s_clock_val++;
  wd_clock_fmt();
  (void)ra8_widget_invalidate(&s_root_kids[k_wd_w_status], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wd_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wd_flush_rect(&dmg, hint);
  }
}

/**
 * @brief App entry: bring the panel up, self-check the chrome, run the live loop.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The PASS banner is emitted and the panel shows the composed chrome; the
 *       loop partial-flushes only the status band so the panel updates its clock
 *       live (the damage-tracked path), otherwise idling.
 * @since 0.1.0
 */
void main(void)
{
  wd_setup_or_halt();
  ra8_isr_globals_enable();
  wd_print(k_wd_msg_boot, (uint32_t)sizeof(k_wd_msg_boot) - 1U);

  if (!wd_panel_up()) {
    wd_fail_halt(k_wd_msg_fpanl, (uint32_t)sizeof(k_wd_msg_fpanl) - 1U);
  }
  if (!wd_build_tree()) {
    wd_fail_halt(k_wd_msg_ftree, (uint32_t)sizeof(k_wd_msg_ftree) - 1U);
  }

  uint32_t crc0 = 0U;
  uint32_t crc1 = 0U;
  uint16_t d0   = 0U;
  uint16_t d1   = 0U;
  if (!wd_selfcheck(&crc0, &d0, &crc1, &d1)) {
    wd_fail_halt(k_wd_msg_fchk, (uint32_t)sizeof(k_wd_msg_fchk) - 1U);
  }
  wd_print_banner(d0, crc0, d1, crc1);

  /* Land on a full composite and show it live on the panel. */
  (void)ra8_gfx_clear((uint32_t)k_wd_col_clear);
  wd_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wd_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wd_flush_rect(&dmg, hint);
  }

  while (1) {
    wd_tick_live();
    ra8_delay_ms((uint32_t)k_wd_frame_ms);
  }
}
