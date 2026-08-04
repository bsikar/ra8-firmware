/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/widget_kit_demo/main.c
 * @brief Concrete ra8_widget leaf widgets composited on the GLCDC panel (#145).
 *
 * @details
 * `widget_compose_demo` proved the ::ra8_widget_panel compositor with anonymous
 * tile leaves. This app is the Phase-2 demonstration of the **concrete leaf
 * widgets** the panel can now composite: ::ra8_widget_label_t (a background fill
 * plus aligned text) and ::ra8_widget_button_t (a bordered face + label that
 * latches on a tap and reports the press through `on_input`). Both draw through
 * an injected ::ra8_widget_paint_t backend wired to `ra8_gfx`, so `ra8_widget`
 * itself stays graphics free.
 *
 * The tree (dwm-style, each piece opt-in):
 *
 *   root (column panel)
 *   |- title  (LABEL,  fixed) -- centred heading
 *   |- body   (ROW PANEL)     -- a nested panel ...
 *   |   |- button A (BUTTON, flex) -- "Prev"
 *   |   |- button B (BUTTON, flex) -- "Next"
 *   |- footer (LABEL,  fixed) -- a left-aligned hint line
 *
 * One ::ra8_widget_panel_compose call lays the children out, computes the
 * minimal damage rect + refresh hint, and renders only the dirty children -- a
 * dirty nested panel repaints its whole subtree.
 *
 * A deterministic self-check runs before the live loop:
 *   - **Full compose**: clear, mark the whole tree dirty -> 3 dirty root
 *     children, damage covers the full 512x512 frame, quality hint; the label
 *     and button leaves all paint (a stable composite CRC).
 *   - **Press**: a synthetic touch routed through the root panel lands on
 *     button A, which latches (`presses == 1`, `pressed == true`) and -- via its
 *     `on_press` callback -- invalidates only the body band.
 *   - **Partial compose**: exactly 1 dirty root child (the body), damage is just
 *     the `512x440` body rect with the fast hint, and the composite CRC changes
 *     (button A now shows its pressed face). This is the issue #145 partial-flush
 *     acceptance, driven by the concrete button widget.
 *
 * It then emits one banner so the app doubles as a ra8_emulator gate:
 *
 *   `widget-kit-demo: dirty0=3 crc0=<8hex> press=1 dirty1=1 crc1=<8hex> flush=512x440 hint=fast PASS`
 *
 * Any failed assertion prints `FAIL ...` and parks. After the banner the loop
 * alternately taps button A / button B and partial-composes the body band, so
 * the panel visibly toggles a button live (the damage-tracked A2 path).
 *
 *
 * [Ring 6 / APP] {World: S}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
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
 * @enum wk_geom_t
 * @brief Framebuffer + band geometry (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wk_fb_w       = 512U, /**< Framebuffer width, pixels.                */
  k_wk_fb_h       = 512U, /**< Framebuffer height, pixels.               */
  k_wk_fb_align   = 64U,  /**< AXI-burst alignment for clean fetches.    */
  k_wk_title_h    = 44U,  /**< Title-label band height, pixels.          */
  k_wk_footer_h   = 28U,  /**< Footer-label band height, pixels.         */
  k_wk_body_h     = 440U, /**< Body band height (fb_h - title - footer). */
  k_wk_pad        = 6U,   /**< Inner text padding, pixels.               */
  k_wk_btn_gap    = 8U,   /**< Gap between the two body buttons.         */
  k_wk_btn_border = 2U,   /**< Button outline thickness, pixels.         */
} wk_geom_t;

/**
 * @enum wk_color_t
 * @brief 0xRRGGBB palette (ra8_gfx packs to RGB565).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wk_col_clear     = 0x000010U, /**< Whole-frame clear (near black).  */
  k_wk_col_title_bg  = 0x182438U, /**< Title-label fill (slate).        */
  k_wk_col_footer_bg = 0x101018U, /**< Footer-label fill (dark).        */
  k_wk_col_text      = 0xFFFFFFU, /**< Primary text (white).            */
  k_wk_col_dim       = 0xA8A8B8U, /**< Secondary text (gray).           */
  k_wk_col_btn_a     = 0x204060U, /**< Button A face, released (blue).  */
  k_wk_col_btn_a_hi  = 0x3A6CB0U, /**< Button A face, pressed (bright). */
  k_wk_col_btn_b     = 0x603020U, /**< Button B face, released (brown). */
  k_wk_col_btn_b_hi  = 0xB06030U, /**< Button B face, pressed (warm).   */
  k_wk_col_btn_brd   = 0x05080CU, /**< Button outline (near black).     */
} wk_color_t;

