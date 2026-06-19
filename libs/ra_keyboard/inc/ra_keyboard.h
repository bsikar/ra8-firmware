/**
 * @file ra_keyboard.h
 * @brief On-screen QWERTY keyboard widget -- layout, hit-test, text buffer.
 *
 * @par Tag
 * [Ring 4 / UI] {World: NS}
 *
 * @details
 * An immediate-mode on-screen keyboard for text entry (e-reader search /
 * filter, #105). The widget is split into pure, testable logic with no
 * rendering dependency:
 *
 * - ::ra_kbd_layout_init lays a fixed QWERTY grid (3 letter rows + a row with
 *   SPACE and ENTER, BACKSPACE trailing the bottom letter row) into a caller
 *   frame, computing one ::ra_ui_rect_t per key.
 * - ::ra_kbd_hit maps a tap (px,py) to a key index via ::ra_ui_rect_contains.
 * - ::ra_kbd_apply mutates a ::ra_kbd_text_t (append / backspace / space /
 *   commit) for a hit key.
 *
 * The caller owns rendering (draw each key's rect + glyph through `ra_gfx`)
 * and tap routing; this module is the deterministic model behind it, so the
 * typing logic is unit-testable and HIL-gateable with synthetic taps.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_ui.h"

/**
 * @enum ra_kbd_limits_t
 * @brief Static capacities for the keyboard widget.
 */
typedef enum : uint8_t {
  k_ra_kbd_rows     = 4U,   /**< Key rows (3 letter rows + SPACE/ENTER row). */
  k_ra_kbd_max_keys = 32U,  /**< Key-rect slots (29 used).                  */
  k_ra_kbd_text_max = 64U,  /**< Text buffer capacity incl. NUL.            */
  k_ra_kbd_no_hit   = 255U, /**< ::ra_kbd_hit "no key" sentinel.           */
} ra_kbd_limits_t;

/**
 * @enum ra_kbd_key_kind_t
 * @brief What a key does when tapped.
 */
typedef enum : uint8_t {
  k_ra_kbd_key_char      = 0U, /**< Append `ch`.                  */
  k_ra_kbd_key_space     = 1U, /**< Append a space.               */
  k_ra_kbd_key_backspace = 2U, /**< Delete the last char.         */
  k_ra_kbd_key_enter     = 3U, /**< Commit the query.             */
} ra_kbd_key_kind_t;

/**
 * @struct ra_kbd_key_t
 * @brief One key: a hit rectangle, a printable char, and a kind.
 */
typedef struct {
  ra_ui_rect_t      rect; /**< Hit / draw rectangle.                  */
  char              ch;   /**< Printable char for char keys; 0 else.  */
  ra_kbd_key_kind_t kind; /**< Key behaviour.                         */
} ra_kbd_key_t;

/**
 * @struct ra_kbd_layout_t
 * @brief The full key grid laid into a frame.
 */
typedef struct {
  ra_kbd_key_t keys[k_ra_kbd_max_keys]; /**< Key rects + chars.   */
  uint8_t      count;                   /**< Keys in use.         */
} ra_kbd_layout_t;

/**
 * @struct ra_kbd_text_t
 * @brief The typed query buffer + commit state.
 *
 * @invariant `len < k_ra_kbd_text_max`; `buf[len] == '\0'`.
 */
typedef struct {
  char    buf[k_ra_kbd_text_max]; /**< NUL-terminated query.     */
  uint8_t len;                    /**< Chars before the NUL.     */
  bool    committed;              /**< ENTER was applied.        */
} ra_kbd_text_t;

/**
 * @brief Lay the QWERTY key grid into @p frame.
 *
 * @details
 * Rows are stacked top-to-bottom inside @p frame; letter keys are evenly
 * spaced across the row width. Row layout: `QWERTYUIOP` / `ASDFGHJKL` /
 * `ZXCVBNM` + BACKSPACE / SPACE + ENTER.
 *
 * @param[out] kb    Receives the laid-out grid.
 * @param[in]  frame Pixel rectangle to fill (w,h > 0).
 *
 * @return ra_err_t
 * @retval k_ra_ok              Grid laid out.
 * @retval k_ra_err_null_ptr    @p kb or @p frame is NULL.
 * @retval k_ra_err_invalid_arg @p frame has non-positive w or h.
 *
 * @pre @p kb, @p frame non-NULL.
 * @pre `frame->w > 0 && frame->h > 0`.
 * @post On success `kb->count == 29`.
 * @post Every key rect lies within @p frame.
 *
 * @note Pure; not thread-safe relative to @p kb.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kbd_layout_init(ra_kbd_layout_t* kb, const ra_ui_rect_t* frame);

/**
 * @brief Map a tap to a key index.
 *
 * @param[in] kb Laid-out grid.
 * @param[in] px Tap X (pixels).
 * @param[in] py Tap Y (pixels).
 *
 * @return Key index `0..kb->count-1`, or ::k_ra_kbd_no_hit if the tap hit no
 *         key (or @p kb is NULL).
 *
 * @pre @p kb is laid out (or NULL, handled).
 * @pre None.
 * @post No state modified.
 * @post Return is a valid index or ::k_ra_kbd_no_hit.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra_kbd_hit(const ra_kbd_layout_t* kb, int32_t px, int32_t py);

/**
 * @brief Reset a text buffer to empty / uncommitted.
 *
 * @param[out] t Buffer to clear.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Cleared.
 * @retval k_ra_err_null_ptr @p t is NULL.
 *
 * @pre @p t non-NULL.
 * @pre None.
 * @post `t->len == 0 && t->buf[0] == '\0' && !t->committed`.
 * @post Buffer ready for ::ra_kbd_apply.
 *
 * @note Not thread-safe relative to @p t.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kbd_text_init(ra_kbd_text_t* t);

/**
 * @brief Apply key @p key_idx to the text buffer.
 *
 * @details
 * Char/space append when there is room; backspace deletes the last char when
 * non-empty; enter sets `committed`. Out-of-range or full/empty edge cases are
 * no-ops (still `k_ra_ok`).
 *
 * @param[in,out] t       Text buffer.
 * @param[in]     kb      Laid-out grid (for the key's kind/char).
 * @param[in]     key_idx Key index from ::ra_kbd_hit.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Applied (possibly a no-op at a capacity edge).
 * @retval k_ra_err_null_ptr @p t or @p kb is NULL.
 *
 * @pre @p t, @p kb non-NULL.
 * @pre @p key_idx is a hit index or ::k_ra_kbd_no_hit (no-op).
 * @post `t->buf` stays NUL-terminated and `t->len < k_ra_kbd_text_max`.
 * @post `committed` is true iff an ENTER key was applied.
 *
 * @note Not thread-safe relative to @p t.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kbd_apply(ra_kbd_text_t* t, const ra_kbd_layout_t* kb, uint8_t key_idx);

#ifdef __cplusplus
}
#endif
