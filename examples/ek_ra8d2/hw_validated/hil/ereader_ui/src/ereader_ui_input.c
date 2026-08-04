/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_ui/src/ereader_ui_input.c
 * @brief E-reader UI chrome -- on-screen keyboard, battery nag, input polling.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of ``ereader_ui/main.c`` to keep every translation unit under the
 * repository's 1000-line cap (a pure code move). This unit owns the Apple-style
 * on-screen keyboard rendering (rounded keys, vector SHIFT / BACKSPACE / RETURN
 * glyph icons, search-field band), the optional Settings stub, the low-battery
 * nag-banner overlay and its fuel-gauge polling, and the per-frame input
 * dispatch (touch taps and SW1/SW2 page-turn buttons). The composited chrome is
 * byte-identical to the pre-split monolith.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "er_pageturn.h"
#include "ereader_ui_steps.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_display_pal.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_i3c.h"
#include "ra8_keyboard.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_touch.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/* ===========================================================================
 * On-screen keyboard -- built with ra8_keyboard, rendered with ra8_gfx
 * =========================================================================== */

/** @enum er_kbd_render_t @brief Keyboard chrome metrics (8x16 glyphs + keys). */
typedef enum : int32_t {
  k_er_kbd_glyph_w   = 8,  /**< ra8_gfx_font_8x16 glyph width.    */
  k_er_kbd_glyph_h   = 16, /**< ra8_gfx_font_8x16 glyph height.   */
  k_er_kbd_qlabel    = 72, /**< "Search:" label column width.     */
  k_er_kbd_gap       = 6,  /**< Inset gap around each key.        */
  k_er_kbd_radius    = 10, /**< Key corner radius (rounded, iOS). */
  k_er_kbd_shadow_dy = 3,  /**< Key drop-shadow offset (down).    */
  k_er_kbd_lab_max   = 8,  /**< Key-label scratch size.           */
  /* Glyph-icon geometry (px from the key centre) for SHIFT / DEL / RETURN. */
  k_er_ico_aw   = 11, /**< Shift arrowhead half-width.         */
  k_er_ico_atop = 12, /**< Shift arrowhead apex, above centre. */
  k_er_ico_abot = 2,  /**< Shift arrowhead base, above centre. */
  k_er_ico_sw   = 3,  /**< Shift stem half-width.              */
  k_er_ico_sbot = 11, /**< Shift stem, below centre.           */
  k_er_ico_dw   = 14, /**< Delete glyph half-width.            */
  k_er_ico_dh   = 9,  /**< Delete glyph half-height.           */
  k_er_ico_dx   = 5,  /**< Delete X half-extent.               */
  k_er_ico_dxo  = 4,  /**< Delete X right-of-centre offset.    */
  k_er_ico_rw   = 11, /**< Return shaft half-width.            */
  k_er_ico_rh   = 10, /**< Return tail height.                 */
  k_er_ico_rah  = 6,  /**< Return arrowhead leg.               */
} er_kbd_render_t;

/** @enum er_kbd_color_t @brief Apple-style grayscale keyboard palette. */
typedef enum : uint32_t {
  k_er_kbd_bg    = 0xCCCCCCU, /**< Keyboard background.      */
  k_er_kbd_keylt = 0xFFFFFFU, /**< Light (letter/space) key. */
  k_er_kbd_keydk = 0xAAAAAAU, /**< Dark (special) key.       */
  k_er_kbd_keysh = 0x909090U, /**< Key drop shadow.          */
} er_kbd_color_t;

/** @brief Text label for key @p idx (SPACE / 123-ABC, or live-case char in @p s).
 *
 * @note SHIFT / BACKSPACE / RETURN are drawn as glyph icons, not text, so they
 *       never reach this helper.
 */
static const char* er_kbd_label(uint8_t idx, char* s)
{
  switch (s_kb.keys[idx].kind) {
    case k_ra8_kbd_key_space:
      return "space";
    case k_ra8_kbd_key_layer:
      if (s_kb.keys[idx].aux == (uint8_t)k_ra8_kbd_layer_numbers) {
        return "123";
      }
      if (s_kb.keys[idx].aux == (uint8_t)k_ra8_kbd_layer_symbols) {
        return "#+=";
      }
      return "ABC";
    default:
      s[0] = ra8_kbd_key_glyph(&s_kb, idx);
      s[1] = '\0';
      return s;
  }
}

