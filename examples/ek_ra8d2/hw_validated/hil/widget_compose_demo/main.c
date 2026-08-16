/**
 * @file examples/ek_ra8d2/hw_validated/hil/widget_compose_demo/main.c
 * @brief Nested ra8_widget tree composited on the live GLCDC panel (#145).
 *
 * @details
 * The sibling `widget_app_demo` proves the `ra8_widget` + `ra8_app` stack as an
 * interactive launcher. This app is the focused demonstration of the **new
 * compositor primitive** issue #145 adds: ::ra8_widget_panel -- a container that
 * is itself a ::ra8_widget_t, which is what turns the flat widget array into a
 * **tree**. It composes a small tree and shows the damage-tracked partial flush
 * on `ra8_emulator`'s panel window.
 *
 * The tree (dwm-style, each piece opt-in):
 *
 *   root (column panel)
 *   |- status   (leaf, fixed)   -- title + a live frame counter
 *   |- body     (ROW PANEL)     -- a nested panel ...
 *   |   |- left  tile (leaf, flex)
 *   |   |- right tile (leaf, flex)
 *   |- footer   (leaf, fixed)   -- a hint line
 *
 * `body` is the new primitive in action: a panel nested inside the root panel.
 * The whole tree is composed by one ::ra8_widget_panel_compose call, which lays
 * the children out, computes the minimal damage rect + refresh hint, and
 * renders only the dirty children -- a dirty nested panel repaints its whole
 * subtree.
 *
 * A deterministic self-check runs before the live loop:
 *   - **Full compose**: mark the whole tree dirty -> 3 dirty children, damage
 *     covers the full 512x512 frame, quality hint, and the nested `body` panel
 *     renders both tiles (proven by per-tile render counters).
 *   - **Partial compose**: bump the frame counter and invalidate **only** the
 *     status bar -> exactly 1 dirty child, damage is just the 512x44 status
 *     rect with the fast hint, the tiles are NOT re-rendered, and the composite
 *     CRC changes (the counter advanced). This is the acceptance bullet: a
 *     status-only change flushes only the status rect.
 *
 * It then emits one banner so the app doubles as a ra8_emulator gate:
 *
 *   `widget-compose-demo: dirty0=3 crc0=<8hex> dirty1=1 crc1=<8hex> flush=512x44 hint=fast PASS`
 *
 * Any failed assertion prints `FAIL ...` and parks. After the banner the loop
 * advances the frame counter and partial-composes the status bar live, so the
 * panel visibly updates only its top band (the damage-tracked A2 path).
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
#include "ra8_panel_timing.h"
#include "ra8_time.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/**
 * @enum wc_geom_t
 * @brief Framebuffer + band geometry (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wc_fb_w     = 512U, /**< Framebuffer width, pixels.             */
  k_wc_fb_h     = 512U, /**< Framebuffer height, pixels.            */
  k_wc_fb_align = 64U,  /**< AXI-burst alignment for clean fetches. */
  k_wc_status_h = 44U,  /**< Status-bar band height, pixels.        */
  k_wc_footer_h = 28U,  /**< Footer band height, pixels.            */
  k_wc_pad      = 8U,   /**< Inner text padding, pixels.            */
  k_wc_line_h   = 18U,  /**< Text line pitch (16px glyph + 2).      */
  k_wc_tile_gap = 6U,   /**< Gap between the two body tiles.        */
} wc_geom_t;

/**
 * @enum wc_color_t
 * @brief 0xRRGGBB palette (ra8_gfx packs to RGB565).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wc_col_clear     = 0x000008U, /**< Whole-frame clear (near black). */
  k_wc_col_status_bg = 0x1E2A40U, /**< Status-bar fill (slate).        */
  k_wc_col_footer_bg = 0x14141AU, /**< Footer fill (dark).             */
  k_wc_col_left_bg   = 0x102038U, /**< Left tile (deep blue).          */
  k_wc_col_right_bg  = 0x382414U, /**< Right tile (warm brown).        */
  k_wc_col_text      = 0xFFFFFFU, /**< Primary text (white).           */
  k_wc_col_dim       = 0xA0A0B0U, /**< Secondary text (gray).          */
} wc_color_t;

/**
 * @enum wc_widget_idx_t
 * @brief Index of each widget within its parent array.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wc_w_status = 0U, /**< Root child 0: status bar (fixed). */
  k_wc_w_body   = 1U, /**< Root child 1: nested body panel.  */
  k_wc_w_footer = 2U, /**< Root child 2: footer (fixed).     */
  k_wc_root_n   = 3U, /**< Root child count.                 */
  k_wc_root_cap = 4U, /**< Root ra8_box scratch (count + 1). */
  k_wc_w_left   = 0U, /**< Body child 0: left tile (flex).   */
  k_wc_w_right  = 1U, /**< Body child 1: right tile (flex).  */
  k_wc_body_n   = 2U, /**< Body child count.                 */
  k_wc_body_cap = 3U, /**< Body ra8_box scratch (count + 1). */
} wc_widget_idx_t;

