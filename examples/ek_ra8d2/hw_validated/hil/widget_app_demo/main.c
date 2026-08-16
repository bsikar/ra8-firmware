/**
 * @file examples/ek_ra8d2/hw_validated/hil/widget_app_demo/main.c
 * @brief Interactive ra8_widget + ra8_app launcher on the live GLCDC panel.
 *
 * @details
 * The companion `widget_app` gate proves the `ra8_widget` compositor (#145) and
 * `ra8_app` framework (#146) headlessly (off-screen framebuffer + CRC). This app
 * is the **visible, interactive** counterpart: it brings the GLCDC panel up so
 * the composition is shown on `ra8_emulator`'s panel window, and drives it with the
 * physical SW1/SW2 push-buttons.
 *
 * What it shows:
 *   - Three **apps** (`Library`, `Reader`, `Settings`) register into one
 *     `ra8_app` registry. `Settings` is `removable` and wrapped in a build-time
 *     guard (`#if WA_APP_SETTINGS`) -- the "core uninstallable" pattern (#146):
 *     building with `-DWA_APP_SETTINGS=0` drops it from the registry entirely.
 *   - Each app is a **widget tree**: a status bar (fixed) over per-app content
 *     (flex) over a tab bar (fixed), laid out by `ra8_widget_layout_stack` and
 *     drawn by each widget's `render` through `ra8_gfx` into the GLCDC buffer.
 *   - **Input routing** (#145 + #146): SW1 = previous app, SW2 = next app. A
 *     press is delivered as a `button` event through `ra8_app_route_input` to the
 *     focused app, whose `on_input` selects the neighbour app and launches it.
 *     A touch is routed through `ra8_widget_dispatch` to the tab bar, which maps
 *     the hit column to an app. Switching fires the focus lifecycle
 *     (`on_leave` / `on_enter`) and re-composites -- visibly, on the panel.
 *
 * Before the interactive loop a deterministic self-check exercises the whole
 * surface (register, launch + lifecycle, distinct composites, touch dispatch,
 * button routing, status-bar-only damage) and emits one banner:
 *
 *   `widget-app-demo: apps=<n> lib=<8hex> rdr=<8hex> route=ok flush=512x44 hint=fast PASS`
 *
 * so the app doubles as a `ra8_emulator` regression gate; any failure prints a
 * `FAIL ...` banner and parks. The 512x512 RGB565 framebuffer lives in SRAM
 * (the full 1024x600 panel needs SDRAM, a separate task) and composites top-left
 * over the GLCDC background plane.
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

#include "ra8_boot_entry.h"
#include "ra8_app.h"
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
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/** @brief Build-time guard for the optional `Settings` app (#146 exclusion). */
#ifndef WA_APP_SETTINGS
/** @brief WA APP SETTINGS. */
#define WA_APP_SETTINGS (1)
#endif

/**
 * @enum wd_geom_t
 * @brief Framebuffer + band geometry (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_fb_w      = 512U, /**< Framebuffer width, pixels.             */
  k_wd_fb_h      = 512U, /**< Framebuffer height, pixels.            */
  k_wd_fb_align  = 64U,  /**< AXI-burst alignment for clean fetches. */
  k_wd_status_h  = 44U,  /**< Status-bar band height, pixels.        */
  k_wd_tabbar_h  = 56U,  /**< Tab-bar band height, pixels.           */
  k_wd_pad       = 8U,   /**< Inner text padding, pixels.            */
  k_wd_line_h    = 18U,  /**< Text line pitch (16px glyph + 2).      */
  k_wd_tab_inset = 3U,   /**< Tab cell inset from the band, pixels.  */
} wd_geom_t;