/** @brief Filled rounded rect (rect cross + four corner discs). */
static void er_round_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
  const int32_t r = (int32_t)k_er_kbd_radius;
  (void)ra8_gfx_rect(x + r, y, w - (2 * r), h, c, true);
  (void)ra8_gfx_rect(x, y + r, w, h - (2 * r), c, true);
  (void)ra8_gfx_circle(x + r, y + r, r, c, true);
  (void)ra8_gfx_circle((x + w - r) - 1, y + r, r, c, true);
  (void)ra8_gfx_circle(x + r, (y + h - r) - 1, r, c, true);
  (void)ra8_gfx_circle((x + w - r) - 1, (y + h - r) - 1, r, c, true);
}

/** @brief Filled upward triangle from apex row @p ay to base row @p by (scanlines). */
static void er_tri_up(int32_t cx, int32_t ay, int32_t by, int32_t hw, uint32_t c)
{
  const int32_t span = by - ay;
  for (int32_t y = ay; y <= by; ++y) {
    const int32_t half = (span > 0) ? ((hw * (y - ay)) / span) : hw;
    (void)ra8_gfx_line(cx - half, y, cx + half, y, c);
  }
}

/** @brief SHIFT glyph: an up-arrow (filled arrowhead over a narrow stem). */
static void er_icon_shift(int32_t cx, int32_t cy, uint32_t c)
{
  er_tri_up(cx, cy - (int32_t)k_er_ico_atop, cy - (int32_t)k_er_ico_abot, (int32_t)k_er_ico_aw, c);
  (void)ra8_gfx_rect(cx - (int32_t)k_er_ico_sw,
                     cy - (int32_t)k_er_ico_abot,
                     2 * (int32_t)k_er_ico_sw,
                     (int32_t)k_er_ico_abot + (int32_t)k_er_ico_sbot,
                     c,
                     true);
}

/** @brief BACKSPACE glyph: a left-pointing pentagon outline with an X inside. */
static void er_icon_delete(int32_t cx, int32_t cy, uint32_t c)
{
  const int32_t w  = (int32_t)k_er_ico_dw;
  const int32_t h  = (int32_t)k_er_ico_dh;
  const int32_t tx = (cx - w) + h; /* where the tip opens into the body */
  (void)ra8_gfx_line(cx - w, cy, tx, cy - h, c);
  (void)ra8_gfx_line(tx, cy - h, cx + w, cy - h, c);
  (void)ra8_gfx_line(cx + w, cy - h, cx + w, cy + h, c);
  (void)ra8_gfx_line(cx + w, cy + h, tx, cy + h, c);
  (void)ra8_gfx_line(tx, cy + h, cx - w, cy, c);
  const int32_t xo = (int32_t)k_er_ico_dxo;
  const int32_t xe = (int32_t)k_er_ico_dx;
  (void)ra8_gfx_line((cx + xo) - xe, cy - xe, cx + xo + xe, cy + xe, c);
  (void)ra8_gfx_line(cx + xo + xe, cy - xe, (cx + xo) - xe, cy + xe, c);
}

/** @brief RETURN glyph: a carriage-return arrow (tail down, shaft left, arrowhead). */
static void er_icon_return(int32_t cx, int32_t cy, uint32_t c)
{
  const int32_t w = (int32_t)k_er_ico_rw;
  const int32_t a = (int32_t)k_er_ico_rah;
  (void)ra8_gfx_line(cx + w, cy - (int32_t)k_er_ico_rh, cx + w, cy, c);
  (void)ra8_gfx_line(cx + w, cy, cx - w, cy, c);
  (void)ra8_gfx_line(cx - w, cy, (cx - w) + a, cy - a, c);
  (void)ra8_gfx_line(cx - w, cy, (cx - w) + a, cy + a, c);
}

/** @brief True for keys drawn dark (SHIFT / BACKSPACE / 123-ABC / RETURN). */
static bool er_key_is_special(ra8_kbd_key_kind_t kind)
{
  return (kind != k_ra8_kbd_key_char) && (kind != k_ra8_kbd_key_space);
}