/**
 * @enum wc_console_t
 * @brief Console baud + text-format constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wc_uart_baud   = 115200U,     /**< Console baud.             */
  k_wc_frame_ms    = 500U,        /**< Live status repaint (ms). */
  k_wc_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.   */
  k_wc_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.          */
  k_wc_hex_nibbles = 8U,          /**< Hex digits per 32-bit.    */
  k_wc_nibble_bits = 4U,          /**< Bits per hex nibble.      */
  k_wc_nibble_mask = 0x0FU,       /**< Low-nibble mask.          */
  k_wc_dec_ten     = 10U,         /**< Hex/decimal split + buf.  */
  k_wc_frame_buf   = 24U,         /**< "frame=N" string buffer.  */
} wc_console_t;

/**
 * @brief GLCDC-scanned render target in SRAM, AXI-burst aligned.
 * @since 0.1.0
 */
[[gnu::aligned(
  k_wc_fb_align)]] static uint16_t s_framebuffer[(uint32_t)k_wc_fb_w * (uint32_t)k_wc_fb_h];

/**
 * @struct wc_status_ctx_t
 * @brief Status-bar state: the live frame counter it renders.
 * @since 0.1.0
 */
typedef struct {
  uint32_t frame;   /**< Frame counter shown in the status bar. */
  uint32_t renders; /**< Times the status render ran.           */
} wc_status_ctx_t;

/**
 * @struct wc_tile_ctx_t
 * @brief One body tile: a label over a coloured fill + a render counter.
 * @since 0.1.0
 */
typedef struct {
  const char* label;   /**< Tile heading.             */
  uint32_t    bg;      /**< Tile background colour.   */
  uint32_t    renders; /**< Times this tile rendered. */
} wc_tile_ctx_t;

static wc_status_ctx_t s_status = {.frame = 0U, .renders = 0U};
static wc_tile_ctx_t   s_left   = {.label = "Left widget", .bg = (uint32_t)k_wc_col_left_bg};
static wc_tile_ctx_t   s_right  = {.label = "Right widget", .bg = (uint32_t)k_wc_col_right_bg};

static ra8_widget_t s_root_kids[k_wc_root_n];      /**< [status, body, footer].  */
static ra8_widget_t s_body_kids[k_wc_body_n];      /**< [left tile, right tile]. */
static ra8_box_t    s_body_scratch[k_wc_body_cap]; /**< Body layout scratch.     */
static ra8_box_t    s_root_scratch[k_wc_root_cap]; /**< Root layout scratch.     */

/** @brief Body row-panel descriptor (the nested panel). */
static ra8_widget_panel_t s_body_panel = {.children    = s_body_kids,
                                          .box_scratch = s_body_scratch,
                                          .count       = (uint16_t)k_wc_body_n,
                                          .box_cap     = (uint16_t)k_wc_body_cap,
                                          .gap         = (int16_t)k_wc_tile_gap,
                                          .pad         = 0,
                                          .axis        = k_ra8_widget_axis_row,
                                          .reserved    = 0U};

/** @brief Root column-panel descriptor. */
static ra8_widget_panel_t s_root_panel = {.children    = s_root_kids,
                                          .box_scratch = s_root_scratch,
                                          .count       = (uint16_t)k_wc_root_n,
                                          .box_cap     = (uint16_t)k_wc_root_cap,
                                          .gap         = 0,
                                          .pad         = 0,
                                          .axis        = k_ra8_widget_axis_col,
                                          .reserved    = 0U};

/** @brief The root panel widget (composed each frame). */
static ra8_widget_t s_root;
/** @brief Live display handle once the GLCDC panel is up. */
static display_handle_t* s_display = nullptr;

