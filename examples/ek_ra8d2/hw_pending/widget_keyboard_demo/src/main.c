/**
 * @file examples/ek_ra8d2/hw_pending/widget_keyboard_demo/src/main.c
 * @brief ra8_widget_keyboard demo: the published on-screen-keyboard widget
 *        driven against the real ra8_keyboard engine, self-checking.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * `ra8_widget_keyboard` is a leaf widget that draws a key grid through the
 * injected ::ra8_widget_paint_t backend and routes a tap through the injected
 * ::ra8_widget_keyboard_ops_t seam (`count` / `key_info` / `hit` / `apply`).
 * Until this app it was named only by its own header, its own translation unit,
 * and one host test whose seam is a recording mock -- so *both* sides of the
 * pairing were fakes and nothing checked the widget against the real engine
 * (issue #1336). This app is that consumer: it binds the seam to `ra8_kbd_hit`
 * / `ra8_kbd_apply` / `ra8_kbd_key_glyph` over a real ::ra8_kbd_layout_t and
 * ::ra8_kbd_text_t, and asserts the whole route in six legs:
 *
 *   1. ::ra8_widget_keyboard_init binds the published vtable, ctx and visibility.
 *   2. One ::ra8_widget_panel_compose over the tree lays the keyboard out inside
 *      the frame, reports a full-frame quality damage rect, and paints a
 *      background fill plus one face per key through the paint seam.
 *   3. Taps on the `r` and `a` keys, routed by ::ra8_widget_dispatch through the
 *      root panel, type into the *engine's* buffer: `s_text.buf == "ra"`.
 *   4. The one-shot SHIFT lands in the engine: SHIFT then `b` appends `'B'` and
 *      leaves SHIFT cleared; backspace then removes it again.
 *   5. A tap outside the frame is consumed by nobody and leaves the buffer as it
 *      was, so a miss cannot pass as a keystroke.
 *   6. RETURN sets `committed`, fires `on_commit` exactly once, and the widget's
 *      self-invalidate makes the next compose report exactly the keyboard's own
 *      rect as damage.
 *
 * The paint backend here is a *recording* one (it counts fills and texts and
 * touches no framebuffer), which is what keeps the app deterministic and
 * board-independent while still exercising the real widget and the real engine:
 * the thing that was never covered is the widget-to-engine pairing, not the
 * pixels. Observable over the SCI8 / J-Link OB VCOM console; a good run prints
 * `widget_keyboard_demo: keyboard widget PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_box.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_keyboard.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_sci.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_keyboard.h"

/**
 * @enum wkd_const_t
 * @brief Console, frame, and tree knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent. The frame is the EK-RA8D2 parallel-panel geometry; the tree
 *          is one root panel holding the keyboard leaf.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wkd_uart_chan  = 8U,          /**< SCI8 J-Link OB console.            */
  k_wkd_frame_w    = 480U,        /**< Frame width (pixels).              */
  k_wkd_frame_h    = 272U,        /**< Frame height (pixels).             */
  k_wkd_kid_count  = 1U,          /**< Root panel children (keyboard).    */
  k_wkd_box_cap    = 2U,          /**< Layout scratch: children + 1.      */
  k_wkd_glyph_w    = 8U,          /**< Recorded text metric, px per char. */
  k_wkd_glyph_h    = 16U,         /**< Recorded text metric, line height. */
  k_wkd_bg         = 0x00202020U, /**< Keyboard background, 0xRRGGBB.     */
  k_wkd_key_face   = 0x00404040U, /**< Key face fill, 0xRRGGBB.           */
  k_wkd_key_border = 0x00101010U, /**< Key border, 0xRRGGBB.              */
  k_wkd_key_fg     = 0x00FFFFFFU, /**< Key glyph / label, 0xRRGGBB.       */
  k_wkd_border_w   = 1U,          /**< Key border thickness (pixels).     */
  k_wkd_off_grid   = 10000,       /**< X/Y far outside the frame.         */
} wkd_const_t;

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

static ra8_kbd_layout_t s_kb;   /**< Real key grid (the engine's state). */
static ra8_kbd_text_t   s_text; /**< Real typed-query buffer.            */

static uint32_t s_fills;   /**< Recorded fill_rect calls.             */
static uint32_t s_texts;   /**< Recorded draw_text calls.             */
static uint32_t s_commits; /**< on_commit invocations (commit edges). */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Record a rectangle fill instead of drawing one.
 *
 * @param[in] user  Unused backend handle.
 * @param[in] x     Left edge (pixels).
 * @param[in] y     Top edge (pixels).
 * @param[in] w     Width (pixels).
 * @param[in] h     Height (pixels).
 * @param[in] color Fill colour, 0xRRGGBB.
 * @return void
 * @pre None.
 * @post @ref s_fills is incremented.
 * @note Touches no framebuffer, so the app is board-independent.
 * @since 0.1.0
 */