/** @brief Draw the text label of key @p idx centred in the key body. */
static void
er_draw_key_label(uint8_t idx, int32_t kx, int32_t ky, int32_t kw, int32_t kh, uint32_t fill)
{
  char          scratch[k_er_kbd_lab_max] = {};
  const char*   lab                       = er_kbd_label(idx, scratch);
  const int32_t lw                        = (int32_t)strlen(lab) * (int32_t)k_er_kbd_glyph_w;
  const int32_t lx                        = kx + ((kw - lw) / 2);
  const int32_t ly                        = ky + ((kh - (int32_t)k_er_kbd_glyph_h) / 2);
  (void)ra8_gfx_text_out(lx, ly, lab, &ra8_gfx_font_8x16, (uint32_t)k_er_ink, fill);
}

/** @brief Draw key @p idx Apple-style: a shadowed rounded key + glyph icon / label.
 *
 * @details SHIFT / BACKSPACE / RETURN render as vector glyph icons; every other
 * key (letters, digits, symbols, SPACE, 123/#+=/ABC) renders its text label. An
 * armed one-shot SHIFT inverts the SHIFT key (white body) like iOS.
 */
static void er_draw_key(uint8_t idx)
{
  const ra8_ui_rect_t      r       = s_kb.keys[idx].rect;
  const int32_t            g       = (int32_t)k_er_kbd_gap;
  const int32_t            kx      = r.x + g;
  const int32_t            ky      = r.y + g;
  const int32_t            kw      = r.w - (2 * g);
  const int32_t            kh      = r.h - (2 * g);
  const ra8_kbd_key_kind_t kind    = s_kb.keys[idx].kind;
  const bool               special = er_key_is_special(kind);
  const bool               sh_on   = (kind == k_ra8_kbd_key_shift) && s_kb.shift;
  const uint32_t fill = (special && !sh_on) ? (uint32_t)k_er_kbd_keydk : (uint32_t)k_er_kbd_keylt;
  er_round_rect(kx, ky + (int32_t)k_er_kbd_shadow_dy, kw, kh, (uint32_t)k_er_kbd_keysh);
  er_round_rect(kx, ky, kw, kh, fill);

  const int32_t cx = kx + (kw / 2);
  const int32_t cy = ky + (kh / 2);
  switch (kind) {
    case k_ra8_kbd_key_shift:
      er_icon_shift(cx, cy, (uint32_t)k_er_ink);
      break;
    case k_ra8_kbd_key_backspace:
      er_icon_delete(cx, cy, (uint32_t)k_er_ink);
      break;
    case k_ra8_kbd_key_enter:
      er_icon_return(cx, cy, (uint32_t)k_er_ink);
      break;
    default:
      er_draw_key_label(idx, kx, ky, kw, kh, fill);
      break;
  }
}

/** @brief Search-field band widget: the "Search:" label + the query / hint. */
static void er_kbd_search_render(ra8_widget_t* w)
{
  (void)w;
  const int32_t qy = (int32_t)k_er_statusbar_h + (int32_t)k_er_text_pad;
  er_text_left((int32_t)k_er_pad_ui, qy, "Search:", (uint32_t)k_er_ink_muted);
  const char* shown = (s_query.len > 0U) ? s_query.buf : k_er_search_hint;
  er_text_left((int32_t)k_er_pad_ui + (int32_t)k_er_kbd_qlabel, qy, shown, (uint32_t)k_er_ink);
}

/** @brief Keyboard band widget: gray backdrop + the laid-out keys + tap targets. */
static void er_kbd_keys_render(ra8_widget_t* w)
{
  (void)w;
  /* Fill the keyboard band with the iOS-style gray backdrop. */
  const int32_t band_y = (int32_t)k_er_statusbar_h + (int32_t)k_er_toolbar_h;
  (void)ra8_gfx_rect(0,
                     band_y,
                     (int32_t)s_fb.width_px,
                     (int32_t)s_fb.height_px - band_y,
                     (uint32_t)k_er_kbd_bg,
                     true);

  const int32_t       ky    = band_y + (int32_t)k_er_pad_ui;
  const ra8_ui_rect_t frame = {.x = (int32_t)k_er_pad_ui,
                               .y = ky,
                               .w = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_pad_ui),
                               .h = (int32_t)s_fb.height_px - ky - (int32_t)k_er_pad_ui};
  (void)ra8_kbd_layout_init(&s_kb, &frame);

  s_target_count = 0U;
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    er_draw_key(i);
    if (s_target_count < (uint16_t)k_er_max_targets) {
      s_targets[s_target_count].rect      = s_kb.keys[i].rect;
      s_targets[s_target_count].action_id = (uint16_t)((uint16_t)k_er_act_key_base + (uint16_t)i);
      s_targets[s_target_count].reserved  = 0U;
      s_target_count++;
    }
  }
}