/**
 * @enum wd_color_t
 * @brief 0xRRGGBB palette (ra8_gfx packs to RGB565).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wd_col_clear     = 0x000008U, /**< Whole-frame clear (near black). */
  k_wd_col_status_bg = 0x1E2A40U, /**< Status-bar fill (slate).        */
  k_wd_col_tabbar_bg = 0x14141AU, /**< Tab-bar fill (dark).            */
  k_wd_col_tab_on    = 0x3060C0U, /**< Active tab cell (blue).         */
  k_wd_col_tab_off   = 0x303038U, /**< Idle tab cell (gray).           */
  k_wd_col_text      = 0xFFFFFFU, /**< Primary text (white).           */
  k_wd_col_dim       = 0xA0A0B0U, /**< Secondary text (gray).          */
  k_wd_col_lib_bg    = 0x102038U, /**< Library content (deep blue).    */
  k_wd_col_rdr_bg    = 0x382414U, /**< Reader content (warm brown).    */
  k_wd_col_set_bg    = 0x102818U, /**< Settings content (deep green).  */
} wd_color_t;

/**
 * @enum wd_app_id_t
 * @brief Registered app ids (launch keys).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_app_library  = 1U, /**< Library / home app.      */
  k_wd_app_reader   = 2U, /**< EPUB reader app.         */
  k_wd_app_settings = 3U, /**< Settings app (optional). */
} wd_app_id_t;

/**
 * @enum wd_btn_t
 * @brief Navigation button ids carried in a `button` widget event.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_btn_prev = 1U, /**< SW1: focus the previous app. */
  k_wd_btn_next = 2U, /**< SW2: focus the next app.     */
} wd_btn_t;

/**
 * @enum wd_widget_idx_t
 * @brief Index of each widget within an app's tree.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wd_w_status  = 0U, /**< Status bar (fixed).          */
  k_wd_w_content = 1U, /**< Content (flex).              */
  k_wd_w_tabbar  = 2U, /**< Tab bar (fixed).             */
  k_wd_w_count   = 3U, /**< Widgets per app.             */
  k_wd_box_cap   = 4U, /**< ra8_box scratch (count + 1). */
} wd_widget_idx_t;

/**
 * @enum wd_console_t
 * @brief Console baud + text-format constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wd_uart_baud   = 115200U,     /**< Console baud.            */
  k_wd_frame_ms    = 25U,         /**< Input poll period (ms).  */
  k_wd_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.  */
  k_wd_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.         */
  k_wd_hex_nibbles = 8U,          /**< Hex digits per 32-bit.   */
  k_wd_nibble_bits = 4U,          /**< Bits per hex nibble.     */
  k_wd_nibble_mask = 0x0FU,       /**< Low-nibble mask.         */
  k_wd_dec_ten     = 10U,         /**< Hex/decimal split + buf. */
} wd_console_t;

/** @brief SW1 (previous app) port pin, sourced from the board layer at init. */
static ra8_port_pin_t s_wd_sw1_pin = k_ra8_pin_none;
/** @brief SW2 (next app) port pin, sourced from the board layer at init. */
static ra8_port_pin_t s_wd_sw2_pin = k_ra8_pin_none;

/**
 * @brief GLCDC-scanned render target in SRAM, AXI-burst aligned.
 * @since 0.1.0
 */
[[gnu::aligned(
  k_wd_fb_align)]] static uint16_t s_framebuffer[(uint32_t)k_wd_fb_w * (uint32_t)k_wd_fb_h];

/**
 * @struct wa_content_t
 * @brief Per-app content widget state: a title over a list of text lines.
 * @since 0.1.0
 */
typedef struct {
  const char*        title;  /**< Heading line.               */
  const char* const* lines;  /**< Body lines (each NUL-term). */
  uint16_t           nlines; /**< Number of body lines.       */
  uint32_t           bg;     /**< Content background colour.  */
} wa_content_t;

/**
 * @struct wa_chrome_t
 * @brief Shared chrome state: the registry the status bar + tab bar read.
 * @since 0.1.0
 */
typedef struct {
  ra8_app_registry_t* reg; /**< Registry (active index + app names). */
} wa_chrome_t;

/**
 * @struct wa_app_state_t
 * @brief One app's widget tree + content + focus-lifecycle counters.
 * @since 0.1.0
 */
