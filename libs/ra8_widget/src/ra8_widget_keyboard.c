/**
 * @file ra8_widget_keyboard.c
 * @brief On-screen-keyboard leaf widget for the ra8_widget tree (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader search keyboard into a reusable ::ra8_widget leaf. The
 * widget owns the key drawing (a bordered face + centred glyph / label) and the
 * tap routing (hit -> apply -> commit edge -> `on_commit`); the `ra8_keyboard`
 * engine is reached through the injected ::ra8_widget_keyboard_ops_t seam, so the
 * file (and the whole `ra8_widget` library) never links `ra8_keyboard`. Painting
 * goes through the injected ::ra8_widget_paint_t backend, so the render + route
 * logic is host-testable with a recording mock seam + paint.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_keyboard.h"

#include "ra8_check.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_keyboard";

/**
 * @enum ra8_widget_kbd_geom_t
 * @brief Glyph-buffer indices (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_widget_kbd_glyph_char = 0U, /**< Glyph char slot in the 2-byte buffer. */
  k_ra8_widget_kbd_glyph_nul  = 1U, /**< NUL terminator slot.                  */
  k_ra8_widget_kbd_glyph_len  = 2U, /**< Single-glyph string buffer length.    */
  k_ra8_widget_kbd_no_glyph   = 0U, /**< `glyph == 0`: not a character key.    */
} ra8_widget_kbd_geom_t;

/**
 * @brief Draw one key: a bordered face plus its centred glyph or label.
 * @details Fills the key's `key_face` face (framed by `border_w`), then centres
 *          its text -- a single-character string for a character key, or the
 *          special-key `label` -- through the paint backend. A key with neither a
 *          glyph nor a label draws just its face.
 * @param[in] kbd  The keyboard descriptor (non-NULL; `paint` non-NULL).
 * @param[in] info The key's geometry + glyph / label (non-NULL).
 * @return Nothing.
 * @pre @p kbd and @p info are non-NULL; @p kbd->paint is non-NULL.
 * @pre None.
 * @post At most a framed face plus one text draw are issued for the key.
 * @post No text is drawn when the backend has no `draw_text`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_kbd_key(const ra8_widget_keyboard_t* kbd, const ra8_widget_key_info_t* info)
{
  const ra8_widget_paint_t* p = kbd->paint;
  priv_widget_fill_box(p, &info->rect, kbd->key_face, kbd->key_border, kbd->border_w);
  if (p->draw_text == nullptr) {
    return;
  }
  char        glyph_buf[k_ra8_widget_kbd_glyph_len] = {};
  const char* text                                  = info->label;
  if (info->glyph != (char)k_ra8_widget_kbd_no_glyph) {
    glyph_buf[k_ra8_widget_kbd_glyph_char] = info->glyph;
    glyph_buf[k_ra8_widget_kbd_glyph_nul]  = '\0';
    text                                   = glyph_buf;
  }
  if (text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  priv_widget_text_pos(p, &info->rect, text, (int16_t)0, k_ra8_widget_align_center, &tx, &ty);
  p->draw_text(p->user, tx, ty, text, kbd->key_fg, kbd->key_face);
}

/**
 * @brief Keyboard vtable render: fill the background, draw every key.
 * @details Reads the ::ra8_widget_keyboard_t from `w->ctx`, fills the widget rect
 *          with `bg`, then -- when the seam supplies `count` + `key_info` -- asks
 *          it for each key and paints it via ::internal_kbd_key. An inert seam or
 *          a missing paint backend is a no-op beyond the background.
 * @param[in] w The keyboard widget (its `ctx` is a ::ra8_widget_keyboard_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid keyboard descriptor or is NULL.
 * @post At most a background fill plus `count` key paints are issued.
 * @post No pixels are touched when the keyboard has no paint backend.
 * @note Not thread-safe; loop bounded by the seam's `count` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_kbd_render(ra8_widget_t* w)
{
  ra8_widget_keyboard_t* kbd = (ra8_widget_keyboard_t*)w->ctx;
  if (kbd == nullptr) {
    return;
  }
  if (kbd->paint == nullptr) {
    return;
  }
  priv_widget_fill_box(kbd->paint, &w->rect, kbd->bg, kbd->bg, (int16_t)0);
  if ((kbd->ops == nullptr) || (kbd->ops->count == nullptr) || (kbd->ops->key_info == nullptr)) {
    return;
  }
  const uint8_t n = kbd->ops->count(kbd->ops->user);
  for (uint8_t i = 0U; i < n; ++i) {
    ra8_widget_key_info_t info = {};
    kbd->ops->key_info(kbd->ops->user, i, &info);
    internal_kbd_key(kbd, &info);
  }
}

/**
 * @brief Keyboard vtable input: hit a key, apply it, fire the commit edge.
 * @details A touch is mapped to a key via the seam's `hit`; a real key is applied
 *          via `apply` (which returns whether the query is now committed), the
 *          widget self-invalidates for a redraw, and `on_commit` fires when
 *          `apply` reports a commit. Any touch is consumed (the keyboard owns its
 *          band); a button event is declined.
 * @param[in] w  The keyboard widget (its `ctx` is a ::ra8_widget_keyboard_t).
 * @param[in] ev The event already routed to this keyboard.
 * @return true if a touch was consumed, false otherwise.
 * @retval true  @p ev was a touch (key applied or a gap tap).
 * @retval false @p ev was not a touch (or `ctx` is NULL).
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid keyboard descriptor or is NULL.
 * @post On a key hit the seam applied it and `w` is dirty.
 * @post `on_commit` fires exactly when `apply` reports a commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_kbd_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_keyboard_t* kbd = (ra8_widget_keyboard_t*)w->ctx;
  if (kbd == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  if ((kbd->ops == nullptr) || (kbd->ops->hit == nullptr) || (kbd->ops->apply == nullptr)) {
    return true;
  }
  const uint8_t key = kbd->ops->hit(kbd->ops->user, ev->x, ev->y);
  if (key == (uint8_t)k_ra8_widget_key_no_hit) {
    return true;
  }
  const bool committed = kbd->ops->apply(kbd->ops->user, key);
  (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_quality);
  if (committed && (kbd->on_commit != nullptr)) {
    kbd->on_commit(w);
  }
  return true;
}

/** @brief The single immutable vtable shared by every on-screen keyboard. */
static const ra8_widget_vtable_t s_kbd_vt = {
  .measure  = nullptr,
  .render   = internal_kbd_render,
  .on_input = internal_kbd_on_input,
};

const ra8_widget_vtable_t* ra8_widget_keyboard_vtable(void)
{
  return &s_kbd_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_keyboard_init(ra8_widget_t* w, ra8_widget_keyboard_t* kbd)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(kbd, s_tag, "kbd must not be nullptr");
  w->vt      = ra8_widget_keyboard_vtable();
  w->ctx     = kbd;
  w->visible = true;
  return k_ra8_ok;
}