/**
 * @enum wk_widget_idx_t
 * @brief Index + action id of each widget within its parent array.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wk_w_title  = 0U,  /**< Root child 0: title label (fixed).  */
  k_wk_w_body   = 1U,  /**< Root child 1: nested body panel.    */
  k_wk_w_footer = 2U,  /**< Root child 2: footer label (fixed). */
  k_wk_root_n   = 3U,  /**< Root child count.                   */
  k_wk_root_cap = 4U,  /**< Root ra8_box scratch (count + 1).   */
  k_wk_w_btn_a  = 0U,  /**< Body child 0: button A (flex).      */
  k_wk_w_btn_b  = 1U,  /**< Body child 1: button B (flex).      */
  k_wk_body_n   = 2U,  /**< Body child count.                   */
  k_wk_body_cap = 3U,  /**< Body ra8_box scratch (count + 1).   */
  k_wk_act_a    = 10U, /**< Button A hit-routing action id.     */
  k_wk_act_b    = 11U, /**< Button B hit-routing action id.     */
} wk_widget_idx_t;

/**
 * @enum wk_touch_t
 * @brief Synthetic touch points used by the self-check + live loop.
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_wk_touch_ax = 120, /**< X inside button A (left half of body).  */
  k_wk_touch_bx = 392, /**< X inside button B (right half of body). */
  k_wk_touch_y  = 264, /**< Y at the body vertical centre.          */
} wk_touch_t;

/**
 * @enum wk_console_t
 * @brief Console baud + text-format constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wk_uart_baud   = 115200U,     /**< Console baud.              */
  k_wk_frame_ms    = 500U,        /**< Live repaint cadence (ms). */
  k_wk_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.    */
  k_wk_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.           */
  k_wk_hex_nibbles = 8U,          /**< Hex digits per 32-bit.     */
  k_wk_nibble_bits = 4U,          /**< Bits per hex nibble.       */
  k_wk_nibble_mask = 0x0FU,       /**< Low-nibble mask.           */
  k_wk_dec_ten     = 10U,         /**< Decimal radix.             */
} wk_console_t;

/**
 * @brief GLCDC-scanned render target in SRAM, AXI-burst aligned.
 * @since 0.1.0
 */
[[gnu::aligned(
  k_wk_fb_align)]] static uint16_t s_framebuffer[(uint32_t)k_wk_fb_w * (uint32_t)k_wk_fb_h];

/* ===========================================================================
 * Paint backend: ra8_widget_paint_t -> ra8_gfx (the only ra8_gfx call sites)
 * ===========================================================================
 */

/** @brief Paint backend fill: a solid ra8_gfx rectangle. */
static void wk_paint_fill(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)ra8_gfx_rect(x, y, w, h, color, true);
}

/** @brief Paint backend text: an 8x16 ra8_gfx string. */
static void
wk_paint_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)ra8_gfx_text_out(x, y, str, &ra8_gfx_font_8x16, fg, bg);
}

/** @brief Paint backend measure: 8x16 ra8_gfx string size (for centring). */
static void wk_paint_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  uint32_t pw = 0U;
  uint32_t ph = 0U;
  (void)ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &pw, &ph);
  *out_w = (int32_t)pw;
  *out_h = (int32_t)ph;
}

/** @brief The shared paint backend every leaf widget draws through. */
static const ra8_widget_paint_t k_wk_paint = {
  .user      = nullptr,
  .fill_rect = wk_paint_fill,
  .draw_text = wk_paint_text,
  .text_size = wk_paint_size,
};

