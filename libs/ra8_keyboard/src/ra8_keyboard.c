/**
 * @file ra8_keyboard.c
 * @brief On-screen keyboard widget -- iOS-style layers + shift.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_keyboard.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ui.h"

/**
 * @var s_tag
 * @brief Log tag for this module.
 */
static const char* s_tag = "KBD";

/**
 * @enum kbd_geom_t
 * @brief Row geometry in 20ths of the width ("half-units"), iOS proportions.
 */
typedef enum : int32_t {
  k_kbd_hu_div    = 20, /**< Half-units per row (a normal key = 2 hu). */
  k_kbd_key_hu    = 2,  /**< Normal key width (half-units).            */
  k_kbd_wide_hu   = 3,  /**< SHIFT / BACKSPACE width.                  */
  k_kbd_inset_hu  = 1,  /**< Home-row inset each side.                 */
  k_kbd_act_hu    = 4,  /**< 123 / ABC / RETURN width.                 */
  k_kbd_space_hu  = 12, /**< SPACE width.                              */
  k_kbd_top_keys  = 10, /**< Keys in rows 0 and 1.                     */
  k_kbd_mid_keys  = 9,  /**< Keys in the inset home row.               */
  k_kbd_r2_lett   = 7,  /**< Letters in letters row 2.                 */
  k_kbd_punct_n   = 5,  /**< Punctuation keys (. , ? ! ') in row 2.    */
  k_kbd_punct_hu0 = 5,  /**< Centred start for the 5 punctuation keys. */
  k_kbd_sym1_n    = 7,  /**< Symbols row 1 keys (_ \\ | ~ < > \`).     */
  k_kbd_sym1_hu0  = 3,  /**< Centred start for symbols row 1.          */
  k_kbd_row1      = 1,  /**< Row index 1.                              */
  k_kbd_row2      = 2,  /**< Row index 2.                              */
  k_kbd_row3      = 3,  /**< Row index 3.                              */
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

/**
 * @brief Append one key descriptor to the layout, bounded by k_ra8_kbd_max_keys.
 *
 * @details
 * Writes a single ra8_kbd_key_t entry into kb->keys[kb->count] and increments
 * kb->count.  If the layout is already at capacity (kb->count >=
 * k_ra8_kbd_max_keys) the call is silently discarded so callers do not need
 * to perform their own bounds check.
 *
 * @param[in,out] kb   Layout being built; count is incremented on success.
 * @param[in]     x    Left edge of the key rectangle in screen pixels.
 * @param[in]     w    Width of the key rectangle in screen pixels.
 * @param[in]     y    Top edge of the key rectangle in screen pixels.
 * @param[in]     h    Height of the key rectangle in screen pixels.
 * @param[in]     lo   Character produced when shift is inactive (0 for special keys).
 * @param[in]     hi   Character produced when shift is active (0 for special keys).
 * @param[in]     kind Key kind tag (char, space, backspace, shift, enter, layer).
 * @param[in]     aux  Auxiliary data; for k_ra8_kbd_key_layer this is the target
 *                     layer index, otherwise 0.
 *
 * @return Nothing.
 *
 * @pre  kb is not nullptr.
 * @pre  kb->count <= k_ra8_kbd_max_keys on entry.
 * @post If kb->count was less than k_ra8_kbd_max_keys, kb->count is incremented
 *       by exactly one and the new key slot is fully initialised.
 * @post If kb->count was already equal to k_ra8_kbd_max_keys, kb is unchanged.
 *
 * @note Not thread-safe; must be called from the same context as the owning
 *       ra8_kbd_layout_t initialisation.
 *
 * @since 0.1.0
 */
static void priv_add(ra8_kbd_layout_t*  kb,
                     int32_t            x,
                     int32_t            w,
                     int32_t            y,
                     int32_t            h,
                     char               lo,
                     char               hi,
                     ra8_kbd_key_kind_t kind,
                     uint8_t            aux)
{
  if (kb->count >= (uint8_t)k_ra8_kbd_max_keys) {
    return;
  }
  ra8_kbd_key_t* k = &kb->keys[kb->count];
  k->rect.x        = x;
  k->rect.y        = y;
  k->rect.w        = w;
  k->rect.h        = h;
  k->ch_lower      = lo;
  k->ch_upper      = hi;
  k->kind          = kind;
  k->aux           = aux;
  kb->count++;
}

/**
 * @brief Compute the pixel X coordinate for a given half-unit offset within a frame.
 *
 * @details
 * Maps a half-unit index @p hu to an absolute horizontal pixel position using
 * the formula: f->x + (f->w * hu) / k_kbd_hu_div.  The division is integer
 * (truncating), matching the iOS-style proportional key layout where the full
 * frame width is divided into k_kbd_hu_div (20) equal slots.
 *
 * @param[in] f  Bounding rectangle of the keyboard widget; x and w are used.
 * @param[in] hu Half-unit index in the range [0, k_kbd_hu_div].
 *
 * @return int32_t Absolute pixel X coordinate of the half-unit boundary.
 * @retval (f->x + (f->w * hu) / k_kbd_hu_div) Computed X coordinate.
 *
 * @pre  f is not nullptr.
 * @pre  hu is in [0, k_kbd_hu_div] (values outside this range yield
 *       out-of-frame coordinates but do not trap).
 * @post The returned value is >= f->x when hu >= 0.
 * @post The returned value equals f->x + f->w when hu == k_kbd_hu_div.
 *
 * @note Not thread-safe; intended only for use during layout construction.
 *
 * @since 0.1.0
 */
static int32_t priv_hx(const ra8_ui_rect_t* f, int32_t hu)
{
  return f->x + ((f->w * hu) / (int32_t)k_kbd_hu_div);
}

/**
 * @brief Place @p n character keys, each k_kbd_key_hu (2) half-units wide,
 *        starting from half-unit offset @p hu0.
 *
 * @details
 * Iterates over indices 0..n-1 and calls priv_add() for each character in the
 * @p lo string.  The left edge of key i is at half-unit (hu0 + i*2) and the
 * right edge at half-unit (hu0 + (i+1)*2), with actual pixel positions
 * resolved via priv_hx().  When @p hi is nullptr the upper-case glyph is set
 * equal to the lower-case glyph (used for layers without a shift effect).
 *
 * @param[in,out] kb   Layout being built; keys are appended via priv_add().
 * @param[in]     lo   Null-terminated string of lower-case glyphs; must have
 *                     at least @p n characters.
 * @param[in]     hi   Null-terminated string of upper-case glyphs with at
 *                     least @p n characters, or nullptr if shift has no effect.
 * @param[in]     n    Number of character keys to place.
 * @param[in]     hu0  Starting half-unit offset within @p f.
 * @param[in]     f    Bounding rectangle of the keyboard widget.
 * @param[in]     y    Top pixel coordinate of the key row.
 * @param[in]     rh   Row height in pixels.
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  lo points to a character array with at least @p n elements.
 * @post Exactly min(n, k_ra8_kbd_max_keys - kb->count) new char keys have been
 *       appended to kb->keys.
 * @post kb->count has increased by the number of keys successfully appended.
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_place(ra8_kbd_layout_t*    kb,
                       const char*          lo,
                       const char*          hi,
                       int32_t              n,
                       int32_t              hu0,
                       const ra8_ui_rect_t* f,
                       int32_t              y,
                       int32_t              rh)
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
             k_ra8_kbd_key_char,
             0U);
  }
}

/**
 * @brief Append one special key that spans half-units [@p a, @p b).
 *
 * @details
 * Computes the pixel left edge from half-unit @p a and the pixel width as
 * priv_hx(f, b) - priv_hx(f, a), then delegates to priv_add() with zero
 * glyph characters.  Used for shift, backspace, space, enter, and layer-
 * toggle keys which span more than the standard 2-half-unit width.
 *
 * @param[in,out] kb   Layout being built; one key is appended via priv_add().
 * @param[in]     a    First half-unit of the key span (inclusive).
 * @param[in]     b    Last half-unit of the key span (exclusive).
 * @param[in]     f    Bounding rectangle of the keyboard widget.
 * @param[in]     y    Top pixel coordinate of the key row.
 * @param[in]     rh   Row height in pixels.
 * @param[in]     kind Key kind (shift, backspace, space, enter, or layer).
 * @param[in]     aux  Auxiliary data; for k_ra8_kbd_key_layer this is the
 *                     target layer index, otherwise 0.
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  a < b and both are in [0, k_kbd_hu_div].
 * @post kb->count is incremented by one if the layout was not already at
 *       k_ra8_kbd_max_keys capacity.
 * @post The appended key has ch_lower == 0 and ch_upper == 0.
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_span(ra8_kbd_layout_t*    kb,
                      int32_t              a,
                      int32_t              b,
                      const ra8_ui_rect_t* f,
                      int32_t              y,
                      int32_t              rh,
                      ra8_kbd_key_kind_t   kind,
                      uint8_t              aux)
{
  const int32_t x0 = priv_hx(f, a);
  priv_add(kb, x0, priv_hx(f, b) - x0, y, rh, 0, 0, kind, aux);
}

/**
 * @brief Build row 2 for the numbers or symbols layer: layer-toggle, punctuation, backspace.
 *
 * @details
 * Lays out three groups from left to right across the full frame width:
 * 1. A k_ra8_kbd_key_layer special key spanning [0, k_kbd_act_hu) whose aux
 *    field is set to @p tog_aux (the layer to switch to on press).
 * 2. k_kbd_punct_n (5) character keys from k_punct starting at half-unit
 *    k_kbd_punct_hu0 (centred in the row).
 * 3. A k_ra8_kbd_key_backspace special key spanning
 *    [k_kbd_hu_div - k_kbd_act_hu, k_kbd_hu_div).
 * This row is shared between the numbers and symbols layers; the only
 * difference is which layer the toggle button targets, expressed by @p tog_aux.
 *
 * @param[in,out] kb      Layout being built; keys are appended via priv_span()
 *                        and priv_place().
 * @param[in]     f       Bounding rectangle of the keyboard widget.
 * @param[in]     y       Top pixel coordinate of the row.
 * @param[in]     rh      Row height in pixels.
 * @param[in]     tog_aux Target layer index stored in the layer-toggle key's
 *                        aux field (k_ra8_kbd_layer_symbols or
 *                        k_ra8_kbd_layer_numbers).
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  tog_aux is a valid ra8_kbd_layer_t cast to uint8_t.
 * @post Seven keys (1 layer-toggle + 5 punctuation + 1 backspace) are appended
 *       to kb->keys, subject to the k_ra8_kbd_max_keys capacity guard.
 * @post kb->count increases by the number of keys successfully appended (up to 7).
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void
priv_row_punct(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* f, int32_t y, int32_t rh, uint8_t tog_aux)
{
  priv_span(kb, 0, (int32_t)k_kbd_act_hu, f, y, rh, k_ra8_kbd_key_layer, tog_aux);
  priv_place(kb, k_punct, nullptr, (int32_t)k_kbd_punct_n, (int32_t)k_kbd_punct_hu0, f, y, rh);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y,
            rh,
            k_ra8_kbd_key_backspace,
            0U);
}

/**
 * @brief Build the bottom row: a layer-toggle key, SPACE, and RETURN.
 *
 * @details
 * Lays out three groups from left to right across the full frame width:
 * 1. A k_ra8_kbd_key_layer special key spanning [0, k_kbd_act_hu) whose aux
 *    field is @p left_aux (the layer index to switch to).
 * 2. A k_ra8_kbd_key_space key spanning
 *    [k_kbd_act_hu, k_kbd_act_hu + k_kbd_space_hu).
 * 3. A k_ra8_kbd_key_enter key spanning
 *    [k_kbd_hu_div - k_kbd_act_hu, k_kbd_hu_div).
 * This row is identical in structure for all three layers; only the target of
 * the left toggle button differs, which is controlled by @p left_aux.
 *
 * @param[in,out] kb        Layout being built; keys are appended via priv_span().
 * @param[in]     f         Bounding rectangle of the keyboard widget.
 * @param[in]     y         Top pixel coordinate of the row.
 * @param[in]     rh        Row height in pixels.
 * @param[in]     left_aux  Target layer index stored in the left layer-toggle
 *                          key's aux field.
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  left_aux is a valid ra8_kbd_layer_t cast to uint8_t.
 * @post Three keys (layer-toggle, space, enter) are appended to kb->keys,
 *       subject to the k_ra8_kbd_max_keys capacity guard.
 * @post kb->count increases by the number of keys successfully appended (up to 3).
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_row_bottom(ra8_kbd_layout_t*    kb,
                            const ra8_ui_rect_t* f,
                            int32_t              y,
                            int32_t              rh,
                            uint8_t              left_aux)
{
  priv_span(kb, 0, (int32_t)k_kbd_act_hu, f, y, rh, k_ra8_kbd_key_layer, left_aux);
  priv_span(kb,
            (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_act_hu + (int32_t)k_kbd_space_hu,
            f,
            y,
            rh,
            k_ra8_kbd_key_space,
            0U);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_act_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y,
            rh,
            k_ra8_kbd_key_enter,
            0U);
}

/**
 * @brief Build the letters layer (QWERTY / SHIFT / 123 bottom-row toggle).
 *
 * @details
 * Populates four rows into @p kb:
 * - Row 0: 10-key QWERTY top row (k_l_r0_lo / k_l_r0_hi) at f->y.
 * - Row 1: 9-key home row (k_l_r1_lo / k_l_r1_hi) inset by k_kbd_inset_hu.
 * - Row 2: SHIFT (wide), 7 letter keys (k_l_r2_lo / k_l_r2_hi), BACKSPACE (wide).
 * - Row 3: layer-toggle to numbers, SPACE, RETURN via priv_row_bottom().
 * The @p rh parameter is used as the uniform row height; f->y is the top of
 * row 0 and subsequent rows are offset by multiples of @p rh.
 *
 * @param[in,out] kb  Layout being built; all letter-layer keys are appended.
 * @param[in]     f   Bounding rectangle of the keyboard widget.
 * @param[in]     rh  Row height in pixels (f->h / k_ra8_kbd_rows).
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  rh > 0 (non-zero row height, guaranteed by ra8_kbd_layout_init).
 * @post All letter-layer keys are appended to kb->keys, up to the
 *       k_ra8_kbd_max_keys capacity limit.
 * @post kb->count reflects the total number of keys placed for this layer.
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_build_letters(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* f, int32_t rh)
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
  priv_span(kb, 0, (int32_t)k_kbd_wide_hu, f, y2, rh, k_ra8_kbd_key_shift, 0U);
  priv_place(kb, k_l_r2_lo, k_l_r2_hi, (int32_t)k_kbd_r2_lett, (int32_t)k_kbd_wide_hu, f, y2, rh);
  priv_span(kb,
            (int32_t)k_kbd_hu_div - (int32_t)k_kbd_wide_hu,
            (int32_t)k_kbd_hu_div,
            f,
            y2,
            rh,
            k_ra8_kbd_key_backspace,
            0U);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra8_kbd_layer_numbers);
}

/**
 * @brief Build the numbers layer (digits, common symbols; #+= toggles to symbols).
 *
 * @details
 * Populates four rows into @p kb:
 * - Row 0: 10-key digit row (k_n_r0 "1234567890") at f->y.
 * - Row 1: 10-key symbol row (k_n_r1 "-/:;()$&@\"") at f->y + rh; no shift
 *          effect (hi == nullptr so upper == lower for each key).
 * - Row 2: layer-toggle to symbols, k_punct punctuation, BACKSPACE via
 *          priv_row_punct() with tog_aux = k_ra8_kbd_layer_symbols.
 * - Row 3: layer-toggle to letters, SPACE, RETURN via priv_row_bottom().
 *
 * @param[in,out] kb  Layout being built; all numbers-layer keys are appended.
 * @param[in]     f   Bounding rectangle of the keyboard widget.
 * @param[in]     rh  Row height in pixels (f->h / k_ra8_kbd_rows).
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  rh > 0 (non-zero row height, guaranteed by ra8_kbd_layout_init).
 * @post All numbers-layer keys are appended to kb->keys, up to the
 *       k_ra8_kbd_max_keys capacity limit.
 * @post kb->count reflects the total number of keys placed for this layer.
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_build_numbers(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* f, int32_t rh)
{
  const int32_t fy = f->y;
  priv_place(kb, k_n_r0, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy, rh);
  priv_place(kb, k_n_r1, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy + rh, rh);
  priv_row_punct(kb, f, fy + ((int32_t)k_kbd_row2 * rh), rh, (uint8_t)k_ra8_kbd_layer_symbols);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra8_kbd_layer_letters);
}

/**
 * @brief Build the symbols layer (brackets, math operators; 123 toggles to numbers).
 *
 * @details
 * Populates four rows into @p kb:
 * - Row 0: 10-key brackets/math row (k_s_r0 "[]{}#%^*+=") at f->y; no shift
 *          effect (hi == nullptr).
 * - Row 1: k_kbd_sym1_n (7) keys from k_s_r1 ("<>\\_ \`|~") centred at
 *          half-unit k_kbd_sym1_hu0 (3); no shift effect.
 * - Row 2: layer-toggle to numbers, k_punct punctuation, BACKSPACE via
 *          priv_row_punct() with tog_aux = k_ra8_kbd_layer_numbers.
 * - Row 3: layer-toggle to letters, SPACE, RETURN via priv_row_bottom().
 *
 * @param[in,out] kb  Layout being built; all symbols-layer keys are appended.
 * @param[in]     f   Bounding rectangle of the keyboard widget.
 * @param[in]     rh  Row height in pixels (f->h / k_ra8_kbd_rows).
 *
 * @return Nothing.
 *
 * @pre  kb and f are not nullptr.
 * @pre  rh > 0 (non-zero row height, guaranteed by ra8_kbd_layout_init).
 * @post All symbols-layer keys are appended to kb->keys, up to the
 *       k_ra8_kbd_max_keys capacity limit.
 * @post kb->count reflects the total number of keys placed for this layer.
 *
 * @note Not thread-safe; must be called from the layout-construction context.
 *
 * @since 0.1.0
 */
static void priv_build_symbols(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* f, int32_t rh)
{
  const int32_t fy = f->y;
  priv_place(kb, k_s_r0, nullptr, (int32_t)k_kbd_top_keys, 0, f, fy, rh);
  priv_place(kb, k_s_r1, nullptr, (int32_t)k_kbd_sym1_n, (int32_t)k_kbd_sym1_hu0, f, fy + rh, rh);
  priv_row_punct(kb, f, fy + ((int32_t)k_kbd_row2 * rh), rh, (uint8_t)k_ra8_kbd_layer_numbers);
  priv_row_bottom(kb, f, fy + ((int32_t)k_kbd_row3 * rh), rh, (uint8_t)k_ra8_kbd_layer_letters);
}

/**
 * @brief Rebuild the key grid for the active layer stored in kb->layer.
 *
 * @details
 * Resets kb->count to zero, computes the uniform row height as
 * kb->frame.h / k_ra8_kbd_rows, then dispatches to the appropriate layer
 * builder:
 * - k_ra8_kbd_layer_numbers -> priv_build_numbers()
 * - k_ra8_kbd_layer_symbols -> priv_build_symbols()
 * - any other value (including k_ra8_kbd_layer_letters) -> priv_build_letters()
 * Called once at init and again every time the active layer changes (via
 * ra8_kbd_apply() handling k_ra8_kbd_key_layer).
 *
 * @param[in,out] kb  Layout to rebuild; kb->layer and kb->frame must be valid.
 *                    kb->count is reset to 0 and then repopulated.
 *
 * @return Nothing.
 *
 * @pre  kb is not nullptr.
 * @pre  kb->frame.h > 0 and kb->frame.w > 0 (ensured by ra8_kbd_layout_init).
 * @post kb->count == 0 before the layer builder runs, then reflects the number
 *       of keys placed by the selected builder.
 * @post Every key in kb->keys[0..kb->count-1] has a fully initialised rect,
 *       glyph pair, kind, and aux field.
 *
 * @note Not thread-safe; must be called from the same context that owns @p kb.
 *
 * @since 0.1.0
 */
static void priv_build_layer(ra8_kbd_layout_t* kb)
{
  kb->count        = 0U;
  const int32_t rh = kb->frame.h / (int32_t)k_ra8_kbd_rows;
  if (kb->layer == (uint8_t)k_ra8_kbd_layer_numbers) {
    priv_build_numbers(kb, &kb->frame, rh);
  } else if (kb->layer == (uint8_t)k_ra8_kbd_layer_symbols) {
    priv_build_symbols(kb, &kb->frame, rh);
  } else {
    priv_build_letters(kb, &kb->frame, rh);
  }
}

[[nodiscard]] ra8_err_t ra8_kbd_layout_init(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* frame)
{
  RA8_CHECK_NULL_PTR(kb, s_tag, "kb must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  /* Decision (MC/DC): reject a frame with no area. */
  if ((frame->w <= 0) || (frame->h <= 0)) {
    return k_ra8_err_invalid_arg;
  }
  kb->frame = *frame;
  kb->shift = false;
  kb->layer = (uint8_t)k_ra8_kbd_layer_letters;
  priv_build_layer(kb);
  return k_ra8_ok;
}

[[nodiscard]] uint8_t ra8_kbd_hit(const ra8_kbd_layout_t* kb, int32_t px, int32_t py)
{
  if (kb == nullptr) {
    return (uint8_t)k_ra8_kbd_no_hit;
  }
  for (uint8_t i = 0U; i < kb->count; i++) {
    if (ra8_ui_rect_contains(&kb->keys[i].rect, px, py)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

[[nodiscard]] char ra8_kbd_key_glyph(const ra8_kbd_layout_t* kb, uint8_t key_idx)
{
  if ((kb == nullptr) || (key_idx >= kb->count)) {
    return (char)0;
  }
  const ra8_kbd_key_t* k = &kb->keys[key_idx];
  if (k->kind != k_ra8_kbd_key_char) {
    return (char)0;
  }
  return (char)(kb->shift ? k->ch_upper : k->ch_lower);
}

[[nodiscard]] ra8_err_t ra8_kbd_text_init(ra8_kbd_text_t* t)
{
  RA8_CHECK_NULL_PTR(t, s_tag, "t must not be nullptr");
  t->len       = 0U;
  t->buf[0]    = '\0';
  t->committed = false;
  return k_ra8_ok;
}

/**
 * @brief Append character @p ch to the text buffer if capacity allows.
 *
 * @details
 * Checks whether t->len is strictly less than (k_ra8_kbd_text_max - 1) to
 * preserve room for the NUL terminator.  If there is space, writes @p ch at
 * t->buf[t->len], increments t->len, and writes '\0' at the new t->len
 * position.  If the buffer is full the call is silently discarded so callers
 * do not need to guard against overflow.
 *
 * @param[in,out] t   Text buffer to append to; len and buf are modified on
 *                    success.
 * @param[in]     ch  Character to append (must not be '\0').
 *
 * @return Nothing.
 *
 * @pre  t is not nullptr.
 * @pre  t->len <= k_ra8_kbd_text_max - 1 on entry (invariant maintained by
 *       this function and ra8_kbd_apply()).
 * @post If t->len was less than (k_ra8_kbd_text_max - 1), t->len is incremented
 *       by one and t->buf[t->len] == '\0'.
 * @post t->buf is always NUL-terminated on exit.
 *
 * @note Not thread-safe; must be called from the same context as ra8_kbd_apply().
 *
 * @since 0.1.0
 */
static void priv_append(ra8_kbd_text_t* t, char ch)
{
  if (t->len < (uint8_t)((uint8_t)k_ra8_kbd_text_max - 1U)) {
    t->buf[t->len] = ch;
    t->len++;
    t->buf[t->len] = '\0';
  }
}

[[nodiscard]] ra8_err_t ra8_kbd_apply(ra8_kbd_text_t* t, ra8_kbd_layout_t* kb, uint8_t key_idx)
{
  RA8_CHECK_NULL_PTR(t, s_tag, "t must not be nullptr");
  RA8_CHECK_NULL_PTR(kb, s_tag, "kb must not be nullptr");
  if (key_idx >= kb->count) {
    return k_ra8_ok; /* no-hit sentinel or out of range -> no-op */
  }
  const ra8_kbd_key_t* k = &kb->keys[key_idx];
  switch (k->kind) {
    case k_ra8_kbd_key_char:
      priv_append(t, (char)(kb->shift ? k->ch_upper : k->ch_lower));
      kb->shift = false; /* one-shot SHIFT clears after a character */
      break;
    case k_ra8_kbd_key_space:
      priv_append(t, ' ');
      kb->shift = false;
      break;
    case k_ra8_kbd_key_backspace:
      if (t->len > 0U) {
        t->len--;
        t->buf[t->len] = '\0';
      }
      break;
    case k_ra8_kbd_key_enter:
      t->committed = true;
      break;
    case k_ra8_kbd_key_shift:
      kb->shift = !kb->shift;
      break;
    case k_ra8_kbd_key_layer:
      kb->layer = k->aux;
      kb->shift = false;
      priv_build_layer(kb); /* re-lay the grid for the new layer */
      break;
    default:
      break;
  }
  return k_ra8_ok;
}
