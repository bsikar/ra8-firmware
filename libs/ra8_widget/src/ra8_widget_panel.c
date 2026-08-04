/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_panel.c
 * @brief Container-panel compositor for the ra8_widget tree model (#145).
 *
 * @details
 * A `panel` is a ::ra8_widget_t whose `ctx` points at a ::ra8_widget_panel_t
 * child array. Binding the shared panel vtable (::ra8_widget_panel_vtable) turns
 * a widget into a container, which is what lifts the flat widget array into a
 * **tree**: because a panel is itself a widget, a panel can be a child of
 * another panel. The compositor logic is delegated to the existing flat ops --
 * ::ra8_widget_layout_stack (layout), ::ra8_widget_damage (minimal flush rect)
 * and ::ra8_widget_render_dirty (composite) -- so this file adds only the
 * tree-recursion glue and stays free of any framebuffer dependency.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_widget.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_panel";

/**
 * @brief Lay a panel's children out inside a rectangle.
 * @details Thin forwarder to ::ra8_widget_layout_stack shared by the compose
 *          entry point and the nested-render callback, so each stays within the
 *          NASA Rule 4 size cap and the stack-layout call is written once.
 * @param[in] p    Panel descriptor (non-NULL; `children` covers `count`).
 * @param[in] rect Rectangle the children stack fills (non-NULL).
 * @return ra8_err_t Forwarded from ::ra8_widget_layout_stack.
 * @retval k_ra8_ok              Children laid out; each visible rect set.
 * @retval k_ra8_err_null_ptr    `p->children` or @p rect is NULL.
 * @retval k_ra8_err_invalid_arg Scratch too small for the visible child count.
 * @pre @p p and @p rect are non-NULL.
 * @pre @p p->box_scratch holds at least `count + 1` nodes.
 * @post On success every visible child has its `rect` assigned.
 * @post On failure no child rect is relied upon by the caller.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_panel_layout(const ra8_widget_panel_t* p, const ra8_ui_rect_t* rect)
{
  return ra8_widget_layout_stack(p->children,
                                 p->count,
                                 rect,
                                 p->axis,
                                 p->gap,
                                 p->pad,
                                 p->box_scratch,
                                 p->box_cap);
}

/**
 * @brief Validate ::ra8_widget_panel_compose arguments and extract the panel.
 * @details Split out so the compose entry point stays under the NASA Rule 4
 *          statement budget: it folds the five null guards plus the panel /
 *          child-array check into one place and hands back the panel pointer.
 * @param[in]  panel_w    The root panel widget (its `ctx` is the panel).
 * @param[in]  frame      Outer rectangle pointer to null-check.
 * @param[in]  out_damage Damage out-pointer to null-check.
 * @param[in]  out_hint   Refresh-hint out-pointer to null-check.
 * @param[in]  out_dirty  Dirty-count out-pointer to null-check.
 * @param[out] out_panel  Receives the validated panel descriptor.
 * @return ra8_err_t
 * @retval k_ra8_ok               Arguments valid; @p out_panel set.
 * @retval k_ra8_err_null_ptr     Any of the checked pointers is NULL.
 * @retval k_ra8_err_invalid_arg  @p panel_w is not a panel (NULL ctx/children).
 * @pre @p out_panel is non-NULL.
 * @pre The other pointers are the caller's compose arguments.
 * @post On success `*out_panel` references the panel's descriptor.
 * @post On failure `*out_panel` is not relied upon by the caller.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compose_check(ra8_widget_t*               panel_w,
                                        const ra8_ui_rect_t*        frame,
                                        const ra8_ui_rect_t*        out_damage,
                                        const ra8_widget_refresh_t* out_hint,
                                        const uint16_t*             out_dirty,
                                        ra8_widget_panel_t**        out_panel)
{
  RA8_CHECK_NULL_PTR(panel_w, s_tag, "panel_w must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  RA8_CHECK_NULL_PTR(out_damage, s_tag, "out_damage must not be nullptr");
  RA8_CHECK_NULL_PTR(out_hint, s_tag, "out_hint must not be nullptr");
  RA8_CHECK_NULL_PTR(out_dirty, s_tag, "out_dirty must not be nullptr");
  ra8_widget_panel_t* p = (ra8_widget_panel_t*)panel_w->ctx;
  if ((p == nullptr) || (p->children == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *out_panel = p;
  return k_ra8_ok;
}

/**
 * @brief Panel vtable render: lay out + composite the whole subtree.
 * @details Runs when a panel is a dirty child of another panel: it lays its
 *          children out inside the panel's `rect`, marks every visible child
 *          dirty with the panel's own refresh hint (a dirty panel repaints its
 *          whole subtree), then composites them via ::ra8_widget_render_dirty.
 *          A child that is itself a panel re-enters here, so the recursion is
 *          bounded by the caller's static tree depth (NASA Rule 1).
 * @param[in] w The panel widget (its `ctx` is a ::ra8_widget_panel_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid panel descriptor.
 * @post Every visible child has been rendered for this subtree.
 * @post No child remains dirty after the render pass.
 * @note Not thread-safe; loop bounded by `count` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_panel_render(ra8_widget_t* w)
{
  ra8_widget_panel_t* p = (ra8_widget_panel_t*)w->ctx;
  if ((p == nullptr) || (p->children == nullptr)) {
    return;
  }
  if (internal_panel_layout(p, &w->rect) != k_ra8_ok) {
    return;
  }
  const ra8_widget_refresh_t hint = (w->refresh == (uint8_t)k_ra8_widget_refresh_none)
                                      ? k_ra8_widget_refresh_quality
                                      : (ra8_widget_refresh_t)w->refresh;
  for (uint16_t i = 0U; i < p->count; ++i) {
    if (p->children[i].visible) {
      (void)ra8_widget_invalidate(&p->children[i], hint);
    }
  }
  (void)ra8_widget_render_dirty(p->children, p->count);
}

/**
 * @brief Panel vtable input: route the event down to the children.
 * @details Offers the event to the panel's children via ::ra8_widget_dispatch
 *          (touch routed by hit-test, button offered first-consumer). A child
 *          that is itself a panel re-enters here; depth is bounded by the
 *          caller's static tree (NASA Rule 1).
 * @param[in] w  The panel widget (its `ctx` is a ::ra8_widget_panel_t).
 * @param[in] ev The event already routed to this panel.
 * @return true if some child consumed the event, false otherwise.
 * @retval true  A child's `on_input` consumed @p ev.
 * @retval false No child consumed it (or the panel has no children).
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid panel descriptor.
 * @post No widget state changes beyond what a child's `on_input` performs.
 * @post The return reflects whether a child consumed @p ev.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_panel_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_panel_t* p = (ra8_widget_panel_t*)w->ctx;
  if ((p == nullptr) || (p->children == nullptr)) {
    return false;
  }
  bool handled = false;
  (void)ra8_widget_dispatch(p->children, p->count, ev, &handled);
  return handled;
}

/** @brief The single immutable vtable shared by every container panel. */
static const ra8_widget_vtable_t s_panel_vt = {
  .measure  = nullptr,
  .render   = internal_panel_render,
  .on_input = internal_panel_on_input,
};

const ra8_widget_vtable_t* ra8_widget_panel_vtable(void)
{
  return &s_panel_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_panel_init(ra8_widget_t* w, ra8_widget_panel_t* panel)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(panel, s_tag, "panel must not be nullptr");
  if (panel->count > 0U) {
    if (panel->children == nullptr) {
      return k_ra8_err_invalid_arg;
    }
    if ((uint32_t)panel->box_cap < ((uint32_t)panel->count + 1U)) {
      return k_ra8_err_invalid_arg;
    }
  }
  w->vt      = ra8_widget_panel_vtable();
  w->ctx     = panel;
  w->visible = true;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_panel_compose(ra8_widget_t*         panel_w,
                                                 const ra8_ui_rect_t*  frame,
                                                 ra8_ui_rect_t*        out_damage,
                                                 ra8_widget_refresh_t* out_hint,
                                                 uint16_t*             out_dirty)
{
  ra8_widget_panel_t* p = nullptr;
  const ra8_err_t     cerr =
    internal_compose_check(panel_w, frame, out_damage, out_hint, out_dirty, &p);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  panel_w->rect        = *frame;
  const ra8_err_t lerr = internal_panel_layout(p, &panel_w->rect);
  if (lerr != k_ra8_ok) {
    return lerr;
  }
  const ra8_err_t derr = ra8_widget_damage(p->children, p->count, out_damage, out_hint, out_dirty);
  if (derr != k_ra8_ok) {
    return derr;
  }
  const ra8_err_t rerr = ra8_widget_render_dirty(p->children, p->count);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  panel_w->dirty   = false;
  panel_w->refresh = (uint8_t)k_ra8_widget_refresh_none;
  return k_ra8_ok;
}
