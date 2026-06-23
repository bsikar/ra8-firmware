/**
 * @file ra_keyboard.c
 * @brief On-screen keyboard widget -- iOS-style layers + shift.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_keyboard.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_ui.h"

/** @var s_tag @brief Log tag for this module. */
static const char* s_tag = "KBD";

/**
 * @enum kbd_geom_t
 * @brief Row geometry in 20ths of the width ("half-units"), iOS proportions.
 */
typedef enum : int32_t {
  k_kbd_hu_div    = 20, /**< Half-units per row (a normal key = 2 hu).      */
  k_kbd_key_hu    = 2,  /**< Normal key width (half-units).                 */
  k_kbd_wide_hu   = 3,  /**< SHIFT / BACKSPACE width.                       */
  k_kbd_inset_hu  = 1,  /**< Home-row inset each side.                      */
  k_kbd_act_hu    = 4,  /**< 123 / ABC / RETURN width.                      */
  k_kbd_space_hu  = 12, /**< SPACE width.                                   */
  k_kbd_top_keys  = 10, /**< Keys in rows 0 and 1.                         */
  k_kbd_mid_keys  = 9,  /**< Keys in the inset home row.                   */
  k_kbd_r2_lett   = 7,  /**< Letters in letters row 2.                     */
  k_kbd_punct_n   = 5,  /**< Punctuation keys (. , ? ! ') in row 2.        */
  k_kbd_punct_hu0 = 5,  /**< Centred start for the 5 punctuation keys.     */
  k_kbd_sym1_n    = 7,  /**< Symbols row 1 keys (_ \\ | ~ < > `).          */
  k_kbd_sym1_hu0  = 3,  /**< Centred start for symbols row 1.              */
  k_kbd_row1      = 1,  /**< Row index 1.                                  */
  k_kbd_row2      = 2,  /**< Row index 2.                                  */
  k_kbd_row3      = 3,  /**< Row index 3.                                  */
} kbd_geom_t;

/* Letters layer rows. */
static const char k_l_r0_lo[] = "qwertyuiop";
static const char k_l_r0_hi[] = "QWERTYUIOP";
static const char k_l_r1_lo[] = "asdfghjkl";
static const char k_l_r1_hi[] = "ASDFGHJKL";
static const char k_l_r2_lo[] = "zxcvbnm";
static const char k_l_r2_hi[] = "ZXCVBNM";
/* Numbers layer rows (no shift effect: upper == lower). */
static const char k_n_r0[] = "1234567890";
static const char k_n_r1[] = "-/:;()$&@\"";
/* Symbols layer rows. Goal: every printable 7-bit ASCII symbol is reachable.
 * The 32 ASCII symbols are split across the numbers row 1 (10), the shared
 * punctuation row (5), and these two symbols rows (10 + 7) = 32, no repeats. */
static const char k_s_r0[] = "[]{}#%^*+=";
static const char k_s_r1[] = "<>\\_`|~";
/* Shared third-row punctuation for the numbers + symbols layers. */
static const char k_punct[] = ".,?!'";

/** @brief Append one key (bounded by k_ra_kbd_max_keys). */
static void priv_add(ra_kbd_layout_t*  kb,
                     int32_t           x,
                     int32_t           w,
                     int32_t           y,
                     int32_t           h,
                     char              lo,
                     char              hi,
                     ra_kbd_key_kind_t kind,
                     uint8_t           aux)
{
  if (kb->count >= (uint8_t)k_ra_kbd_max_keys) {
    return;
  }
  ra_kbd_key_t* k = &kb->keys[kb->count];
  k->rect.x       = x;
  k->rect.y       = y;
  k->rect.w       = w;
  k->rect.h       = h;
  k->ch_lower     = lo;
  k->ch_upper     = hi;
  k->kind         = kind;
  k->aux          = aux;
  kb->count++;
}

/** @brief X for half-unit @p hu within @p frame. */
static int32_t priv_hx(const ra_ui_rect_t* f, int32_t hu)
{
  return f->x + ((f->w * hu) / (int32_t)k_kbd_hu_div);
}