static void wkd_fill(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)color;
  s_fills++;
}

/**
 * @brief Record a text draw instead of drawing one.
 *
 * @param[in] user Unused backend handle.
 * @param[in] x    Text origin X (pixels).
 * @param[in] y    Text origin Y (pixels).
 * @param[in] str  NUL-terminated string the widget wanted drawn.
 * @param[in] fg   Foreground colour, 0xRRGGBB.
 * @param[in] bg   Background colour, 0xRRGGBB.
 * @return void
 * @pre None.
 * @post @ref s_texts is incremented.
 * @since 0.1.0
 */
static void wkd_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)x;
  (void)y;
  (void)str;
  (void)fg;
  (void)bg;
  s_texts++;
}

/**
 * @brief Report a fixed-metric text size so key labels can be centred.
 *
 * @param[in]  user  Unused backend handle.
 * @param[in]  str   String to measure (NULL measures as empty).
 * @param[out] out_w Receives the pixel width.
 * @param[out] out_h Receives the pixel height.
 * @return void
 * @pre @p out_w and @p out_h are non-NULL.
 * @post Both outputs are set; no state is modified.
 * @note Matches the 8x16 console font the real renderer uses.
 * @since 0.1.0
 */
static void wkd_text_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  if ((out_w == nullptr) || (out_h == nullptr)) {
    return;
  }
  const size_t len = (str == nullptr) ? 0U : strlen(str);
  *out_w           = (int32_t)len * (int32_t)k_wkd_glyph_w;
  *out_h           = (int32_t)k_wkd_glyph_h;
}

/** @brief Recording paint backend the keyboard widget draws through. */
static const ra8_widget_paint_t k_wkd_paint = {
  .user      = nullptr,
  .fill_rect = wkd_fill,
  .draw_text = wkd_text,
  .text_size = wkd_text_size,
};

/**
 * @brief Seam: number of keys in the engine's active layer.
 *
 * @param[in] user The ::ra8_kbd_layout_t handle bound into the seam.
 * @return uint8_t Key count, or 0 when @p user is NULL.
 * @pre None.
 * @post No state is modified.
 * @since 0.1.0
 */
static uint8_t wkd_ops_count(void* user)
{
  const ra8_kbd_layout_t* kb = (const ra8_kbd_layout_t*)user;
  return (kb == nullptr) ? 0U : kb->count;
}

/**
 * @brief Short label for a non-character key.
 *
 * @param[in] kind Key behaviour reported by the engine.
 * @return const char* Label text, or NULL for a character key.
 * @pre None.
 * @post No state is modified.
 * @since 0.1.0
 */
static const char* wkd_special_label(ra8_kbd_key_kind_t kind)
{
  switch (kind) {
    case k_ra8_kbd_key_space:
      return "SPACE";
    case k_ra8_kbd_key_backspace:
      return "DEL";
    case k_ra8_kbd_key_enter:
      return "GO";
    case k_ra8_kbd_key_shift:
      return "SHIFT";
    case k_ra8_kbd_key_layer:
      return "123";
    case k_ra8_kbd_key_char:
    default:
      return nullptr;
  }
}

/**
 * @brief Seam: describe key @p idx so the widget can draw it.
 *
 * @param[in]  user The ::ra8_kbd_layout_t handle bound into the seam.
 * @param[in]  idx  Key index.
 * @param[out] out  Receives the key's rect plus its glyph or label.
 * @return void
 * @pre @p out is non-NULL.
 * @post On an out-of-range index @p out is left empty rather than stale.
 * @note The glyph comes from ::ra8_kbd_key_glyph, so the drawn case follows
 *       the engine's live SHIFT state.
 * @since 0.1.0
 */
static void wkd_ops_key_info(void* user, uint8_t idx, ra8_widget_key_info_t* out)
{
  const ra8_kbd_layout_t* kb = (const ra8_kbd_layout_t*)user;
  if (out == nullptr) {
    return;
  }
  const ra8_widget_key_info_t empty = {};
  *out                              = empty;
  if ((kb == nullptr) || (idx >= kb->count)) {
    return;
  }
  out->rect        = kb->keys[idx].rect;
  const char glyph = ra8_kbd_key_glyph(kb, idx);
  out->glyph       = glyph;
  out->label       = (glyph == '\0') ? wkd_special_label(kb->keys[idx].kind) : nullptr;
}