typedef struct {
  ra8_widget_t widgets[k_wd_w_count]; /**< [status, content, tabbar]. */
  wa_content_t content;               /**< Content widget state.      */
  uint32_t     enters;                /**< Times on_enter fired.      */
  uint32_t     leaves;                /**< Times on_leave fired.      */
} wa_app_state_t;

static ra8_app_registry_t s_reg;                      /**< The app registry.       */
static ra8_app_t*         s_slots[k_wd_app_settings]; /**< Registry storage (<=3). */
static wa_chrome_t        s_chrome = {.reg = &s_reg}; /**< Shared chrome state.    */
static wa_app_state_t     s_library;                  /**< Library app state.      */
static wa_app_state_t     s_reader;                   /**< Reader app state.       */
#if WA_APP_SETTINGS
static wa_app_state_t s_settings; /**< Settings app state (optional). */
#endif

/**
 * @var g_wd_request
 * @brief App id an input handler asked to focus, or k_ra8_app_none.
 * @note Set by a widget/app `on_input`; consumed by the main loop.
 * @warning Written from input handlers only.
 * @since 0.1.0
 */
static volatile int16_t g_wd_request = (int16_t)k_ra8_app_none;
/** @brief Live display handle once the GLCDC panel is up. */
static display_handle_t* s_display = nullptr;
/** @brief Debounce latch: true while SW1 is held (fire once per press). */
static bool s_was_sw1;
/** @brief Debounce latch: true while SW2 is held. @see s_was_sw1 */
static bool s_was_sw2;

/* Content text for each app. */
static const char* const k_wd_lib_lines[] = {
  "The Time Machine        H. G. Wells",
  "Frankenstein            M. Shelley",
  "Pride and Prejudice     J. Austen",
  "Open with SW2; browse with SW1/SW2.",
};
static const char* const k_wd_rdr_lines[] = {
  "Chapter 1",
  "The Time Traveller (for so it will be",
  "convenient to speak of him) was",
  "expounding a recondite matter to us.",
  "                              page 1/214",
};
#if WA_APP_SETTINGS
static const char* const k_wd_set_lines[] = {
  "Refresh cadence   fast (A2)",
  "Margins           normal",
  "Font              8x16 mono",
  "This app is removable (WA_APP_SETTINGS).",
};
#endif

static const uint8_t k_wd_msg_boot[]  = "widget-app-demo: boot\r\n";
static const uint8_t k_wd_msg_finit[] = "widget-app-demo: FAIL init\r\n";
static const uint8_t k_wd_msg_fpanl[] = "widget-app-demo: FAIL panel\r\n";
static const uint8_t k_wd_msg_freg[]  = "widget-app-demo: FAIL register\r\n";
static const uint8_t k_wd_msg_fchk[]  = "widget-app-demo: FAIL selfcheck\r\n";
static const uint8_t k_wd_msg_pre[]   = "widget-app-demo: apps=";
static const uint8_t k_wd_msg_lib[]   = " lib=";
static const uint8_t k_wd_msg_rdr[]   = " rdr=";
static const uint8_t k_wd_msg_route[] = " route=ok flush=512x44 hint=fast";
static const uint8_t k_wd_msg_pass[]  = " PASS\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
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
 * Widget render + input
 * ===========================================================================
 */

/** @brief Name of the focused app, or a placeholder if none. */
static const char* wd_active_name(const ra8_app_registry_t* reg)
{
  ra8_app_t* act = nullptr;
  if (ra8_app_active(reg, &act) != k_ra8_ok) {
    return "?";
  }
  if (act == nullptr) {
    return "?";
  }
  return act->name;
}