static const uint8_t k_wc_msg_boot[]  = "widget-compose-demo: boot\r\n";
static const uint8_t k_wc_msg_finit[] = "widget-compose-demo: FAIL init\r\n";
static const uint8_t k_wc_msg_fpanl[] = "widget-compose-demo: FAIL panel\r\n";
static const uint8_t k_wc_msg_ftree[] = "widget-compose-demo: FAIL tree\r\n";
static const uint8_t k_wc_msg_fchk[]  = "widget-compose-demo: FAIL selfcheck\r\n";
static const uint8_t k_wc_msg_pre[]   = "widget-compose-demo: dirty0=";
static const uint8_t k_wc_msg_crc0[]  = " crc0=";
static const uint8_t k_wc_msg_d1[]    = " dirty1=";
static const uint8_t k_wc_msg_crc1[]  = " crc1=";
static const uint8_t k_wc_msg_tail[]  = " flush=512x44 hint=fast";
static const uint8_t k_wc_msg_pass[]  = " PASS\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the board console. */
static void wc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a fail banner and park forever in WFI. */
static void wc_fail_halt(const uint8_t* msg, uint32_t len)
{
  wc_print(msg, len);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print @p value as 8 uppercase hex digits. */
static void wc_print_hex(uint32_t value)
{
  uint8_t buf[k_wc_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_wc_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_wc_hex_nibbles - 1U - i) * (uint32_t)k_wc_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_wc_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_wc_dec_ten) ? ('0' + nib) : ('A' + (nib - k_wc_dec_ten)));
  }
  wc_print(buf, (uint32_t)k_wc_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void wc_print_uint(uint32_t value)
{
  uint8_t  buf[k_wc_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_wc_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_wc_dec_ten));
    n++;
    value /= (uint32_t)k_wc_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    wc_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief FNV-1a-32 over the composited framebuffer bytes. */
static uint32_t wc_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_wc_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_wc_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Widget renderers
 * ===========================================================================
 */

/** @brief Format "frame=<n>" into @p buf (>= k_wc_frame_buf bytes, NUL-term). */
static void wc_fmt_frame(char* buf, uint32_t frame)
{
  static const char prefix[] = "frame=";
  uint32_t          pos      = 0U;
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(prefix) - 1U); i++) {
    buf[pos] = prefix[i];
    pos++;
  }
  char     digits[k_wc_dec_ten];
  uint32_t n = 0U;
  if (frame == 0U) {
    digits[n] = '0';
    n++;
  }
  while ((frame > 0U) && (n < (uint32_t)k_wc_dec_ten)) {
    digits[n] = (char)('0' + (frame % (uint32_t)k_wc_dec_ten));
    n++;
    frame /= (uint32_t)k_wc_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[pos] = digits[n - 1U - i];
    pos++;
  }
  buf[pos] = '\0';
}

/** @brief Status-bar render: fill + title over the live frame counter. */
static void wc_status_render(ra8_widget_t* w)
{
  wc_status_ctx_t* st = (wc_status_ctx_t*)w->ctx;
  st->renders++;
  char frame_str[k_wc_frame_buf];
  wc_fmt_frame(frame_str, st->frame);
  (void)
    ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, (uint32_t)k_wc_col_status_bg, true);
  const int32_t tx = w->rect.x + (int32_t)k_wc_pad;
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wc_pad,
                         "widget-compose-demo  status bar",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wc_col_text,
                         (uint32_t)k_wc_col_status_bg);
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wc_pad + (int32_t)k_wc_line_h,
                         frame_str,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wc_col_dim,
                         (uint32_t)k_wc_col_status_bg);
}

/** @brief Body-tile render: fill + label + a per-tile render counter. */
static void wc_tile_render(ra8_widget_t* w)
{
  wc_tile_ctx_t* t = (wc_tile_ctx_t*)w->ctx;
  t->renders++;
  (void)ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, t->bg, true);
  const int32_t tx = w->rect.x + (int32_t)k_wc_pad;
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wc_pad,
                         t->label,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wc_col_text,
                         t->bg);
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wc_pad + (int32_t)k_wc_line_h +
                           (int32_t)k_wc_line_h,
                         "nested in the body panel",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wc_col_dim,
                         t->bg);
}

/** @brief Footer render: fill + a one-line hint. */
static void wc_footer_render(ra8_widget_t* w)
{
  (void)w->ctx;
  (void)
    ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, (uint32_t)k_wc_col_footer_bg, true);
  (void)ra8_gfx_text_out(w->rect.x + (int32_t)k_wc_pad,
                         w->rect.y + ((int32_t)k_wc_footer_h / 4),
                         "ra8_widget_panel: nested tree, damage-tracked flush",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wc_col_dim,
                         (uint32_t)k_wc_col_footer_bg);
}

/** @brief Status / footer / tiles are display-only (NULL on_input). */
static const ra8_widget_vtable_t k_wc_status_vt = {.measure  = nullptr,
                                                   .render   = wc_status_render,
                                                   .on_input = nullptr};
static const ra8_widget_vtable_t k_wc_tile_vt   = {.measure  = nullptr,
                                                   .render   = wc_tile_render,
                                                   .on_input = nullptr};