/** @brief Vtables for the keyboard search-field / keys band widgets. */
static const ra8_widget_vtable_t k_er_kbd_search_vt = {.render = er_kbd_search_render};
static const ra8_widget_vtable_t k_er_kbd_keys_vt   = {.render = er_kbd_keys_render};

void er_render_keyboard(void)
{
  (void)ra8_gfx_clear((uint32_t)k_er_paper);
  const ra8_ui_rect_t frame    = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  ra8_widget_t        bands[2] = {};
  bands[0].vt                  = &k_er_kbd_search_vt;
  bands[0].fixed               = (int16_t)((int32_t)k_er_statusbar_h + (int32_t)k_er_toolbar_h);
  bands[0].visible             = true;
  bands[1].vt                  = &k_er_kbd_keys_vt;
  bands[1].flex                = 1U;
  bands[1].visible             = true;
  ra8_box_t band_scratch[3];
  (void)ra8_widget_layout_stack(bands, 2U, &frame, k_ra8_widget_axis_col, 0, 0, band_scratch, 3U);
  for (uint16_t i = 0U; i < 2U; ++i) {
    (void)ra8_widget_invalidate(&bands[i], k_ra8_widget_refresh_quality);
  }
  (void)ra8_widget_render_dirty(bands, 2U);
}

/* ===========================================================================
 * Low-battery nag banner + fuel-gauge polling
 * =========================================================================== */

/** @brief Nag banner messages (the SOC percent + dismiss hint follow at render). */
static const char k_er_nag_low_msg[]  = "Low battery ";
static const char k_er_nag_crit_msg[] = "Battery critical ";
/** @brief Trailing hint so the banner says how to close itself. */
static const char k_er_nag_hint[] = "  (tap to dismiss)";

/** @brief Best-effort read of one fuel-gauge register on the touch IIC_B bus. */
static bool er_fg_read(uint8_t reg, uint8_t* out)
{
  if (out == nullptr) {
    return false;
  }
  if (ra8_i3c_write((uint8_t)k_er_touch_channel, (uint8_t)k_er_fg_addr, &reg, 1U, true) !=
      k_ra8_ok) {
    return false;
  }
  return (ra8_i3c_read((uint8_t)k_er_touch_channel, (uint8_t)k_er_fg_addr, out, 1U, false) ==
          k_ra8_ok);
}

/** @brief Read SOC + charge direction from the fuel gauge; false if absent (NAK). */
static bool er_read_battery(uint8_t* soc, bool* charging)
{
  if ((soc == nullptr) || (charging == nullptr)) {
    return false;
  }
  uint8_t pct      = 0U;
  uint8_t crate_hi = 0U;
  if (!er_fg_read((uint8_t)k_er_fg_reg_soc, &pct)) {
    return false;
  }
  if (!er_fg_read((uint8_t)k_er_fg_reg_crate, &crate_hi)) {
    return false;
  }
  *soc      = pct;
  *charging = ((crate_hi & (uint8_t)k_er_fg_sign) == 0U);
  return true;
}

/** @brief Append "<v>%" (v in 0..255) to @p buf at *pos, bounded + NUL-terminated. */
static void er_append_pct(char* buf, uint32_t cap, uint32_t* pos, uint8_t v)
{
  char     tmp[k_er_nag_dec_max];
  uint32_t n = 0U;
  if (v == 0U) {
    tmp[n] = '0';
    n++;
  }
  while ((v > 0U) && (n < (uint32_t)k_er_nag_dec_max)) {
    tmp[n] = (char)('0' + (v % (uint8_t)k_er_nag_dec_ten));
    n++;
    v = (uint8_t)(v / (uint8_t)k_er_nag_dec_ten);
  }
  for (uint32_t i = 0U; (i < n) && (*pos < (cap - 1U)); i++) {
    buf[*pos] = tmp[n - 1U - i];
    (*pos)++;
  }
  if (*pos < (cap - 1U)) {
    buf[*pos] = '%';
    (*pos)++;
  }
  buf[*pos] = '\0';
}