/** @brief Status-bar render: fill + active app name + the button hint. */
static void wd_status_render(ra8_widget_t* w)
{
  const wa_chrome_t* ch = (const wa_chrome_t*)w->ctx;
  (void)
    ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, (uint32_t)k_wd_col_status_bg, true);
  const int32_t tx = w->rect.x + (int32_t)k_wd_pad;
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wd_pad,
                         wd_active_name(ch->reg),
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wd_col_text,
                         (uint32_t)k_wd_col_status_bg);
  (void)ra8_gfx_text_out(tx,
                         w->rect.y + (int32_t)k_wd_pad + (int32_t)k_wd_line_h,
                         "SW1 < prev    SW2 next >    tap a tab",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_wd_col_dim,
                         (uint32_t)k_wd_col_status_bg);
}

/** @brief Content render: fill + title over the app's body lines. */
static void wd_content_render(ra8_widget_t* w)
{
  const wa_content_t* c = (const wa_content_t*)w->ctx;
  (void)ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, c->bg, true);
  const int32_t tx = w->rect.x + (int32_t)k_wd_pad;
  int32_t       ty = w->rect.y + (int32_t)k_wd_pad;
  (void)ra8_gfx_text_out(tx, ty, c->title, &ra8_gfx_font_8x16, (uint32_t)k_wd_col_text, c->bg);
  ty += (int32_t)k_wd_line_h + (int32_t)k_wd_line_h;
  for (uint16_t i = 0U; i < c->nlines; i++) {
    (void)ra8_gfx_text_out(tx, ty, c->lines[i], &ra8_gfx_font_8x16, (uint32_t)k_wd_col_dim, c->bg);
    ty += (int32_t)k_wd_line_h;
  }
}

/** @brief Tab-bar render: one cell per app, the active one highlighted. */
static void wd_tabbar_render(ra8_widget_t* w)
{
  const wa_chrome_t* ch = (const wa_chrome_t*)w->ctx;
  uint16_t           n  = 0U;
  if (ra8_app_count(ch->reg, &n) != k_ra8_ok) {
    return;
  }
  if (n == 0U) {
    return;
  }
  (void)
    ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, (uint32_t)k_wd_col_tabbar_bg, true);
  const int32_t tabw = w->rect.w / (int32_t)n;
  for (uint16_t i = 0U; i < n; i++) {
    ra8_app_t* app = nullptr;
    if (ra8_app_at(ch->reg, i, &app) != k_ra8_ok) {
      continue;
    }
    const int32_t  cx   = w->rect.x + ((int32_t)i * tabw);
    const bool     on   = ((int32_t)i == ch->reg->active);
    const uint32_t fill = on ? (uint32_t)k_wd_col_tab_on : (uint32_t)k_wd_col_tab_off;
    (void)ra8_gfx_rect(cx + (int32_t)k_wd_tab_inset,
                       w->rect.y + (int32_t)k_wd_tab_inset,
                       tabw - (2 * (int32_t)k_wd_tab_inset),
                       w->rect.h - (2 * (int32_t)k_wd_tab_inset),
                       fill,
                       true);
    (void)ra8_gfx_text_out(cx + (int32_t)k_wd_pad,
                           w->rect.y + (w->rect.h / 2) - ((int32_t)k_wd_line_h / 2),
                           app->name,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_wd_col_text,
                           fill);
  }
}

/** @brief Tab-bar input: map a touch column to an app id (request a launch). */
static bool wd_tabbar_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  const wa_chrome_t* ch = (const wa_chrome_t*)w->ctx;
  uint16_t           n  = 0U;
  if (ra8_app_count(ch->reg, &n) != k_ra8_ok) {
    return false;
  }
  if (n == 0U) {
    return false;
  }
  const int32_t tabw = w->rect.w / (int32_t)n;
  if (tabw <= 0) {
    return false;
  }
  const int32_t col = (ev->x - w->rect.x) / tabw;
  if (col < 0) {
    return false;
  }
  if (col >= (int32_t)n) {
    return false;
  }
  ra8_app_t* app = nullptr;
  if (ra8_app_at(ch->reg, (uint16_t)col, &app) != k_ra8_ok) {
    return false;
  }
  g_wd_request = (int16_t)app->id;
  return true;
}