static const ra8_widget_vtable_t k_wc_footer_vt = {.measure  = nullptr,
                                                   .render   = wc_footer_render,
                                                   .on_input = nullptr};

/* ===========================================================================
 * Tree assembly + compose
 * ===========================================================================
 */

/** @brief Build a leaf widget bound to a vtable + context. */
static void wc_leaf_init(ra8_widget_t*              w,
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

/** @brief Assemble the nested widget tree; return false on a bind error. */
static bool wc_build_tree(void)
{
  wc_leaf_init(&s_body_kids[k_wc_w_left], &k_wc_tile_vt, &s_left, 0, 1U);
  wc_leaf_init(&s_body_kids[k_wc_w_right], &k_wc_tile_vt, &s_right, 0, 1U);
  wc_leaf_init(&s_root_kids[k_wc_w_status], &k_wc_status_vt, &s_status, (int16_t)k_wc_status_h, 0U);
  wc_leaf_init(&s_root_kids[k_wc_w_footer], &k_wc_footer_vt, nullptr, (int16_t)k_wc_footer_h, 0U);

  /* The body child is itself a panel -- the nesting the new primitive adds. */
  if (ra8_widget_panel_init(&s_root_kids[k_wc_w_body], &s_body_panel) != k_ra8_ok) {
    return false;
  }
  s_root_kids[k_wc_w_body].flex = 1U;
  return (ra8_widget_panel_init(&s_root, &s_root_panel) == k_ra8_ok);
}

/** @brief Mark every root child dirty so the next compose repaints the tree. */
static void wc_invalidate_all(ra8_widget_refresh_t refresh)
{
  for (uint16_t i = 0U; i < (uint16_t)k_wc_root_n; i++) {
    (void)ra8_widget_invalidate(&s_root_kids[i], refresh);
  }
}

/** @brief Compose the tree into the framebuffer; report the flush rect. */
static ra8_err_t wc_compose(ra8_ui_rect_t* dmg, ra8_widget_refresh_t* hint, uint16_t* dirty)
{
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_wc_fb_w, .h = (int32_t)k_wc_fb_h};
  return ra8_widget_panel_compose(&s_root, &frame, dmg, hint, dirty);
}

/* ===========================================================================
 * GLCDC panel bring-up + flush
 * ===========================================================================
 */

/** @brief Display PAL config: GLCDC LCD backend bound to the SRAM framebuffer. */
static const display_cfg_t k_wc_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_wc_fb_w,
  .height_px         = (uint16_t)k_wc_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/** @brief Bring up the GLCDC panel and bind ra8_gfx to its framebuffer. */
static bool wc_panel_up(void)
{
  if (display_init(&k_wc_display_cfg, &s_display) != k_ra8_ok) {
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
                       (uint16_t)k_wc_fb_w,
                       (uint16_t)k_wc_fb_h,
                       k_ra8_gfx_format_rgb565) == k_ra8_ok);
}

/** @brief Map a widget refresh hint to the display PAL refresh hint. */
static display_refresh_hint_t wc_map_hint(ra8_widget_refresh_t hint)
{
  if (hint == k_ra8_widget_refresh_fast) {
    return k_display_refresh_fast;
  }
  return k_display_refresh_quality;
}

/** @brief Flush only @p dmg to the panel with the mapped refresh hint. */
static void wc_flush_rect(const ra8_ui_rect_t* dmg, ra8_widget_refresh_t hint)
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
  (void)display_flush(s_display, r, wc_map_hint(hint));
}

/* ===========================================================================
 * Deterministic self-check
 * ===========================================================================
 */

/** @brief Assert the full compose: 3 dirty, full-frame quality, tiles drawn. */
static bool wc_check_full(uint32_t* out_crc, uint16_t* out_dirty)
{
  s_left.renders  = 0U;
  s_right.renders = 0U;
  wc_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wc_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != (uint16_t)k_wc_root_n) {
    return false;
  }
  if ((dmg.w != (int32_t)k_wc_fb_w) || (dmg.h != (int32_t)k_wc_fb_h)) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_quality) {
    return false;
  }
  if ((s_left.renders != 1U) || (s_right.renders != 1U)) {
    return false; /* nested body panel must have composited both tiles */
  }
  *out_crc   = wc_framebuffer_hash();
  *out_dirty = nd;
  return true;
}