/** @brief Append NUL-terminated @p s to @p buf at *pos, bounded + NUL-terminated. */
static void er_append_str(char* buf, uint32_t cap, uint32_t* pos, const char* s)
{
  /* Bounded by the buffer cap (NASA Rule 2); the NUL is the early exit. */
  for (uint32_t i = 0U; (i < (uint32_t)k_er_nag_line_max) && (s[i] != '\0') && (*pos < (cap - 1U));
       i++) {
    buf[*pos] = s[i];
    (*pos)++;
  }
  buf[*pos] = '\0';
}

/** @brief True iff (x,y) lands on the low-battery banner rect. */
static bool er_nag_hit(int32_t x, int32_t y)
{
  const int32_t bx = (int32_t)k_er_nag_margin;
  const int32_t bw = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_nag_margin);
  return (x >= bx) && (x < (bx + bw)) && (y >= (int32_t)k_er_nag_top) &&
         (y < ((int32_t)k_er_nag_top + (int32_t)k_er_nag_h));
}

void er_nag_render(ra8_widget_t* w)
{
  (void)w;
  if (s_batt_nag == k_ra8_batt_nag_none) {
    return;
  }
  const bool     crit = (s_batt_nag == k_ra8_batt_nag_critical);
  const uint32_t bg   = crit ? (uint32_t)k_er_nag_crit_bg : (uint32_t)k_er_nag_low_bg;
  const int32_t  x    = (int32_t)k_er_nag_margin;
  const int32_t  bw   = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_nag_margin);
  (void)ra8_gfx_rect(x, (int32_t)k_er_nag_top, bw, (int32_t)k_er_nag_h, bg, true);
  (void)ra8_gfx_rect(x, (int32_t)k_er_nag_top, bw, (int32_t)k_er_nag_h, (uint32_t)k_er_ink, false);
  char     line[k_er_nag_line_max];
  uint32_t pos = 0U;
  er_append_str(line,
                (uint32_t)k_er_nag_line_max,
                &pos,
                crit ? k_er_nag_crit_msg : k_er_nag_low_msg);
  er_append_pct(line, (uint32_t)k_er_nag_line_max, &pos, s_batt_soc);
  er_append_str(line, (uint32_t)k_er_nag_line_max, &pos, k_er_nag_hint);
  (void)ra8_gfx_text_out(x + (int32_t)k_er_nag_text_dx,
                         (int32_t)k_er_nag_top + (int32_t)k_er_nag_text_dy,
                         line,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_nag_text,
                         bg);
}

/* ===========================================================================
 * Tap routing -- per-screen dispatch shared by touch + buttons
 * =========================================================================== */

/**
 * @brief Handle a tap while the Reading view is on top.
 *
 * @details In priority order: the back-region (return from a link jump, else
 * pop to the Library), an in-content `<a>` link (er_reading_link_tap), then a
 * left/right page-turn. Split out of er_handle_tap() to keep each within the
 * cognitive-complexity budget.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 * @return true iff the tap changed the reading location / screen.
 * @retval true  Re-render required.
 * @retval false Tap hit nothing actionable.
 * @pre The Reading view is on top of ::s_nav.
 * @pre ra8_gfx is bound and the chapter is laid out.
 * @post The navigation / page state may change.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_handle_reading_tap(int32_t x, int32_t y)
{
  const ra8_ui_rect_t back = {0, 0, (int32_t)k_er_back_w, (int32_t)k_er_statusbar_h};
  if (ra8_ui_rect_contains(&back, x, y)) {
    /* Back returns from a link jump (footnote or chapter) first, then pops the
     * screen back to the Library (#110). */
    if (s_loc_back_count > 0U) {
      s_loc_back_count--;
      s_chapter_idx  = s_loc_back[s_loc_back_count].chapter;
      s_reading_page = s_loc_back[s_loc_back_count].page;
      return true;
    }
    uint16_t prev = 0U;
    return (ra8_ui_nav_pop(&s_nav, &prev) == k_ra8_ok);
  }
  /* Follow an in-content `<a>` link before falling back to page-turn. */
  if (er_reading_link_tap(x, y)) {
    return true;
  }
  /* Page-turn: a tap in the right third of the screen advances, the left third
   * goes back, the middle third is neutral. Crosses chapter boundaries and
   * clamps at the book ends (er_apply_pageturn). A no-op leaves the screen
   * unchanged (e.g. in the bitmap fallback where s_reading_pages stays 1). */
  return er_apply_pageturn(er_tap_to_dir(x, (int32_t)s_fb.width_px));
}

