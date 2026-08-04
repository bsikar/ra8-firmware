/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_keyboard.h
 * @brief On-screen keyboard widget -- iOS-style layers, shift, hit-test.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 4 / UI] {World: NS}
 *
 * @details
 * An immediate-mode on-screen keyboard for text entry (e-reader search /
 * filter, #105), modelled on the iOS keyboard. Pure, rendering-free logic:
 *
 * - ::ra8_kbd_layout_init lays the **letters** layer into a caller frame:
 *   `qwertyuiop` / `asdfghjkl` (inset) / SHIFT + `zxcvbnm` + BACKSPACE /
 *   123 + SPACE + RETURN, with the home row inset and SHIFT/BACKSPACE/123/
 *   RETURN widened, like iOS.
 * - The **123** key switches to the numbers/symbols layer (`1234567890` /
 *   `-/:;()$&@"` / symbols + BACKSPACE / ABC + SPACE + RETURN); **ABC**
 *   switches back. Tapping a layer key re-lays the grid in place.
 * - SHIFT is one-shot: it capitalises the next letter only (so `Hello`), then
 *   clears.
 * - ::ra8_kbd_hit maps a tap to a key index; ::ra8_kbd_apply mutates the text
 *   buffer, the SHIFT state, and the active layer; ::ra8_kbd_key_glyph gives a
 *   renderer the current-case character for a key.
 *
 * The caller owns rendering (draw each key + its glyph) and tap routing; this
 * is the deterministic model underneath, unit-testable and HIL-gateable with
 * synthetic taps.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ui.h"

/**
 * @enum ra8_kbd_limits_t
 * @brief Static capacities for the keyboard widget.
 */
typedef enum : uint8_t {
  k_ra8_kbd_rows     = 4U,   /**< Key rows per layer.                    */
  k_ra8_kbd_max_keys = 40U,  /**< Key-rect slots (<= 31 used per layer). */
  k_ra8_kbd_text_max = 64U,  /**< Text buffer capacity incl. NUL.        */
  k_ra8_kbd_no_hit   = 255U, /**< ::ra8_kbd_hit "no key" sentinel.       */
} ra8_kbd_limits_t;

/**
 * @enum ra8_kbd_layer_t
 * @brief Which key set is shown.
 */
typedef enum : uint8_t {
  k_ra8_kbd_layer_letters = 0U, /**< QWERTY letters (with one-shot SHIFT). */
  k_ra8_kbd_layer_numbers = 1U, /**< Digits + common symbols (123).        */
  k_ra8_kbd_layer_symbols = 2U, /**< Brackets / math symbols (#+=).        */
} ra8_kbd_layer_t;

/**
 * @enum ra8_kbd_key_kind_t
 * @brief What a key does when tapped.
 */
typedef enum : uint8_t {
  k_ra8_kbd_key_char      = 0U, /**< Append the shift-correct char. */
  k_ra8_kbd_key_space     = 1U, /**< Append a space.                */
  k_ra8_kbd_key_backspace = 2U, /**< Delete the last char.          */
  k_ra8_kbd_key_enter     = 3U, /**< Commit the query (RETURN).     */
  k_ra8_kbd_key_shift     = 4U, /**< Toggle the one-shot SHIFT.     */
  k_ra8_kbd_key_layer     = 5U, /**< Switch to the layer in `aux`.  */
} ra8_kbd_key_kind_t;

/**
 * @struct ra8_kbd_key_t
 * @brief One key: a hit rectangle, its unshifted + shifted chars, a kind, and
 *        (for layer keys) the target layer in @c aux.
 */
typedef struct {
  ra8_ui_rect_t      rect;     /**< Hit / draw rectangle.                     */
  char               ch_lower; /**< Unshifted char (char keys); 0 else.       */
  char               ch_upper; /**< Shifted char (char keys); 0 else.         */
  ra8_kbd_key_kind_t kind;     /**< Key behaviour.                            */
  uint8_t            aux;      /**< Target ::ra8_kbd_layer_t for a layer key. */
} ra8_kbd_key_t;

/**
 * @struct ra8_kbd_layout_t
 * @brief The key grid for the active layer, plus SHIFT / layer / frame state.
 */
typedef struct {
  ra8_kbd_key_t keys[k_ra8_kbd_max_keys]; /**< Keys for the active layer.    */
  uint8_t       count;                    /**< Keys in use.                  */
  bool          shift;                    /**< One-shot SHIFT armed.         */
  uint8_t       layer;                    /**< Active ::ra8_kbd_layer_t.     */
  ra8_ui_rect_t frame;                    /**< Saved frame (re-lay on swap). */
} ra8_kbd_layout_t;

/**
 * @struct ra8_kbd_text_t
 * @brief The typed query buffer + commit state.
 *
 * @invariant `len < k_ra8_kbd_text_max`; `buf[len] == '\0'`.
 */
typedef struct {
  char    buf[k_ra8_kbd_text_max]; /**< NUL-terminated query. */
  uint8_t len;                     /**< Chars before the NUL. */
  bool    committed;               /**< RETURN was applied.   */
} ra8_kbd_text_t;

/**
 * @brief Lay the letters layer into @p frame; clear SHIFT.
 *
 * @param[out] kb    Receives the laid-out grid (letters layer, SHIFT cleared).
 * @param[in]  frame Pixel rectangle to fill (w,h > 0); saved for layer swaps.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Grid laid out.
 * @retval k_ra8_err_null_ptr    @p kb or @p frame is NULL.
 * @retval k_ra8_err_invalid_arg @p frame has non-positive w or h.
 *
 * @pre @p kb, @p frame non-NULL.
 * @pre `frame->w > 0 && frame->h > 0`.
 * @post On success `kb->layer == k_ra8_kbd_layer_letters` and `!kb->shift`.
 * @post Every key rect lies within @p frame.
 *
 * @note Pure; not thread-safe relative to @p kb.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_kbd_layout_init(ra8_kbd_layout_t* kb, const ra8_ui_rect_t* frame);

/**
 * @brief Map a tap to a key index.
 *
 * @param[in] kb Laid-out grid.
 * @param[in] px Tap X (pixels).
 * @param[in] py Tap Y (pixels).
 *
 * @return Key index `0..kb->count-1`, or ::k_ra8_kbd_no_hit if the tap hit no
 *         key (or @p kb is NULL).
 *
 * @pre @p kb is laid out (or NULL, handled).
 * @pre None.
 * @post No state modified.
 * @post Return is a valid index or ::k_ra8_kbd_no_hit.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_kbd_hit(const ra8_kbd_layout_t* kb, int32_t px, int32_t py);

/**
 * @brief Effective character a char key emits given the live SHIFT state.
 *
 * @param[in] kb      Laid-out grid (for SHIFT).
 * @param[in] key_idx Key index.
 *
 * @return The shift-correct char for a char key, or 0 for a non-char / invalid
 *         key (or NULL @p kb).
 *
 * @pre @p kb is laid out (or NULL, handled).
 * @pre None.
 * @post No state modified.
 *
 * @note Pure; lets a renderer label keys in the current case.
 * @since 0.1.0
 */
[[nodiscard]] char ra8_kbd_key_glyph(const ra8_kbd_layout_t* kb, uint8_t key_idx);

/**
 * @brief Reset a text buffer to empty / uncommitted.
 *
 * @param[out] t Buffer to clear.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Cleared.
 * @retval k_ra8_err_null_ptr @p t is NULL.
 *
 * @pre @p t non-NULL.
 * @pre None.
 * @post `t->len == 0 && t->buf[0] == '\0' && !t->committed`.
 * @post Buffer ready for ::ra8_kbd_apply.
 *
 * @note Not thread-safe relative to @p t.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_kbd_text_init(ra8_kbd_text_t* t);

/**
 * @brief Apply key @p key_idx to the text buffer, SHIFT, and active layer.
 *
 * @details
 * A char/space appends (shift-correct) and clears one-shot SHIFT; SHIFT
 * toggles `kb->shift`; a layer key swaps the layer (re-laying the grid in
 * place); backspace deletes; RETURN sets `committed`. Out-of-range / capacity
 * edges are no-ops (still `k_ra8_ok`).
 *
 * @param[in,out] t       Text buffer.
 * @param[in,out] kb      Laid-out grid (SHIFT / layer / keys may change).
 * @param[in]     key_idx Key index from ::ra8_kbd_hit.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Applied (possibly a no-op at a capacity edge).
 * @retval k_ra8_err_null_ptr @p t or @p kb is NULL.
 *
 * @pre @p t, @p kb non-NULL.
 * @pre @p key_idx is a hit index or ::k_ra8_kbd_no_hit (no-op).
 * @post `t->buf` stays NUL-terminated and `t->len < k_ra8_kbd_text_max`.
 * @post `committed` is true iff a RETURN key was applied.
 *
 * @note Not thread-safe relative to @p t / @p kb.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_kbd_apply(ra8_kbd_text_t* t, ra8_kbd_layout_t* kb, uint8_t key_idx);

#ifdef __cplusplus
}
#endif
