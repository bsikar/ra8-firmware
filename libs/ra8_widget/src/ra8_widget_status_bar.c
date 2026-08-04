/**
 * @file ra8_widget_status_bar.c
 * @brief Status-bar leaf widget for the ra8_widget tree model (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader top band into a reusable ::ra8_widget leaf: a background
 * fill, a left-aligned + a right-aligned label, and a bottom hairline rule, all
 * painted through the injected ::ra8_widget_paint_t backend so the file carries
 * no `ra8_gfx` dependency. Text placement reuses the shared `RA8_PRIV`
 * ::ra8_widget_priv_text_pos helper (left / right alignment, vertical centring
 * when the backend can measure). Display-only: the vtable leaves `on_input` NULL.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_status_bar.h"

#include "ra8_check.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_status_bar";

/**
 * @brief No hairline: a `rule_h` at or below this draws no bottom rule.
 */
typedef enum : int16_t {
  k_ra8_widget_sb_no_rule = 0, /**< Threshold below which the rule is skipped. */
} ra8_widget_sb_geom_t;

/**
 * @brief Draw one aligned label on the status band through the paint backend.
 * @details Places @p text at @p align within @p rect via ::ra8_widget_priv_text_pos
 *          and draws it. A NULL @p text or a NULL `draw_text` backend is a no-op,
 *          so the left / right labels share one branch-coverable helper.
 * @param[in] paint Draw backend (non-NULL).
 * @param[in] rect  The band rectangle the text sits inside (non-NULL).
 * @param[in] text  NUL-terminated ASCII, or NULL to draw nothing.
 * @param[in] pad   Inner inset from the rect edges, pixels.
 * @param[in] align Horizontal alignment (left / right).
 * @param[in] fg    Text colour, 0xRRGGBB.
 * @param[in] bg    Background colour drawn behind the glyphs, 0xRRGGBB.
 * @return Nothing.
 * @pre @p paint and @p rect are non-NULL.
 * @pre @p text is NUL-terminated or NULL.
 * @post At most one `draw_text` call is issued.
 * @post No draw call is issued when @p text or `draw_text` is NULL.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sb_label(const ra8_widget_paint_t* paint,
                              const ra8_ui_rect_t*      rect,
                              const char*               text,
                              int16_t                   pad,
                              ra8_widget_align_t        align,
                              uint32_t                  fg,
                              uint32_t                  bg)
{
  if (text == nullptr) {
    return;
  }
  if (paint->draw_text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  ra8_widget_priv_text_pos(paint, rect, text, pad, align, &tx, &ty);
  paint->draw_text(paint->user, tx, ty, text, fg, bg);
}

/**
 * @brief Status-bar vtable render: fill, left + right labels, bottom hairline.
 * @details Reads the ::ra8_widget_status_bar_t from `w->ctx`, fills the rect with
 *          `bg`, draws the two labels via ::internal_sb_label, then -- when
 *          `rule_h > 0` -- fills a `rule` strip along the band's bottom edge. A
 *          missing paint backend / `fill_rect` callback keeps drawing safe.
 * @param[in] w The status-bar widget (its `ctx` is a ::ra8_widget_status_bar_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid status-bar descriptor or is NULL.
 * @post At most a background fill, two text draws, and one rule fill are issued.
 * @post No pixels are touched when the bar has no paint backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sb_render(ra8_widget_t* w)
{
  ra8_widget_status_bar_t* sb = (ra8_widget_status_bar_t*)w->ctx;
  if (sb == nullptr) {
    return;
  }
  if (sb->paint == nullptr) {
    return;
  }
  const ra8_ui_rect_t* r = &w->rect;
  ra8_widget_priv_fill_box(sb->paint, r, sb->bg, sb->bg, (int16_t)k_ra8_widget_sb_no_rule);
  internal_sb_label(sb->paint, r, sb->left, sb->pad, k_ra8_widget_align_left, sb->fg, sb->bg);
  internal_sb_label(sb->paint,
                    r,
                    sb->right,
                    sb->pad,
                    k_ra8_widget_align_right,
                    sb->fg_right,
                    sb->bg);
  if (sb->rule_h <= (int16_t)k_ra8_widget_sb_no_rule) {
    return;
  }
  if (sb->paint->fill_rect == nullptr) {
    return;
  }
  sb->paint->fill_rect(sb->paint->user,
                       r->x,
                       (r->y + r->h) - (int32_t)sb->rule_h,
                       r->w,
                       (int32_t)sb->rule_h,
                       sb->rule);
}

/** @brief The single immutable vtable shared by every status bar. */
static const ra8_widget_vtable_t s_sb_vt = {
  .measure  = nullptr,
  .render   = internal_sb_render,
  .on_input = nullptr,
};

const ra8_widget_vtable_t* ra8_widget_status_bar_vtable(void)
{
  return &s_sb_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_status_bar_init(ra8_widget_t* w, ra8_widget_status_bar_t* bar)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(bar, s_tag, "bar must not be nullptr");
  w->vt      = ra8_widget_status_bar_vtable();
  w->ctx     = bar;
  w->visible = true;
  return k_ra8_ok;
}