/* ===========================================================================
 * Widget instances (caller-owned, no allocation)
 * ===========================================================================
 */

static void wk_on_button_press(ra8_widget_t* w);

/** @brief Title label: a centred heading over the slate band. */
static ra8_widget_label_t s_title = {.paint    = &k_wk_paint,
                                     .text     = "widget_kit_demo",
                                     .fg       = (uint32_t)k_wk_col_text,
                                     .bg       = (uint32_t)k_wk_col_title_bg,
                                     .pad      = (int16_t)k_wk_pad,
                                     .align    = k_ra8_widget_align_center,
                                     .reserved = 0U};

/** @brief Footer label: a left-aligned hint line. */
static ra8_widget_label_t s_footer = {.paint    = &k_wk_paint,
                                      .text     = "label + button leaves on ra8_widget_panel",
                                      .fg       = (uint32_t)k_wk_col_dim,
                                      .bg       = (uint32_t)k_wk_col_footer_bg,
                                      .pad      = (int16_t)k_wk_pad,
                                      .align    = k_ra8_widget_align_left,
                                      .reserved = 0U};

/** @brief Button A ("Prev"): blue face, latches + flushes the body on tap. */
static ra8_widget_button_t s_btn_a = {.paint        = &k_wk_paint,
                                      .text         = "Prev",
                                      .on_press     = wk_on_button_press,
                                      .fg           = (uint32_t)k_wk_col_text,
                                      .face         = (uint32_t)k_wk_col_btn_a,
                                      .face_pressed = (uint32_t)k_wk_col_btn_a_hi,
                                      .border       = (uint32_t)k_wk_col_btn_brd,
                                      .presses      = 0U,
                                      .pad          = (int16_t)k_wk_pad,
                                      .border_w     = (int16_t)k_wk_btn_border,
                                      .align        = k_ra8_widget_align_center,
                                      .pressed      = false,
                                      .reserved     = 0U};

/** @brief Button B ("Next"): brown face, latches + flushes the body on tap. */
static ra8_widget_button_t s_btn_b = {.paint        = &k_wk_paint,
                                      .text         = "Next",
                                      .on_press     = wk_on_button_press,
                                      .fg           = (uint32_t)k_wk_col_text,
                                      .face         = (uint32_t)k_wk_col_btn_b,
                                      .face_pressed = (uint32_t)k_wk_col_btn_b_hi,
                                      .border       = (uint32_t)k_wk_col_btn_brd,
                                      .presses      = 0U,
                                      .pad          = (int16_t)k_wk_pad,
                                      .border_w     = (int16_t)k_wk_btn_border,
                                      .align        = k_ra8_widget_align_center,
                                      .pressed      = false,
                                      .reserved     = 0U};

static ra8_widget_t s_root_kids[k_wk_root_n];      /**< [title, body, footer]. */
static ra8_widget_t s_body_kids[k_wk_body_n];      /**< [button A, button B].  */
static ra8_box_t    s_body_scratch[k_wk_body_cap]; /**< Body layout scratch.   */
static ra8_box_t    s_root_scratch[k_wk_root_cap]; /**< Root layout scratch.   */

/** @brief Body row-panel descriptor (the nested panel of buttons). */
static ra8_widget_panel_t s_body_panel = {.children    = s_body_kids,
                                          .box_scratch = s_body_scratch,
                                          .count       = (uint16_t)k_wk_body_n,
                                          .box_cap     = (uint16_t)k_wk_body_cap,
                                          .gap         = (int16_t)k_wk_btn_gap,
                                          .pad         = 0,
                                          .axis        = k_ra8_widget_axis_row,
                                          .reserved    = 0U};

/** @brief Root column-panel descriptor. */
static ra8_widget_panel_t s_root_panel = {.children    = s_root_kids,
                                          .box_scratch = s_root_scratch,
                                          .count       = (uint16_t)k_wk_root_n,
                                          .box_cap     = (uint16_t)k_wk_root_cap,
                                          .gap         = 0,
                                          .pad         = 0,
                                          .axis        = k_ra8_widget_axis_col,
                                          .reserved    = 0U};