/**
 * @brief Seam: map a tap to a key index through the real engine.
 *
 * @param[in] user The ::ra8_kbd_layout_t handle bound into the seam.
 * @param[in] x    Tap X (pixels).
 * @param[in] y    Tap Y (pixels).
 * @return uint8_t Key index, or ::k_ra8_widget_key_no_hit for a miss.
 * @pre None.
 * @post No state is modified.
 * @note Translates the engine's ::k_ra8_kbd_no_hit into the widget's own
 *       sentinel rather than assuming the two numbers stay equal.
 * @since 0.1.0
 */
static uint8_t wkd_ops_hit(void* user, int32_t x, int32_t y)
{
  const ra8_kbd_layout_t* kb = (const ra8_kbd_layout_t*)user;
  const uint8_t           hit = ra8_kbd_hit(kb, x, y);
  return (hit == (uint8_t)k_ra8_kbd_no_hit) ? (uint8_t)k_ra8_widget_key_no_hit : hit;
}

/**
 * @brief Seam: apply key @p idx to the engine and report the commit state.
 *
 * @param[in] user The ::ra8_kbd_layout_t handle bound into the seam.
 * @param[in] idx  Key index from ::wkd_ops_hit.
 * @return bool True iff the query is now committed.
 * @pre None.
 * @post The engine's text buffer, SHIFT and layer reflect the key.
 * @since 0.1.0
 */
static bool wkd_ops_apply(void* user, uint8_t idx)
{
  ra8_kbd_layout_t* kb = (ra8_kbd_layout_t*)user;
  if (kb == nullptr) {
    return false;
  }
  (void)ra8_kbd_apply(&s_text, kb, idx);
  return s_text.committed;
}

/** @brief The engine seam the keyboard widget routes and draws through. */
static ra8_widget_keyboard_ops_t s_ops = {
  .user     = &s_kb,
  .count    = wkd_ops_count,
  .key_info = wkd_ops_key_info,
  .hit      = wkd_ops_hit,
  .apply    = wkd_ops_apply,
};

/**
 * @brief Commit-edge callback: count the RETURN the widget reported.
 *
 * @param[in] w The keyboard widget that saw the commit edge.
 * @return void
 * @pre None.
 * @post @ref s_commits is incremented.
 * @since 0.1.0
 */
static void wkd_on_commit(struct ra8_widget* w)
{
  (void)w;
  s_commits++;
}

/** @brief The on-screen keyboard descriptor bound to the widget leaf. */
static ra8_widget_keyboard_t s_kbd = {
  .paint      = &k_wkd_paint,
  .ops        = &s_ops,
  .on_commit  = wkd_on_commit,
  .bg         = (uint32_t)k_wkd_bg,
  .key_face   = (uint32_t)k_wkd_key_face,
  .key_border = (uint32_t)k_wkd_key_border,
  .key_fg     = (uint32_t)k_wkd_key_fg,
  .border_w   = (int16_t)k_wkd_border_w,
  .reserved   = 0U,
};

/** @brief Root children array holding the keyboard. */
static ra8_widget_t s_kids[k_wkd_kid_count];

/** @brief Layout scratch nodes for box sizing. */
static ra8_box_t s_scratch[k_wkd_box_cap];

/** @brief Root container: one full-frame column holding the keyboard. */
static ra8_widget_panel_t s_root_panel = {
  .children    = s_kids,
  .box_scratch = s_scratch,
  .count       = (uint16_t)k_wkd_kid_count,
  .box_cap     = (uint16_t)k_wkd_box_cap,
  .gap         = 0,
  .pad         = 0,
  .axis        = k_ra8_widget_axis_col,
  .reserved    = 0U,
};

static ra8_widget_t s_root; /**< The root panel widget. */

/**
 * @brief Run one compose cycle over the whole tree.
 *
 * @param[out] dmg   Receives the damage rectangle to flush.
 * @param[out] hint  Receives the folded refresh hint.
 * @param[out] dirty Receives the number of composited children.
 * @return ra8_err_t Error code forwarded from ::ra8_widget_panel_compose.
 * @pre All outputs are non-NULL.
 * @post On success every dirty child has been rendered and cleared.
 * @since 0.1.0
 */
static ra8_err_t wkd_compose(ra8_ui_rect_t* dmg, ra8_widget_refresh_t* hint, uint16_t* dirty)
{
  const ra8_ui_rect_t frame = {
    .x = 0, .y = 0, .w = (int32_t)k_wkd_frame_w, .h = (int32_t)k_wkd_frame_h
  };
  return ra8_widget_panel_compose(&s_root, &frame, dmg, hint, dirty);
}