/** @brief Status / content widgets ignore input (NULL handler never consumes). */
static const ra8_widget_vtable_t k_wd_status_vt  = {.measure  = nullptr,
                                                    .render   = wd_status_render,
                                                    .on_input = nullptr};
static const ra8_widget_vtable_t k_wd_content_vt = {.measure  = nullptr,
                                                    .render   = wd_content_render,
                                                    .on_input = nullptr};
static const ra8_widget_vtable_t k_wd_tabbar_vt  = {.measure  = nullptr,
                                                    .render   = wd_tabbar_render,
                                                    .on_input = wd_tabbar_on_input};

/* ===========================================================================
 * App lifecycle
 * ===========================================================================
 */

/** @brief Resolve the app id at (active + dir), wrapped, or k_ra8_app_none. */
static int16_t wd_neighbor_id(int32_t dir)
{
  uint16_t n = 0U;
  if (ra8_app_count(&s_reg, &n) != k_ra8_ok) {
    return (int16_t)k_ra8_app_none;
  }
  if (n == 0U) {
    return (int16_t)k_ra8_app_none;
  }
  int32_t cur = (int32_t)s_reg.active;
  if (cur < 0) {
    cur = 0;
  }
  const int32_t nx  = (cur + dir + (int32_t)n) % (int32_t)n;
  ra8_app_t*    app = nullptr;
  if (ra8_app_at(&s_reg, (uint16_t)nx, &app) != k_ra8_ok) {
    return (int16_t)k_ra8_app_none;
  }
  return (int16_t)app->id;
}

/** @brief App input: SW button -> neighbour app; touch -> widget dispatch. */
static bool wd_app_on_input(ra8_app_t* a, const ra8_widget_event_t* ev)
{
  wa_app_state_t* st = (wa_app_state_t*)a->ctx;
  if (ev->kind == k_ra8_widget_ev_button) {
    int32_t dir = 0;
    if (ev->button_id == (uint16_t)k_wd_btn_prev) {
      dir = -1;
    }
    if (ev->button_id == (uint16_t)k_wd_btn_next) {
      dir = 1;
    }
    if (dir == 0) {
      return false;
    }
    g_wd_request = wd_neighbor_id(dir);
    return true;
  }
  bool handled = false;
  (void)ra8_widget_dispatch(st->widgets, (uint16_t)k_wd_w_count, ev, &handled);
  return handled;
}

/** @brief App render: clear, lay out the tree, mark all dirty, composite. */
static void wd_app_render(const ra8_app_t* a)
{
  wa_app_state_t*     st = (wa_app_state_t*)a->ctx;
  ra8_box_t           scratch[k_wd_box_cap];
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_wd_fb_w, .h = (int32_t)k_wd_fb_h};
  (void)ra8_gfx_clear((uint32_t)k_wd_col_clear);
  (void)ra8_widget_layout_stack(st->widgets,
                                (uint16_t)k_wd_w_count,
                                &frame,
                                k_ra8_widget_axis_col,
                                0,
                                0,
                                scratch,
                                (uint16_t)k_wd_box_cap);
  for (uint16_t i = 0U; i < (uint16_t)k_wd_w_count; i++) {
    (void)ra8_widget_invalidate(&st->widgets[i], k_ra8_widget_refresh_quality);
  }
  (void)ra8_widget_render_dirty(st->widgets, (uint16_t)k_wd_w_count);
}

static void wd_app_enter(ra8_app_t* a)
{
  ((wa_app_state_t*)a->ctx)->enters++;
}

static void wd_app_leave(ra8_app_t* a)
{
  ((wa_app_state_t*)a->ctx)->leaves++;
}

/** @brief Shared app lifecycle vtable (every app behaves the same way). */
static const ra8_app_vtable_t k_wd_app_vt = {
  .init     = nullptr,
  .on_enter = wd_app_enter,
  .tick     = nullptr,
  .render   = wd_app_render,
  .on_input = wd_app_on_input,
  .on_leave = wd_app_leave,
  .deinit   = nullptr,
};

