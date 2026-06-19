/**
 * @file ra_keyboard.c
 * @brief On-screen QWERTY keyboard widget implementation.
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

/** @enum kbd_geom_t @brief Row key counts + bottom-row split (no magic numbers). */
typedef enum : int32_t {
  k_kbd_row0_keys  = 10, /**< QWERTYUIOP.                       */
  k_kbd_row1_keys  = 9,  /**< ASDFGHJKL.                        */
  k_kbd_row2_cells = 8,  /**< ZXCVBNM (7) + BACKSPACE.          */
  k_kbd_row2_lett  = 7,  /**< Letters in row 2.                 */
  k_kbd_space_num  = 3,  /**< SPACE spans 3/4 of the last row.  */
  k_kbd_split_den  = 4,  /**< Denominator for the SPACE split.  */
  k_kbd_row_idx_2  = 2,  /**< Row 2 (letters + backspace).      */
  k_kbd_row_idx_3  = 3,  /**< Row 3 (space + enter).            */
} kbd_geom_t;

/** @brief Row 0 letters (top). */
static const char k_kbd_row0[] = "QWERTYUIOP";
/** @brief Row 1 letters (middle). */
static const char k_kbd_row1[] = "ASDFGHJKL";
/** @brief Row 2 letters (bottom letter row). */
static const char k_kbd_row2[] = "ZXCVBNM";

/** @brief Append one key to @p kb (bounded by k_ra_kbd_max_keys). */
static void priv_add_key(ra_kbd_layout_t*  kb,
                         int32_t           x,
                         int32_t           y,
                         int32_t           w,
                         int32_t           h,
                         char              ch,
                         ra_kbd_key_kind_t kind)
{
  if (kb->count >= (uint8_t)k_ra_kbd_max_keys) {
    return;
  }
  ra_kbd_key_t* k = &kb->keys[kb->count];
  k->rect.x       = x;
  k->rect.y       = y;
  k->rect.w       = w;
  k->rect.h       = h;
  k->ch           = ch;
  k->kind         = kind;
  kb->count++;
}

/** @brief Place @p n evenly-spaced char keys of @p s across one row. */
static void priv_place_letters(ra_kbd_layout_t* kb,
                               const char*      s,
                               int32_t          n,
                               int32_t          x0,
                               int32_t          y,
                               int32_t          w,
                               int32_t          h)
{
  const int32_t kw = w / n;
  for (int32_t i = 0; i < n; i++) {
    priv_add_key(kb, x0 + (i * kw), y, kw, h, s[i], k_ra_kbd_key_char);
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

  kb->count        = 0U;
  const int32_t rh = frame->h / (int32_t)k_ra_kbd_rows;
  const int32_t fx = frame->x;
  const int32_t fy = frame->y;
  const int32_t fw = frame->w;

  priv_place_letters(kb, k_kbd_row0, (int32_t)k_kbd_row0_keys, fx, fy, fw, rh);
  priv_place_letters(kb, k_kbd_row1, (int32_t)k_kbd_row1_keys, fx, fy + rh, fw, rh);

  /* Row 2: 7 letters then a wider BACKSPACE filling the 8th cell. */
  const int32_t y2  = fy + ((int32_t)k_kbd_row_idx_2 * rh);
  const int32_t kw2 = fw / (int32_t)k_kbd_row2_cells;
  for (int32_t i = 0; i < (int32_t)k_kbd_row2_lett; i++) {
    priv_add_key(kb, fx + (i * kw2), y2, kw2, rh, k_kbd_row2[i], k_ra_kbd_key_char);
  }
  const int32_t bs_x = fx + ((int32_t)k_kbd_row2_lett * kw2);
  priv_add_key(kb, bs_x, y2, fw - ((int32_t)k_kbd_row2_lett * kw2), rh, 0, k_ra_kbd_key_backspace);

  /* Row 3: SPACE (3/4) then ENTER (1/4). */
  const int32_t y3 = fy + ((int32_t)k_kbd_row_idx_3 * rh);
  const int32_t sw = (fw * (int32_t)k_kbd_space_num) / (int32_t)k_kbd_split_den;
  priv_add_key(kb, fx, y3, sw, rh, ' ', k_ra_kbd_key_space);
  priv_add_key(kb, fx + sw, y3, fw - sw, rh, 0, k_ra_kbd_key_enter);

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

[[nodiscard]] ra_err_t ra_kbd_apply(ra_kbd_text_t* t, const ra_kbd_layout_t* kb, uint8_t key_idx)
{
  RA_CHECK_NULL_PTR(t, s_tag, "t must not be nullptr");
  RA_CHECK_NULL_PTR(kb, s_tag, "kb must not be nullptr");
  if (key_idx >= kb->count) {
    return k_ra_ok; /* no-hit sentinel or out of range -> no-op */
  }
  const ra_kbd_key_t* k = &kb->keys[key_idx];
  switch (k->kind) {
    case k_ra_kbd_key_char:
      priv_append(t, k->ch);
      break;
    case k_ra_kbd_key_space:
      priv_append(t, ' ');
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
    default:
      break;
  }
  return k_ra_ok;
}