/**
 * @brief First character key in the active layer carrying @p ch unshifted.
 *
 * @param[in] ch Unshifted character to find.
 * @return uint8_t Key index, or ::k_ra8_kbd_no_hit when absent.
 * @pre The grid is laid out.
 * @post No state is modified.
 * @since 0.1.0
 */
static uint8_t wkd_key_of_char(char ch)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra8_kbd_key_char) && (s_kb.keys[i].ch_lower == ch)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/**
 * @brief First key in the active layer of behaviour @p kind.
 *
 * @param[in] kind Key behaviour to find.
 * @return uint8_t Key index, or ::k_ra8_kbd_no_hit when absent.
 * @pre The grid is laid out.
 * @post No state is modified.
 * @since 0.1.0
 */
static uint8_t wkd_key_of_kind(ra8_kbd_key_kind_t kind)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if (s_kb.keys[i].kind == kind) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/**
 * @brief Route a tap at the centre of key @p idx through the widget tree.
 *
 * @param[in] idx Key index to tap.
 * @return bool True iff a widget consumed the tap.
 * @pre The grid is laid out and @p idx is a real key.
 * @post The engine state reflects the key when the tap was consumed.
 * @note The event goes to ::ra8_widget_dispatch on the *root*, so the tap is
 *       routed root panel -> keyboard leaf -> seam, as an app would do it.
 * @since 0.1.0
 */
static bool wkd_tap_key(uint8_t idx)
{
  if (idx >= s_kb.count) {
    return false;
  }
  const ra8_ui_rect_t      r  = s_kb.keys[idx].rect;
  const ra8_widget_event_t ev = {
    .kind      = k_ra8_widget_ev_touch,
    .reserved  = 0U,
    .button_id = 0U,
    .x         = r.x + (r.w / 2),
    .y         = r.y + (r.h / 2),
  };
  bool handled = false;
  if (ra8_widget_dispatch(&s_root, 1U, &ev, &handled) != k_ra8_ok) {
    return false;
  }
  return handled;
}

/**
 * @brief Route a tap far outside the frame through the widget tree.
 *
 * @param[out] out_handled Receives whether any widget consumed the tap.
 * @return ra8_err_t Error code forwarded from ::ra8_widget_dispatch.
 * @pre @p out_handled is non-NULL.
 * @post No engine state changes when nothing consumed the tap.
 * @since 0.1.0
 */
static ra8_err_t wkd_tap_off_grid(bool* out_handled)
{
  const ra8_widget_event_t ev = {
    .kind      = k_ra8_widget_ev_touch,
    .reserved  = 0U,
    .button_id = 0U,
    .x         = (int32_t)k_wkd_off_grid,
    .y         = (int32_t)k_wkd_off_grid,
  };
  return ra8_widget_dispatch(&s_root, 1U, &ev, out_handled);
}

/**
 * @brief Assemble the tree, then assert the widget against the real engine.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Every leg held.
 * @retval k_ra8_err_invalid_arg  A leg's assertion failed.
 * @retval other                  Forwarded from the failing call.
 *
 * @pre The console is up (failures are reported through it).
 * @post The engine buffer holds the committed query on success.
 * @note Bounded loops only; no allocation (NASA Rules 2 and 3).
 * @since 0.1.0
 */