/** @brief The root panel widget (composed each frame). */
static ra8_widget_t s_root;
/** @brief Live display handle once the GLCDC panel is up. */
static display_handle_t* s_display = nullptr;
/** @brief Live-loop tick used to alternate which button is tapped. */
static uint32_t s_tick = 0U;

static const uint8_t k_wk_msg_boot[]  = "widget-kit-demo: boot\r\n";
static const uint8_t k_wk_msg_finit[] = "widget-kit-demo: FAIL init\r\n";
static const uint8_t k_wk_msg_fpanl[] = "widget-kit-demo: FAIL panel\r\n";
static const uint8_t k_wk_msg_ftree[] = "widget-kit-demo: FAIL tree\r\n";
static const uint8_t k_wk_msg_fchk[]  = "widget-kit-demo: FAIL selfcheck\r\n";
static const uint8_t k_wk_msg_pre[]   = "widget-kit-demo: dirty0=3 crc0=";
static const uint8_t k_wk_msg_mid[]   = " press=1 dirty1=1 crc1=";
static const uint8_t k_wk_msg_tail[]  = " flush=512x440 hint=fast PASS\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the board console. */
static void wk_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print a fail banner and park forever in WFI. */
static void wk_fail_halt(const uint8_t* msg, uint32_t len)
{
  wk_print(msg, len);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print @p value as 8 uppercase hex digits. */
static void wk_print_hex(uint32_t value)
{
  uint8_t buf[k_wk_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_wk_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_wk_hex_nibbles - 1U - i) * (uint32_t)k_wk_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_wk_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_wk_dec_ten) ? ('0' + nib) : ('A' + (nib - k_wk_dec_ten)));
  }
  wk_print(buf, (uint32_t)k_wk_hex_nibbles);
}

/** @brief FNV-1a-32 over the composited framebuffer bytes. */
static uint32_t wk_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_wk_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_wk_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Tree assembly + compose
 * ===========================================================================
 */

/** @brief Mark the body band dirty so a button press flushes only that rect. */
static void wk_on_button_press(ra8_widget_t* w)
{
  (void)w;
  (void)ra8_widget_invalidate(&s_root_kids[k_wk_w_body], k_ra8_widget_refresh_fast);
}

/** @brief Assemble the nested widget tree; return false on a bind error. */
static bool wk_build_tree(void)
{
  if (ra8_widget_label_init(&s_root_kids[k_wk_w_title], &s_title) != k_ra8_ok) {
    return false;
  }
  s_root_kids[k_wk_w_title].fixed = (int16_t)k_wk_title_h;
  if (ra8_widget_label_init(&s_root_kids[k_wk_w_footer], &s_footer) != k_ra8_ok) {
    return false;
  }
  s_root_kids[k_wk_w_footer].fixed = (int16_t)k_wk_footer_h;
  if (ra8_widget_button_init(&s_body_kids[k_wk_w_btn_a], &s_btn_a) != k_ra8_ok) {
    return false;
  }
  s_body_kids[k_wk_w_btn_a].flex      = 1U;
  s_body_kids[k_wk_w_btn_a].action_id = (uint16_t)k_wk_act_a;
  if (ra8_widget_button_init(&s_body_kids[k_wk_w_btn_b], &s_btn_b) != k_ra8_ok) {
    return false;
  }
  s_body_kids[k_wk_w_btn_b].flex      = 1U;
  s_body_kids[k_wk_w_btn_b].action_id = (uint16_t)k_wk_act_b;
  if (ra8_widget_panel_init(&s_root_kids[k_wk_w_body], &s_body_panel) != k_ra8_ok) {
    return false;
  }
  s_root_kids[k_wk_w_body].flex = 1U;
  return (ra8_widget_panel_init(&s_root, &s_root_panel) == k_ra8_ok);
}