/** @brief Build one app's widget tree: status (fixed) / content (flex) / tabs (fixed). */
static void wd_state_init(wa_app_state_t*    st,
                          const char*        title,
                          const char* const* lines,
                          uint16_t           nlines,
                          uint32_t           bg)
{
  st->content.title  = title;
  st->content.lines  = lines;
  st->content.nlines = nlines;
  st->content.bg     = bg;
  st->enters         = 0U;
  st->leaves         = 0U;

  st->widgets[k_wd_w_status]         = (ra8_widget_t){};
  st->widgets[k_wd_w_status].vt      = &k_wd_status_vt;
  st->widgets[k_wd_w_status].ctx     = &s_chrome;
  st->widgets[k_wd_w_status].fixed   = (int16_t)k_wd_status_h;
  st->widgets[k_wd_w_status].visible = true;

  st->widgets[k_wd_w_content]         = (ra8_widget_t){};
  st->widgets[k_wd_w_content].vt      = &k_wd_content_vt;
  st->widgets[k_wd_w_content].ctx     = &st->content;
  st->widgets[k_wd_w_content].flex    = 1U;
  st->widgets[k_wd_w_content].visible = true;

  st->widgets[k_wd_w_tabbar]         = (ra8_widget_t){};
  st->widgets[k_wd_w_tabbar].vt      = &k_wd_tabbar_vt;
  st->widgets[k_wd_w_tabbar].ctx     = &s_chrome;
  st->widgets[k_wd_w_tabbar].fixed   = (int16_t)k_wd_tabbar_h;
  st->widgets[k_wd_w_tabbar].visible = true;
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

/** @brief Push the whole composited frame to the panel (GC16-quality). */
static void wd_flush_full(void)
{
  if (s_display == nullptr) {
    return;
  }
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_quality);
}

/** @brief Render the focused app and flush the whole panel. */
static void wd_render_active(void)
{
  (void)ra8_app_render(&s_reg);
  wd_flush_full();
}

/* ===========================================================================
 * Input polling
 * ===========================================================================
 */

/** @brief Read a user switch (active-low); true == pressed. */
static bool wd_sw_pressed(ra8_port_pin_t pin)
{
  ra8_level_t level = k_ra8_level_high;
  if (ra8_gpio_read(pin, &level) != k_ra8_ok) {
    return false;
  }
  return (level == k_ra8_level_low);
}

/** @brief Route a navigation button through the focused app (#146). */
static void wd_route_button(wd_btn_t btn)
{
  const ra8_widget_event_t ev      = {.kind      = k_ra8_widget_ev_button,
                                      .reserved  = 0U,
                                      .button_id = (uint16_t)btn,
                                      .x         = 0,
                                      .y         = 0};
  bool                     handled = false;
  (void)ra8_app_route_input(&s_reg, &ev, &handled);
}

/** @brief Poll SW1/SW2 (edge-triggered) and route a fresh press as a button. */
static void wd_poll_buttons(void)
{
  const bool sw1    = wd_sw_pressed(s_wd_sw1_pin);
  const bool sw2    = wd_sw_pressed(s_wd_sw2_pin);
  bool       fresh1 = false;
  if (sw1) {
    if (!s_was_sw1) {
      fresh1 = true;
    }
  }
  bool fresh2 = false;
  if (sw2) {
    if (!s_was_sw2) {
      fresh2 = true;
    }
  }
  s_was_sw1 = sw1;
  s_was_sw2 = sw2;
  if (fresh1) {
    wd_route_button(k_wd_btn_prev);
  }
  if (fresh2) {
    wd_route_button(k_wd_btn_next);
  }
}

/** @brief Apply a pending app-switch request: launch + re-composite if changed. */
static void wd_service_request(void)
{
  const int16_t want = g_wd_request;
  g_wd_request       = (int16_t)k_ra8_app_none;
  if (want == (int16_t)k_ra8_app_none) {
    return;
  }
  ra8_app_t* act = nullptr;
  (void)ra8_app_active(&s_reg, &act);
  if (act != nullptr) {
    if (act->id == (uint16_t)want) {
      return; /* already focused -- no flicker. */
    }
  }
  if (ra8_app_launch(&s_reg, (uint16_t)want) == k_ra8_ok) {
    wd_render_active();
  }
}

