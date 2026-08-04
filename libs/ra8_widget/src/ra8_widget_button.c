/**
 * @file ra8_widget_button.c
 * @brief Push-button leaf widget for the ra8_widget tree model (#145 Phase 2).
 *
 * @details
 * The interactive counterpart to the text label: a bordered, filled face with
 * an aligned label that **latches on a tap**. `render` paints the face (a
 * different fill while pressed) and label through the injected
 * ::ra8_widget_paint_t backend; `on_input` consumes a touch the dispatcher has
 * already hit-tested onto it -- flipping `pressed`, bumping the `presses`
 * counter, self-invalidating with the fast hint (so only this button's rect
 * re-flushes through the panel's damage cycle), then firing the optional
 * `on_press` callback. Button-kind events are declined so they keep routing.
 * Like the rest of `ra8_widget`, the file carries no `ra8_gfx` dependency.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_check.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_button";

/**
 * @brief Pick the face fill colour for the button's current state.
 * @details Folds the pressed/released face selection out of the render body so
 *          it stays a single, branch-coverable decision.
 * @param[in] b The button descriptor (non-NULL).
 * @return `face_pressed` when latched, else `face`.
 * @retval b->face_pressed When `b->pressed` is true.
 * @retval b->face         When `b->pressed` is false.
 * @pre @p b is non-NULL.
 * @pre None.
 * @post No state is modified.
 * @post Return is one of the two configured face colours.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_button_face(const ra8_widget_button_t* b)
{
  if (b->pressed) {
    return b->face_pressed;
  }
  return b->face;
}

/**
 * @brief Button vtable render: paint the bordered face, then the label.
 * @details Reads the ::ra8_widget_button_t from `w->ctx`, fills its (optionally
 *          bordered) face for the current pressed state via
 *          ::ra8_widget_priv_fill_box, then -- when there is text and a
 *          `draw_text` backend -- places and draws the label over the face.
 * @param[in] w The button widget (its `ctx` is a ::ra8_widget_button_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid button descriptor or is NULL.
 * @post At most the face fill(s) plus one text draw are issued.
 * @post No pixels are touched when the button has no paint backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_button_render(ra8_widget_t* w)
{
  ra8_widget_button_t* b = (ra8_widget_button_t*)w->ctx;
  if (b == nullptr) {
    return;
  }
  if (b->paint == nullptr) {
    return;
  }
  const uint32_t face = internal_button_face(b);
  ra8_widget_priv_fill_box(b->paint, &w->rect, face, b->border, b->border_w);
  if (b->text == nullptr) {
    return;
  }
  if (b->paint->draw_text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  ra8_widget_priv_text_pos(b->paint, &w->rect, b->text, b->pad, b->align, &tx, &ty);
  b->paint->draw_text(b->paint->user, tx, ty, b->text, b->fg, face);
}

/**
 * @brief Button vtable input: latch a touch, decline everything else.
 * @details A touch routed onto this button flips `pressed`, increments
 *          `presses`, marks the button dirty with the fast hint (so the panel
 *          flushes only this rect next compose), fires `on_press` if set, and
 *          reports the event consumed. Non-touch events return false so the
 *          dispatcher keeps offering them to other widgets.
 * @param[in] w  The button widget (its `ctx` is a ::ra8_widget_button_t).
 * @param[in] ev The event already routed to this button.
 * @return true if a touch was latched, false otherwise.
 * @retval true  @p ev was a touch and the button latched it.
 * @retval false @p ev was not a touch (or `ctx` is NULL).
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid button descriptor or is NULL.
 * @post On a touch `pressed` toggled and `presses` grew by one.
 * @post On a non-touch event no button state changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_button_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_button_t* b = (ra8_widget_button_t*)w->ctx;
  if (b == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  b->pressed = !b->pressed;
  b->presses++;
  (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_fast);
  if (b->on_press != nullptr) {
    b->on_press(w);
  }
  return true;
}

/** @brief The single immutable vtable shared by every push button. */
static const ra8_widget_vtable_t s_button_vt = {
  .measure  = nullptr,
  .render   = internal_button_render,
  .on_input = internal_button_on_input,
};

const ra8_widget_vtable_t* ra8_widget_button_vtable(void)
{
  return &s_button_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_button_init(ra8_widget_t* w, ra8_widget_button_t* button)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(button, s_tag, "button must not be nullptr");
  w->vt      = ra8_widget_button_vtable();
  w->ctx     = button;
  w->visible = true;
  return k_ra8_ok;
}