/** @brief Mark every root child dirty so the next compose repaints the tree. */
static void wk_invalidate_all(ra8_widget_refresh_t refresh)
{
  for (uint16_t i = 0U; i < (uint16_t)k_wk_root_n; i++) {
    (void)ra8_widget_invalidate(&s_root_kids[i], refresh);
  }
}

/** @brief Compose the tree into the framebuffer; report the flush rect. */
static ra8_err_t wk_compose(ra8_ui_rect_t* dmg, ra8_widget_refresh_t* hint, uint16_t* dirty)
{
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_wk_fb_w, .h = (int32_t)k_wk_fb_h};
  return ra8_widget_panel_compose(&s_root, &frame, dmg, hint, dirty);
}

/** @brief Route a synthetic touch through the root panel to a leaf widget. */
static bool wk_route_touch(int32_t x, int32_t y)
{
  const ra8_widget_event_t ev = {.kind = k_ra8_widget_ev_touch, .x = x, .y = y};
  if (s_root.vt == nullptr) {
    return false;
  }
  if (s_root.vt->on_input == nullptr) {
    return false;
  }
  return s_root.vt->on_input(&s_root, &ev);
}

/* ===========================================================================
 * GLCDC panel bring-up + flush
 * ===========================================================================
 */

/** @brief Display PAL config: GLCDC LCD backend bound to the SRAM framebuffer. */
static const display_cfg_t k_wk_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_wk_fb_w,
  .height_px         = (uint16_t)k_wk_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
};

/** @brief Bring up the GLCDC panel and bind ra8_gfx to its framebuffer. */
static bool wk_panel_up(void)
{
  if (display_init(&k_wk_display_cfg, &s_display) != k_ra8_ok) {
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
                       (uint16_t)k_wk_fb_w,
                       (uint16_t)k_wk_fb_h,
                       k_ra8_gfx_format_rgb565) == k_ra8_ok);
}

/** @brief Map a widget refresh hint to the display PAL refresh hint. */
static display_refresh_hint_t wk_map_hint(ra8_widget_refresh_t hint)
{
  if (hint == k_ra8_widget_refresh_fast) {
    return k_display_refresh_fast;
  }
  return k_display_refresh_quality;
}

/** @brief Flush only @p dmg to the panel with the mapped refresh hint. */
static void wk_flush_rect(const ra8_ui_rect_t* dmg, ra8_widget_refresh_t hint)
{
  if (s_display == nullptr) {
    return;
  }
  if (dmg->w <= 0) {
    return;
  }
  if (dmg->h <= 0) {
    return;
  }
  const display_rect_t r = {.x = (uint16_t)dmg->x,
                            .y = (uint16_t)dmg->y,
                            .w = (uint16_t)dmg->w,
                            .h = (uint16_t)dmg->h};
  (void)display_flush(s_display, r, wk_map_hint(hint));
}

/* ===========================================================================
 * Deterministic self-check
 * ===========================================================================
 */

/** @brief Assert the full compose: clear, 3 dirty, full-frame quality. */
static bool wk_check_full(uint32_t* out_crc)
{
  (void)ra8_gfx_clear((uint32_t)k_wk_col_clear);
  wk_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wk_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != (uint16_t)k_wk_root_n) {
    return false;
  }
  if (dmg.w != (int32_t)k_wk_fb_w) {
    return false;
  }
  if (dmg.h != (int32_t)k_wk_fb_h) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_quality) {
    return false;
  }
  *out_crc = wk_framebuffer_hash();
  return true;
}

/** @brief Assert button A latches a routed touch + invalidates the body. */
static bool wk_check_press(void)
{
  if (!wk_route_touch((int32_t)k_wk_touch_ax, (int32_t)k_wk_touch_y)) {
    return false;
  }
  if (s_btn_a.presses != 1U) {
    return false;
  }
  return s_btn_a.pressed;
}

