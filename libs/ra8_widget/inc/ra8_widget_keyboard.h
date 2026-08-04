/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_keyboard.h
 * @brief On-screen-keyboard leaf widget for the ra8_widget tree (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The e-reader search field opens the on-screen keyboard modelled by
 * `ra8_keyboard` (a pure key grid + hit-test + text buffer). ::ra8_widget_keyboard_t
 * wraps that as a reusable ::ra8_widget leaf: it **draws** each key (a bordered
 * face plus its current-case glyph or a special-key label) through the injected
 * ::ra8_widget_paint_t backend, and **routes** a tap to a key, applies it, and
 * fires an `on_commit` callback when the query is committed (RETURN).
 *
 * The `ra8_keyboard` engine is reached through an injected
 * ::ra8_widget_keyboard_ops_t seam (`count` / `key_info` / `hit` / `apply`), so
 * this widget -- and the whole `ra8_widget` library -- never links `ra8_keyboard`
 * directly. The app binds the seam to `ra8_kbd_hit` / `ra8_kbd_apply` /
 * `ra8_kbd_key_glyph`, keeping the key-drawing + routing logic host-testable with
 * a recording mock seam while the layout logic stays in `ra8_keyboard`.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/**
 * @enum ra8_widget_key_sentinel_t
 * @brief Sentinel returned by ::ra8_widget_keyboard_ops_t::hit for "no key".
 */
typedef enum : uint8_t {
  k_ra8_widget_key_no_hit = 255U, /**< The tap hit no key (a gap / off-grid). */
} ra8_widget_key_sentinel_t;

/**
 * @struct ra8_widget_key_info_t
 * @brief One key's drawable geometry + glyph, supplied by the keyboard seam.
 *
 * @details
 * The widget asks the seam for each key's rect and label so it can paint the grid
 * without knowing the layout: a character key carries its current-case `glyph`
 * (and a NULL `label`); a special key (space, backspace, shift, ...) carries a
 * short `label` string and a `glyph` of 0.
 *
 * @invariant Exactly one of `glyph != 0` / `label != NULL` is set for a drawn key.
 *
 * @see ra8_widget_keyboard_ops_t
 * @since 0.1.0
 */
typedef struct ra8_widget_key_info {
  ra8_ui_rect_t rect;  /**< Key hit / draw rectangle.                     */
  const char*   label; /**< Special-key label (NULL for a character key). */
  char          glyph; /**< Current-case char (0 for a special / no key). */
  uint8_t       pad0;  /**< Padding.                                      */
  uint16_t      pad1;  /**< Padding to a 4-byte boundary.                 */
} ra8_widget_key_info_t;

/**
 * @struct ra8_widget_keyboard_ops_t
 * @brief Injected seam driving an ra8_keyboard from the keyboard widget.
 *
 * @details
 * A Dependency-Inversion seam (like ::ra8_widget_paint_t) that keeps
 * `ra8_widget` free of any `ra8_keyboard` dependency. `count` reports the number
 * of keys; `key_info` fills each key's ::ra8_widget_key_info_t for drawing; `hit`
 * maps a tap to a key index (or ::k_ra8_widget_key_no_hit); `apply` applies a key
 * and returns whether the query is now committed. The app binds these to
 * `ra8_kbd_*`.
 *
 * @invariant `hit` returns an index `< count()` or ::k_ra8_widget_key_no_hit.
 *
 * @see ra8_widget_keyboard_t
 * @since 0.1.0
 */
typedef struct ra8_widget_keyboard_ops {
  /** @brief Opaque keyboard handle passed back to every callback below. */
  void* user;
  /** @brief Number of keys in the active layer. */
  uint8_t (*count)(void* user);
  /** @brief Fill @p out with key @p idx's rect + glyph / label for drawing. */
  void (*key_info)(void* user, uint8_t idx, ra8_widget_key_info_t* out);
  /** @brief Map a tap to a key index, or ::k_ra8_widget_key_no_hit. */
  uint8_t (*hit)(void* user, int32_t x, int32_t y);
  /** @brief Apply key @p idx; return true iff the query is now committed. */
  bool (*apply)(void* user, uint8_t idx);
} ra8_widget_keyboard_ops_t;

/**
 * @struct ra8_widget_keyboard_t
 * @brief An on-screen keyboard: key styling + engine seam + commit callback.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_keyboard_init. `render` fills the widget rect with `bg`, then draws
 * each key as a `border`-framed `key_face` face carrying its centred glyph /
 * label in `key_fg`. `on_input` maps a touch to a key via the seam, applies it,
 * self-invalidates for a redraw, and -- on the commit edge -- fires `on_commit`.
 *
 * @invariant `border_w >= 0`.
 *
 * @par Example:
 * @code
 * ra8_widget_keyboard_ops_t ops = {.user = &kb, .count = kb_count,
 *                                  .key_info = kb_info, .hit = kb_hit,
 *                                  .apply = kb_apply};
 * ra8_widget_keyboard_t kbd = {.paint = &paint, .ops = &ops,
 *                              .on_commit = run_search, .bg = 0x00202020U,
 *                              .key_face = 0x00404040U, .key_border = 0x00101010U,
 *                              .key_fg = 0x00FFFFFFU, .border_w = 1};
 * ra8_widget_t w = {};
 * (void)ra8_widget_keyboard_init(&w, &kbd);
 * @endcode
 *
 * @see ra8_widget_keyboard_init
 * @see ra8_widget_keyboard_ops_t
 * @since 0.1.0
 */
typedef struct ra8_widget_keyboard {
  const ra8_widget_paint_t*        paint;  /**< Draw backend (NULL -> draws nothing). */
  const ra8_widget_keyboard_ops_t* ops;    /**< Keyboard engine seam (NULL -> inert). */
  void (*on_commit)(struct ra8_widget* w); /**< Commit-edge callback (optional/NULL). */
  uint32_t bg;                             /**< Keyboard background colour, 0xRRGGBB. */
  uint32_t key_face;                       /**< Key face fill colour, 0xRRGGBB.       */
  uint32_t key_border;                     /**< Key border colour, 0xRRGGBB.          */
  uint32_t key_fg;                         /**< Key glyph / label colour, 0xRRGGBB.   */
  int16_t  border_w;                       /**< Key border thickness, px (>= 0).      */
  uint16_t reserved;                       /**< Padding to a 4-byte boundary.         */
} ra8_widget_keyboard_t;

/**
 * @brief Return the shared vtable backing every on-screen-keyboard widget.
 *
 * @details
 * One immutable vtable serves all keyboards: `render` fills the background and
 * draws every key through the keyboard's ::ra8_widget_paint_t + seam; `on_input`
 * maps a touch to a key, applies it, and fires `on_commit` on the commit edge;
 * `measure` is NULL. ::ra8_widget_keyboard_init binds it.
 *
 * @return Non-NULL pointer to the static keyboard vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_keyboard_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_keyboard_vtable(void);

/**
 * @brief Bind a widget instance to an on-screen keyboard.
 *
 * @details
 * Wires @p w to render + route @p kbd: sets its vtable to
 * ::ra8_widget_keyboard_vtable, points its `ctx` at @p kbd, and makes it visible.
 * The caller still sets @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w   Widget to turn into a keyboard (non-NULL).
 * @param[in]     kbd Keyboard descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p kbd.
 * @retval k_ra8_err_null_ptr @p w or @p kbd is NULL.
 *
 * @pre @p w and @p kbd are non-NULL.
 * @pre @p kbd outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_keyboard_vtable()`, `w->ctx == kbd`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_keyboard_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_keyboard_init(ra8_widget_t* w, ra8_widget_keyboard_t* kbd);

#ifdef __cplusplus
}
#endif