/** @brief Assert the partial compose: status-only damage, tiles untouched. */
static bool wc_check_partial(uint32_t* out_crc, uint16_t* out_dirty)
{
  s_left.renders  = 0U;
  s_right.renders = 0U;
  s_status.frame++;
  (void)ra8_widget_invalidate(&s_root_kids[k_wc_w_status], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wc_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != 1U) {
    return false;
  }
  if ((dmg.x != 0) || (dmg.y != 0) || (dmg.w != (int32_t)k_wc_fb_w)) {
    return false;
  }
  if (dmg.h != (int32_t)k_wc_status_h) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_fast) {
    return false;
  }
  if ((s_left.renders != 0U) || (s_right.renders != 0U)) {
    return false; /* partial flush must NOT re-render the body */
  }
  *out_crc   = wc_framebuffer_hash();
  *out_dirty = nd;
  return true;
}

/** @brief Run the whole deterministic surface; return both composites' CRCs. */
static bool wc_selfcheck(uint32_t* crc0, uint16_t* d0, uint32_t* crc1, uint16_t* d1)
{
  if (!wc_check_full(crc0, d0)) {
    return false;
  }
  if (!wc_check_partial(crc1, d1)) {
    return false;
  }
  return (*crc0 != *crc1); /* the advanced frame counter changes the composite */
}

/* ===========================================================================
 * Boot
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time and the console. */
static void wc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    wc_fail_halt(k_wc_msg_finit, (uint32_t)sizeof(k_wc_msg_finit) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wc_fail_halt(k_wc_msg_finit, (uint32_t)sizeof(k_wc_msg_finit) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wc_fail_halt(k_wc_msg_finit, (uint32_t)sizeof(k_wc_msg_finit) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_wc_uart_baud) != k_ra8_ok) {
    wc_fail_halt(k_wc_msg_finit, (uint32_t)sizeof(k_wc_msg_finit) - 1U);
  }
}

/** @brief Emit the PASS banner once every self-check assertion holds. */
static void wc_print_banner(uint16_t d0, uint32_t crc0, uint16_t d1, uint32_t crc1)
{
  wc_print(k_wc_msg_pre, (uint32_t)sizeof(k_wc_msg_pre) - 1U);
  wc_print_uint((uint32_t)d0);
  wc_print(k_wc_msg_crc0, (uint32_t)sizeof(k_wc_msg_crc0) - 1U);
  wc_print_hex(crc0);
  wc_print(k_wc_msg_d1, (uint32_t)sizeof(k_wc_msg_d1) - 1U);
  wc_print_uint((uint32_t)d1);
  wc_print(k_wc_msg_crc1, (uint32_t)sizeof(k_wc_msg_crc1) - 1U);
  wc_print_hex(crc1);
  wc_print(k_wc_msg_tail, (uint32_t)sizeof(k_wc_msg_tail) - 1U);
  wc_print(k_wc_msg_pass, (uint32_t)sizeof(k_wc_msg_pass) - 1U);
}

/** @brief Advance the frame counter and partial-flush only the status band. */
static void wc_tick_live(void)
{
  s_status.frame++;
  (void)ra8_widget_invalidate(&s_root_kids[k_wc_w_status], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wc_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wc_flush_rect(&dmg, hint);
  }
}

/**
 * @brief App entry: bring the panel up, self-check, then run the live loop.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The PASS banner is emitted and the panel shows the composed tree; the
 *       loop partial-flushes only the status band so the panel updates its top
 *       band live (the damage-tracked path), otherwise idling.
 * @since 0.1.0
 */
void main(void)
{
  wc_setup_or_halt();
  ra8_isr_globals_enable();
  wc_print(k_wc_msg_boot, (uint32_t)sizeof(k_wc_msg_boot) - 1U);

  if (!wc_panel_up()) {
    wc_fail_halt(k_wc_msg_fpanl, (uint32_t)sizeof(k_wc_msg_fpanl) - 1U);
  }
  if (!wc_build_tree()) {
    wc_fail_halt(k_wc_msg_ftree, (uint32_t)sizeof(k_wc_msg_ftree) - 1U);
  }

  uint32_t crc0 = 0U;
  uint32_t crc1 = 0U;
  uint16_t d0   = 0U;
  uint16_t d1   = 0U;
  if (!wc_selfcheck(&crc0, &d0, &crc1, &d1)) {
    wc_fail_halt(k_wc_msg_fchk, (uint32_t)sizeof(k_wc_msg_fchk) - 1U);
  }
  wc_print_banner(d0, crc0, d1, crc1);

  /* Land on a full composite and show it live on the panel. */
  (void)ra8_gfx_clear((uint32_t)k_wc_col_clear);
  wc_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wc_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wc_flush_rect(&dmg, hint);
  }

  while (1) {
    wc_tick_live();
    ra8_delay_ms((uint32_t)k_wc_frame_ms);
  }
}