/* ===========================================================================
 * Registration + deterministic self-check
 * ===========================================================================
 */

/** @brief Initialise app state, the registry, and register every app. */
static bool wd_register_apps(ra8_app_t* library, ra8_app_t* reader, ra8_app_t* settings)
{
  wd_state_init(&s_library,
                "Library",
                k_wd_lib_lines,
                (uint16_t)(sizeof(k_wd_lib_lines) / sizeof(k_wd_lib_lines[0])),
                (uint32_t)k_wd_col_lib_bg);
  wd_state_init(&s_reader,
                "Reader",
                k_wd_rdr_lines,
                (uint16_t)(sizeof(k_wd_rdr_lines) / sizeof(k_wd_rdr_lines[0])),
                (uint32_t)k_wd_col_rdr_bg);
  *library = (ra8_app_t){.vt        = &k_wd_app_vt,
                         .ctx       = &s_library,
                         .id        = (uint16_t)k_wd_app_library,
                         .name      = "Library",
                         .removable = false};
  *reader  = (ra8_app_t){.vt        = &k_wd_app_vt,
                         .ctx       = &s_reader,
                         .id        = (uint16_t)k_wd_app_reader,
                         .name      = "Reader",
                         .removable = false};
  if (ra8_app_registry_init(&s_reg, s_slots, (uint16_t)(sizeof(s_slots) / sizeof(s_slots[0]))) !=
      k_ra8_ok) {
    return false;
  }
  if (ra8_app_register(&s_reg, library) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_register(&s_reg, reader) != k_ra8_ok) {
    return false;
  }
#if WA_APP_SETTINGS
  wd_state_init(&s_settings,
                "Settings",
                k_wd_set_lines,
                (uint16_t)(sizeof(k_wd_set_lines) / sizeof(k_wd_set_lines[0])),
                (uint32_t)k_wd_col_set_bg);
  *settings = (ra8_app_t){.vt        = &k_wd_app_vt,
                          .ctx       = &s_settings,
                          .id        = (uint16_t)k_wd_app_settings,
                          .name      = "Settings",
                          .removable = true};
  return ra8_app_register(&s_reg, settings) == k_ra8_ok;
#else
  (void)settings;
  return true;
#endif
}

/** @brief Drive a synthetic touch on the Library tab while Reader is focused. */
static bool wd_check_touch_dispatch(void)
{
  ra8_widget_t* tb = &s_reader.widgets[k_wd_w_tabbar];
  uint16_t      n  = 0U;
  if (ra8_app_count(&s_reg, &n) != k_ra8_ok) {
    return false;
  }
  if (n == 0U) {
    return false;
  }
  const int32_t            tabw = tb->rect.w / (int32_t)n;
  const ra8_widget_event_t tev  = {.kind      = k_ra8_widget_ev_touch,
                                   .reserved  = 0U,
                                   .button_id = 0U,
                                   .x         = tb->rect.x + (tabw / 2),
                                   .y         = tb->rect.y + (tb->rect.h / 2)};
  g_wd_request                  = (int16_t)k_ra8_app_none;
  bool handled                  = false;
  (void)ra8_app_route_input(&s_reg, &tev, &handled);
  if (!handled) {
    return false;
  }
  return (g_wd_request == (int16_t)k_wd_app_library);
}