/**
 * @brief Route a tap on the keyboard screen: type a key, or commit on ENTER.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 * @return true if the screen must re-render (a key changed the query, or the
 *         committed query popped back to the filtered Library).
 */
static bool er_handle_keyboard_tap(int32_t x, int32_t y)
{
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra8_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
  if (!hit || (action < (uint16_t)k_er_act_key_base)) {
    return false;
  }
  const uint8_t idx = (uint8_t)(action - (uint16_t)k_er_act_key_base);
  (void)ra8_kbd_apply(&s_query, &s_kb, idx);
  if (s_query.committed) {
    uint16_t prev = (uint16_t)k_er_screen_library;
    return (ra8_ui_nav_pop(&s_nav, &prev) == k_ra8_ok); /* ENTER -> filtered Library */
  }
  return true; /* re-render to show the updated query */
}

/**
 * @brief Route a tap to a navigation action for the current screen.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 *
 * @return true if the tap changed the screen (caller should re-render).
 * @retval true  Navigation stack changed.
 * @retval false Tap hit nothing actionable.
 *
 * @pre ra8_gfx is bound; ``s_nav`` initialised; targets reflect the screen.
 * @pre None.
 * @post On a Library card tap the Reading view is pushed; on a Reading
 *       back-region tap it is popped.
 * @post On no hit the stack is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_handle_tap(int32_t x, int32_t y)
{
  /* Default any screen/location change to a clean full refresh; a same-chapter
   * page turn lowers this to a fast partial inside er_apply_pageturn. */
  s_pending_event   = k_display_event_chapter;
  s_nag_region_only = false; /* assume a full repaint unless only the nag changes. */
  /* A tap on the low-battery banner dismisses it (acknowledge). ra8_batt will not
   * re-raise the same band until the battery recovers, so it stays dismissed
   * until the next descent into a worse band, or a recovery then re-drain. Only
   * the banner rect changed, so the caller can repaint just that region. */
  if ((s_batt_nag != k_ra8_batt_nag_none) && er_nag_hit(x, y)) {
    s_batt_nag        = k_ra8_batt_nag_none;
    s_nag_region_only = true;
    return true;
  }
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra8_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    return er_handle_reading_tap(x, y);
  }
  if (top == (uint16_t)k_er_screen_keyboard) {
    return er_handle_keyboard_tap(x, y);
  }
#ifdef RA8_APP_SETTINGS
  if (top == (uint16_t)k_er_screen_settings) {
    /* Settings has no tap targets; any tap returns to the Library. */
    return (ra8_ui_nav_pop(&s_nav, &top) == k_ra8_ok);
  }
#endif
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra8_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
#ifdef RA8_APP_SETTINGS
  if (hit && (action == (uint16_t)k_er_act_settings)) {
    return (ra8_ui_nav_push(&s_nav, (uint16_t)k_er_screen_settings) == k_ra8_ok);
  }
#endif
  if (hit && (action == (uint16_t)k_er_act_open_book)) {
    s_reading_page   = 0U;                   /* always open a book at its first page */
    s_chapter_idx    = 0U;                   /* and its first chapter                */
    s_loc_back_count = 0U;                   /* with a fresh navigation back-stack   */
    s_pending_event  = k_display_event_open; /* fresh book -> clean INIT             */
    return (ra8_ui_nav_push(&s_nav, (uint16_t)k_er_screen_reading) == k_ra8_ok);
  }
  if (hit && (action == (uint16_t)k_er_act_search)) {
    (void)ra8_kbd_text_init(&s_query); /* fresh query each time Search opens */
    return (ra8_ui_nav_push(&s_nav, (uint16_t)k_er_screen_keyboard) == k_ra8_ok);
  }
  return false;
}

/* ===========================================================================
 * Per-frame input polling -- touch, page-turn buttons, fuel gauge
 * =========================================================================== */

/** @brief Debounce: true while a contact is held, to fire once per tap. */
static bool s_was_touching;

/** @brief Debounce: true while SW1/SW2 are held, to fire once per press. */
static bool s_was_sw1;
static bool s_was_sw2; /**< @see s_was_sw1 */