/** @brief Assert the partial compose: body-only damage, fast hint. */
static bool wk_check_partial(uint32_t* out_crc)
{
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wk_compose(&dmg, &hint, &nd) != k_ra8_ok) {
    return false;
  }
  if (nd != 1U) {
    return false;
  }
  if (dmg.y != (int32_t)k_wk_title_h) {
    return false;
  }
  if (dmg.h != (int32_t)k_wk_body_h) {
    return false;
  }
  if (hint != k_ra8_widget_refresh_fast) {
    return false;
  }
  *out_crc = wk_framebuffer_hash();
  return true;
}

/** @brief Run the whole deterministic surface; return both composites' CRCs. */
static bool wk_selfcheck(uint32_t* crc0, uint32_t* crc1)
{
  if (!wk_check_full(crc0)) {
    return false;
  }
  if (!wk_check_press()) {
    return false;
  }
  if (!wk_check_partial(crc1)) {
    return false;
  }
  return (*crc0 != *crc1); /* button A's pressed face changes the composite */
}

/* ===========================================================================
 * Boot
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time and the console. */
static void wk_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    wk_fail_halt(k_wk_msg_finit, (uint32_t)sizeof(k_wk_msg_finit) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wk_fail_halt(k_wk_msg_finit, (uint32_t)sizeof(k_wk_msg_finit) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wk_fail_halt(k_wk_msg_finit, (uint32_t)sizeof(k_wk_msg_finit) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_wk_uart_baud) != k_ra8_ok) {
    wk_fail_halt(k_wk_msg_finit, (uint32_t)sizeof(k_wk_msg_finit) - 1U);
  }
}

/** @brief Emit the PASS banner once every self-check assertion holds. */
static void wk_print_banner(uint32_t crc0, uint32_t crc1)
{
  wk_print(k_wk_msg_pre, (uint32_t)sizeof(k_wk_msg_pre) - 1U);
  wk_print_hex(crc0);
  wk_print(k_wk_msg_mid, (uint32_t)sizeof(k_wk_msg_mid) - 1U);
  wk_print_hex(crc1);
  wk_print(k_wk_msg_tail, (uint32_t)sizeof(k_wk_msg_tail) - 1U);
}

/** @brief Alternate-tap a button and partial-flush only the body band. */
static void wk_tick_live(void)
{
  const int32_t tx = ((s_tick & 1U) == 0U) ? (int32_t)k_wk_touch_ax : (int32_t)k_wk_touch_bx;
  s_tick++;
  (void)wk_route_touch(tx, (int32_t)k_wk_touch_y);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wk_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wk_flush_rect(&dmg, hint);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring the panel up, self-check, then run the live loop.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The PASS banner is emitted and the panel shows the composed widget kit;
 *       the loop alternately taps a button and partial-flushes the body band so
 *       the panel updates a button live (the damage-tracked path).
 * @since 0.1.0
 */
int32_t main(void)
{
  wk_setup_or_halt();
  ra8_isr_globals_enable();
  wk_print(k_wk_msg_boot, (uint32_t)sizeof(k_wk_msg_boot) - 1U);

  if (!wk_panel_up()) {
    wk_fail_halt(k_wk_msg_fpanl, (uint32_t)sizeof(k_wk_msg_fpanl) - 1U);
  }
  if (!wk_build_tree()) {
    wk_fail_halt(k_wk_msg_ftree, (uint32_t)sizeof(k_wk_msg_ftree) - 1U);
  }

  uint32_t crc0 = 0U;
  uint32_t crc1 = 0U;
  if (!wk_selfcheck(&crc0, &crc1)) {
    wk_fail_halt(k_wk_msg_fchk, (uint32_t)sizeof(k_wk_msg_fchk) - 1U);
  }
  wk_print_banner(crc0, crc1);

  /* Land on a clean full composite and show it live on the panel. */
  (void)ra8_gfx_clear((uint32_t)k_wk_col_clear);
  wk_invalidate_all(k_ra8_widget_refresh_quality);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  if (wk_compose(&dmg, &hint, &nd) == k_ra8_ok) {
    wk_flush_rect(&dmg, hint);
  }

  while (1) {
    wk_tick_live();
    ra8_delay_ms((uint32_t)k_wk_frame_ms);
  }
}
#pragma GCC diagnostic pop