/** @brief Assert status-bar-only invalidation yields exactly its rect, fast. */
static bool wd_check_damage(void)
{
  (void)ra8_widget_invalidate(&s_reader.widgets[k_wd_w_status], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg  = {};
  ra8_widget_refresh_t hint = k_ra8_widget_refresh_none;
  uint16_t             nd   = 0U;
  (void)ra8_widget_damage(s_reader.widgets, (uint16_t)k_wd_w_count, &dmg, &hint, &nd);
  if (nd != 1U) {
    return false;
  }
  if (dmg.h != (int32_t)k_wd_status_h) {
    return false;
  }
  return (hint == k_ra8_widget_refresh_fast);
}

/** @brief Run the whole deterministic surface; return the two composites' hashes. */
static bool wd_selfcheck(uint32_t* out_lib, uint32_t* out_rdr)
{
  if (ra8_app_launch(&s_reg, (uint16_t)k_wd_app_library) != k_ra8_ok) {
    return false;
  }
  (void)ra8_app_render(&s_reg);
  const uint32_t lib = wd_framebuffer_hash();
  if (ra8_app_launch(&s_reg, (uint16_t)k_wd_app_reader) != k_ra8_ok) {
    return false;
  }
  (void)ra8_app_render(&s_reg);
  const uint32_t rdr = wd_framebuffer_hash();
  if (lib == rdr) {
    return false;
  }
  if (s_library.leaves != 1U) {
    return false;
  }
  if (s_reader.enters != 1U) {
    return false;
  }
  if (s_library.enters != 1U) {
    return false;
  }
  if (!wd_check_touch_dispatch()) {
    return false;
  }
  if (!wd_check_damage()) {
    return false;
  }
  *out_lib = lib;
  *out_rdr = rdr;
  return true;
}

/* ===========================================================================
 * Boot
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time, the console, and the SW1/SW2 inputs. */
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
  (void)ra8_board_sw_pin(k_ra8_board_sw1, &s_wd_sw1_pin);
  (void)ra8_board_sw_pin(k_ra8_board_sw2, &s_wd_sw2_pin);
  (void)ra8_board_sw_init(k_ra8_board_sw1);
  (void)ra8_board_sw_init(k_ra8_board_sw2);
}

/** @brief Emit the PASS banner once the self-check holds. */
static void wd_print_banner(uint32_t lib_crc, uint32_t rdr_crc)
{
  uint16_t n = 0U;
  (void)ra8_app_count(&s_reg, &n);
  wd_print(k_wd_msg_pre, (uint32_t)sizeof(k_wd_msg_pre) - 1U);
  wd_print_uint((uint32_t)n);
  wd_print(k_wd_msg_lib, (uint32_t)sizeof(k_wd_msg_lib) - 1U);
  wd_print_hex(lib_crc);
  wd_print(k_wd_msg_rdr, (uint32_t)sizeof(k_wd_msg_rdr) - 1U);
  wd_print_hex(rdr_crc);
  wd_print(k_wd_msg_route, (uint32_t)sizeof(k_wd_msg_route) - 1U);
  wd_print(k_wd_msg_pass, (uint32_t)sizeof(k_wd_msg_pass) - 1U);
}

/**
 * @brief App entry: bring the panel up, self-check, then run the launcher.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The PASS banner is emitted and the panel shows the Library app; the
 *       loop polls SW1/SW2 to switch apps live, otherwise idling.
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

  ra8_app_t library  = {};
  ra8_app_t reader   = {};
  ra8_app_t settings = {};
  if (!wd_register_apps(&library, &reader, &settings)) {
    wd_fail_halt(k_wd_msg_freg, (uint32_t)sizeof(k_wd_msg_freg) - 1U);
  }

  uint32_t lib_crc = 0U;
  uint32_t rdr_crc = 0U;
  if (!wd_selfcheck(&lib_crc, &rdr_crc)) {
    wd_fail_halt(k_wd_msg_fchk, (uint32_t)sizeof(k_wd_msg_fchk) - 1U);
  }
  wd_print_banner(lib_crc, rdr_crc);

  /* Land on the Library app and show it live on the panel. */
  g_wd_request = (int16_t)k_ra8_app_none;
  (void)ra8_app_launch(&s_reg, (uint16_t)k_wd_app_library);
  wd_render_active();

  while (1) {
    wd_poll_buttons();
    wd_service_request();
    ra8_delay_ms((uint32_t)k_wd_frame_ms);
  }
}