static ra8_err_t internal_keyboard_route(void)
{
  ra8_err_t err = ra8_kbd_text_init(&s_text);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_widget_keyboard_init(&s_kids[0], &s_kbd);
  if (err != k_ra8_ok) {
    return err;
  }
  s_kids[0].flex = 1U;
  err            = ra8_widget_panel_init(&s_root, &s_root_panel);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Leg 1: the published vtable is what the binder installed. */
  if ((s_kids[0].vt != ra8_widget_keyboard_vtable()) || (s_kids[0].ctx != &s_kbd) ||
      !s_kids[0].visible) {
    internal_print("widget_keyboard_demo: leg 1 bind FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }

  /* Leg 2: compose the tree, lay the engine grid into the composed rect, and
   * compose again so every key is painted through the seam. */
  ra8_ui_rect_t        dmg   = {};
  ra8_widget_refresh_t hint  = k_ra8_widget_refresh_none;
  uint16_t             dirty = 0U;
  err                        = ra8_widget_invalidate(&s_kids[0], k_ra8_widget_refresh_quality);
  if (err != k_ra8_ok) {
    return err;
  }
  err = wkd_compose(&dmg, &hint, &dirty);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((dirty != 1U) || (dmg.w <= 0) || (dmg.h <= 0)) {
    internal_print("widget_keyboard_demo: leg 2 layout FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  const ra8_ui_rect_t kbd_rect = s_kids[0].rect;
  err                          = ra8_kbd_layout_init(&s_kb, &kbd_rect);
  if (err != k_ra8_ok) {
    return err;
  }
  s_fills = 0U;
  s_texts = 0U;
  err     = ra8_widget_invalidate(&s_kids[0], k_ra8_widget_refresh_quality);
  if (err != k_ra8_ok) {
    return err;
  }
  err = wkd_compose(&dmg, &hint, &dirty);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((s_kb.count == 0U) || (s_fills < ((uint32_t)s_kb.count + 1U)) || (s_texts == 0U) ||
      (hint != k_ra8_widget_refresh_quality)) {
    internal_print("widget_keyboard_demo: leg 2 paint FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }

  /* Leg 3: taps on real keys type into the engine's own buffer. */
  const uint8_t k_r = wkd_key_of_char('r');
  const uint8_t k_a = wkd_key_of_char('a');
  const uint8_t k_b = wkd_key_of_char('b');
  if ((k_r >= s_kb.count) || (k_a >= s_kb.count) || (k_b >= s_kb.count)) {
    internal_print("widget_keyboard_demo: leg 3 key lookup FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if (!wkd_tap_key(k_r) || !wkd_tap_key(k_a)) {
    internal_print("widget_keyboard_demo: leg 3 route FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if ((s_text.len != 2U) || (strcmp(s_text.buf, "ra") != 0)) {
    internal_print("widget_keyboard_demo: leg 3 text FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }

  /* Leg 4: the one-shot SHIFT and backspace edges land in the engine. */
  const uint8_t k_shift = wkd_key_of_kind(k_ra8_kbd_key_shift);
  const uint8_t k_del   = wkd_key_of_kind(k_ra8_kbd_key_backspace);
  if ((k_shift >= s_kb.count) || (k_del >= s_kb.count)) {
    internal_print("widget_keyboard_demo: leg 4 key lookup FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if (!wkd_tap_key(k_shift) || !s_kb.shift) {
    internal_print("widget_keyboard_demo: leg 4 shift FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if (!wkd_tap_key(k_b) || (strcmp(s_text.buf, "raB") != 0) || s_kb.shift) {
    internal_print("widget_keyboard_demo: leg 4 shifted char FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if (!wkd_tap_key(k_del) || (strcmp(s_text.buf, "ra") != 0)) {
    internal_print("widget_keyboard_demo: leg 4 backspace FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }

  /* Leg 5: a tap off the grid is consumed by nobody and types nothing. */
  bool handled = true;
  err          = wkd_tap_off_grid(&handled);
  if (err != k_ra8_ok) {
    return err;
  }
  if (handled || (strcmp(s_text.buf, "ra") != 0)) {
    internal_print("widget_keyboard_demo: leg 5 off-grid FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }

  /* Leg 6: RETURN commits once, and the self-invalidate damages just the
   * keyboard's own rect. */
  const uint8_t k_enter = wkd_key_of_kind(k_ra8_kbd_key_enter);
  if (k_enter >= s_kb.count) {
    internal_print("widget_keyboard_demo: leg 6 key lookup FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  s_commits = 0U;
  if (!wkd_tap_key(k_enter) || !s_text.committed || (s_commits != 1U)) {
    internal_print("widget_keyboard_demo: leg 6 commit FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  if (!s_kids[0].dirty) {
    internal_print("widget_keyboard_demo: leg 6 self-invalidate FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  err = wkd_compose(&dmg, &hint, &dirty);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((dirty != 1U) || (dmg.x != kbd_rect.x) || (dmg.y != kbd_rect.y) || (dmg.w != kbd_rect.w) ||
      (dmg.h != kbd_rect.h)) {
    internal_print("widget_keyboard_demo: leg 6 damage FAIL\r\n");
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings the console up, runs the keyboard-widget route against the
 *          real key engine, and parks in an infinite loop.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A PASS or FAIL verdict line is queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  (void)ra8_cgc_init();
  (void)ra8_mstp_init();
  (void)ra8_board_uart_console_init(115200U);
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_wkd_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("widget_keyboard_demo: boot\r\n");

  if (internal_keyboard_route() == k_ra8_ok) {
    internal_print("widget_keyboard_demo: keyboard widget PASS\r\n");
  } else {
    internal_print("widget_keyboard_demo: keyboard widget FAIL\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_wkd_uart_chan);
  while (true) {
  }
}