/** @brief Place @p n char keys, each 2 half-units, from half-unit @p hu0. */
static void priv_place(ra_kbd_layout_t*    kb,
                       const char*         lo,
                       const char*         hi,
                       int32_t             n,
                       int32_t             hu0,
                       const ra_ui_rect_t* f,
                       int32_t             y,
                       int32_t             rh)
{
  for (int32_t i = 0; i < n; i++) {
    const int32_t x0 = priv_hx(f, hu0 + (i * (int32_t)k_kbd_key_hu));
    const int32_t x1 = priv_hx(f, hu0 + ((i + 1) * (int32_t)k_kbd_key_hu));
    priv_add(kb,
             x0,
             x1 - x0,
             y,
             rh,
             (char)lo[i],
             (char)((hi != nullptr) ? hi[i] : lo[i]),
             k_ra_kbd_key_char,
             0U);
  }
}

/** @brief A full-width special key spanning half-units [@p a, @p b). */
static void priv_span(ra_kbd_layout_t*    kb,
                      int32_t             a,
                      int32_t             b,
                      const ra_ui_rect_t* f,
                      int32_t             y,
                      int32_t             rh,
                      ra_kbd_key_kind_t   kind,
                      uint8_t             aux)
{
  const int32_t x0 = priv_hx(f, a);
  priv_add(kb, x0, priv_hx(f, b) - x0, y, rh, 0, 0, kind, aux);
}

/** @brief Row 2 for the numbers/symbols layers: a layer toggle, ., ! etc, del. */
static void
priv_row_punct(ra_kbd_layout_t* kb, const ra_ui_rect_t* f, int32_t y, int32_t rh, uint8_t tog_aux)
{
  priv_span(kb, 0, (int32_t)k_kbd_act_hu, f, y, rh, k_ra_kbd_key_layer, tog_aux);
  priv_place(kb, k_punct, nullptr, (int32_t)k_kbd_punct_n, (int32_t)k_kbd_punct_hu0, f, y, rh);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y,
            rh,
            k_ra_kbd_key_backspace,
            0U);
}

/** @brief Bottom row: a layer toggle (123/ABC), SPACE, RETURN. */
static void
priv_row_bottom(ra_kbd_layout_t* kb, const ra_ui_rect_t* f, int32_t y, int32_t rh, uint8_t left_aux)
{
  priv_span(kb, 0, (int32_t)k_kbd_act_hu, f, y, rh, k_ra_kbd_key_layer, left_aux);
  priv_span(kb,
            (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_act_hu + (int32_t)k_kbd_space_hu,
            f,
            y,
            rh,
            k_ra_kbd_key_space,
            0U);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y,
            rh,
            k_ra_kbd_key_enter,
            0U);
}

/** @brief Build the letters layer (QWERTY + SHIFT + 123). */
static void priv_build_letters(ra_kbd_layout_t* kb, const ra_ui_rect_t* f, int32_t rh)
{
  const int32_t fy = f->y;
  priv_place(kb, k_l_r0_lo, k_l_r0_hi, (int32_t)k_kbd_top_keys, 0, f, fy, rh);
  priv_place(kb,
             k_l_r1_lo,
             k_l_r1_hi,
             (int32_t)k_kbd_mid_keys,
             (int32_t)k_kbd_inset_hu,
             f,
             fy + rh,
             rh);
  const int32_t y2 = fy + ((int32_t)k_kbd_row2 * rh);
  priv_span(kb, 0, (int32_t)k_kbd_wide_hu, f, y2, rh, k_ra_kbd_key_shift, 0U);
  priv_place(kb, k_l_r2_lo, k_l_r2_hi, (int32_t)k_kbd_r2_lett, (int32_t)k_kbd_wide_hu, f, y2, rh);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_wide_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y2,
            rh,
            k_ra_kbd_key_backspace,
            0U);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra_kbd_layer_numbers);
}

/** @brief Build the numbers layer (digits + common symbols; #+= toggles syms). */
static void priv_build_numbers(ra_kbd_layout_t* kb, const ra_ui_rect_t* f, int32_t rh)
{
  const int32_t fy = f->y;
  priv_place(kb, k_n_r0, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy, rh);
  priv_place(kb, k_n_r1, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy + rh, rh);
  priv_row_punct(kb, f, fy + ((int32_t)k_kbd_row2 * rh), rh, (uint8_t)k_ra_kbd_layer_symbols);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra_kbd_layer_letters);
}

/** @brief Build the symbols layer (brackets / math; 123 toggles numbers). */
static void priv_build_symbols(ra_kbd_layout_t* kb, const ra_ui_rect_t* f, int32_t rh)
{
  const int32_t fy = f->y;
  priv_place(kb, k_s_r0, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy, rh);
  priv_place(kb, k_s_r1, nullptr, (int32_t)k_kbd_sym1_n, (int32_t)k_kbd_sym1_hu0, f, fy + rh, rh);
  priv_row_punct(kb, f, fy + ((int32_t)k_kbd_row2 * rh), rh, (uint8_t)k_ra_kbd_layer_numbers);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra_kbd_layer_letters);
}