void er_poll_touch(void)
{
  ra8_touch_point_t pt  = {};
  uint8_t           got = 0U;
  if (ra8_touch_read(&pt, (uint8_t)k_er_touch_max_points, &got) != k_ra8_ok) {
    return;
  }
  const bool touching = (got > 0U);
  if (touching && !s_was_touching) {
    if (er_handle_tap((int32_t)pt.x, (int32_t)pt.y)) {
      if (s_nag_region_only) {
        er_render_nag_region(); /* a tap that only dismissed the banner */
      } else {
        er_render_current();
      }
      er_flush_event(s_pending_event);
    }
  }
  s_was_touching = touching;
}

/** @brief Read a user switch (active-low); true == pressed. */
static bool er_sw_pressed(ra8_port_pin_t pin)
{
  ra8_level_t level = k_ra8_level_high;
  if (ra8_gpio_read(pin, &level) != k_ra8_ok) {
    return false; /* unreadable switch -> treat as released (touch still works) */
  }
  return (level == k_ra8_level_low);
}

void er_poll_buttons(void)
{
  ra8_port_pin_t sw1_pin = k_ra8_pin_none;
  ra8_port_pin_t sw2_pin = k_ra8_pin_none;
  (void)ra8_board_sw_pin(k_ra8_board_sw1, &sw1_pin);
  (void)ra8_board_sw_pin(k_ra8_board_sw2, &sw2_pin);
  const bool sw1    = er_sw_pressed(sw1_pin);
  const bool sw2    = er_sw_pressed(sw2_pin);
  const bool fresh1 = sw1 && !s_was_sw1;
  const bool fresh2 = sw2 && !s_was_sw2;
  s_was_sw1         = sw1;
  s_was_sw2         = sw2;
  /* Page-turn buttons act only in the Reading view (a press elsewhere is a
   * no-op, so it cannot disturb the reading location from another screen). */
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra8_ui_nav_top(&s_nav, &top);
  if ((top != (uint16_t)k_er_screen_reading) || !(fresh1 || fresh2)) {
    return;
  }
  if (er_apply_pageturn(er_buttons_to_dir(fresh1, fresh2))) {
    er_render_current();
    er_flush_event(s_pending_event);
  }
}

/**
 * @brief Implementation of `er_poll_battery()` -- the clear-the-banner glue.
 *
 * @par MC/DC:
 * Decision: `(s_batt_nag != k_ra8_batt_nag_none) && (chg || (soc > recover))`
 * (3 conditions: banner-active, charging, soc-recovered). The ra8_batt policy's
 * own edge / re-arm decisions are MC/DC-tested in tests/test_ra8_batt.c; this
 * clear-the-banner glue is exercised by the ra8_emulator battery-slider gate
 * (dragging below 20/10 percent raises the banner; back up or toggling charge
 * clears it) rather than a host unit test, per the hw_pending board-demo
 * convention (no host test, like smbus_demo).
 */
void er_poll_battery(void)
{
  /* Throttle: the gauge moves slowly and each read is two I2C transactions, so
   * polling it every frame dominated the loop (the GT911 touch read is the only
   * other per-frame I2C). Poll ~once a second so touch stays responsive. */
  static uint32_t s_skip = 0U;
  if (s_skip != 0U) {
    s_skip--;
    return;
  }
  s_skip = (uint32_t)k_er_batt_every - 1U;

  uint8_t soc = 0U;
  bool    chg = false;
  if (!er_read_battery(&soc, &chg)) {
    return; /* best-effort: no gauge fitted -> no banner */
  }
  s_batt_soc         = soc;
  ra8_batt_nag_t nag = k_ra8_batt_nag_none;
  if (ra8_batt_update(&s_batt_mon, soc, chg, &nag) != k_ra8_ok) {
    return;
  }
  const ra8_batt_nag_t prev = s_batt_nag;
  const uint8_t recover = (uint8_t)((uint8_t)k_ra8_batt_low_pct + (uint8_t)k_ra8_batt_rearm_margin);
  if (nag != k_ra8_batt_nag_none) {
    s_batt_nag = nag; /* new low / critical edge -> raise or upgrade the banner */
  } else if ((s_batt_nag != k_ra8_batt_nag_none) && (chg || (soc > recover))) {
    s_batt_nag = k_ra8_batt_nag_none; /* recovered or charging -> clear the banner */
  }
  if (s_batt_nag != prev) {
    er_render_nag_region(); /* only the banner changed -> repaint just its rect */
    er_flush_event(k_display_event_chapter);
  }
}