/** @brief Lay the active layer's keys into the saved frame. */
static void priv_build_layer(ra_kbd_layout_t* kb)
{
  kb->count        = 0U;
  const int32_t rh = kb->frame.h / (int32_t)k_ra_kbd_rows;
  if (kb->layer == (uint8_t)k_ra_kbd_layer_numbers) {
    priv_build_numbers(kb, &kb->frame, rh);
  } else if (kb->layer == (uint8_t)k_ra_kbd_layer_symbols) {
    priv_build_symbols(kb, &kb->frame, rh);
  } else {
    priv_build_letters(kb, &kb->frame, rh);
  }
}

[[nodiscard]] ra_err_t ra_kbd_layout_init(ra_kbd_layout_t* kb, const ra_ui_rect_t* frame)
{
  RA_CHECK_NULL_PTR(kb, s_tag, "kb must not be nullptr");
  RA_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  /* Decision (MC/DC): reject a frame with no area. */
  if ((frame->w <= 0) || (frame->h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  kb->frame = *frame;
  kb->shift = false;
  kb->layer = (uint8_t)k_ra_kbd_layer_letters;
  priv_build_layer(kb);
  return k_ra_ok;
}

[[nodiscard]] uint8_t ra_kbd_hit(const ra_kbd_layout_t* kb, int32_t px, int32_t py)
{
  if (kb == nullptr) {
    return (uint8_t)k_ra_kbd_no_hit;
  }
  for (uint8_t i = 0U; i < kb->count; i++) {
    if (ra_ui_rect_contains(&kb->keys[i].rect, px, py)) {
      return i;
    }
  }
  return (uint8_t)k_ra_kbd_no_hit;
}

[[nodiscard]] char ra_kbd_key_glyph(const ra_kbd_layout_t* kb, uint8_t key_idx)
{
  if ((kb == nullptr) || (key_idx >= kb->count)) {
    return (char)0;
  }
  const ra_kbd_key_t* k = &kb->keys[key_idx];
  if (k->kind != k_ra_kbd_key_char) {
    return (char)0;
  }
  return (char)(kb->shift ? k->ch_upper : k->ch_lower);
}

[[nodiscard]] ra_err_t ra_kbd_text_init(ra_kbd_text_t* t)
{
  RA_CHECK_NULL_PTR(t, s_tag, "t must not be nullptr");
  t->len       = 0U;
  t->buf[0]    = '\0';
  t->committed = false;
  return k_ra_ok;
}

/** @brief Append @p ch if there is room; NUL-terminate. */
static void priv_append(ra_kbd_text_t* t, char ch)
{
  if (t->len < (uint8_t)((uint8_t)k_ra_kbd_text_max - 1U)) {
    t->buf[t->len] = ch;
    t->len++;
    t->buf[t->len] = '\0';
  }
}

[[nodiscard]] ra_err_t ra_kbd_apply(ra_kbd_text_t* t, ra_kbd_layout_t* kb, uint8_t key_idx)
{
  RA_CHECK_NULL_PTR(t, s_tag, "t must not be nullptr");
  RA_CHECK_NULL_PTR(kb, s_tag, "kb must not be nullptr");
  if (key_idx >= kb->count) {
    return k_ra_ok; /* no-hit sentinel or out of range -> no-op */
  }
  const ra_kbd_key_t* k = &kb->keys[key_idx];
  switch (k->kind) {
    case k_ra_kbd_key_char:
      priv_append(t, (char)(kb->shift ? k->ch_upper : k->ch_lower));
      kb->shift = false; /* one-shot SHIFT clears after a character */
      break;
    case k_ra_kbd_key_space:
      priv_append(t, ' ');
      kb->shift = false;
      break;
    case k_ra_kbd_key_backspace:
      if (t->len > 0U) {
        t->len--;
        t->buf[t->len] = '\0';
      }
      break;
    case k_ra_kbd_key_enter:
      t->committed = true;
      break;
    case k_ra_kbd_key_shift:
      kb->shift = !kb->shift;
      break;
    case k_ra_kbd_key_layer:
      kb->layer = k->aux;
      kb->shift = false;
      priv_build_layer(kb); /* re-lay the grid for the new layer */
      break;
    default:
      break;
  }
  return k_ra_ok;
}
