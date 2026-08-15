/**
 * @file ra8_widget_book.c
 * @brief Book-card + book-grid leaf widget for the ra8_widget tree (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader Library book grid into a reusable ::ra8_widget leaf. The
 * grid geometry (::internal_bg_content, ::internal_bg_cell) is shared between
 * render (paint each card) and the hit maths (::internal_bg_on_input), so a tap
 * opens the card it looks like it opens. Each card stacks a cover swatch, a title
 * row, an author row, and a progress bar, painted through the injected
 * ::ra8_widget_paint_t backend -- so the file carries no `ra8_gfx` dependency and
 * the whole grid is host-testable with a recording mock paint. The card progress
 * bar reuses the shared ::priv_widget_fill_frac helper.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_book.h"

#include "ra8_check.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_book_grid";

/**
 * @enum ra8_widget_book_geom_t
 * @brief Grid geometry constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_ra8_widget_book_min_cols = 1U,   /**< Minimum columns; `cols == 0` -> 1. */
  k_ra8_widget_book_pct_full = 100U, /**< Full read-progress percentage.     */
  k_ra8_widget_book_none     = 0U,   /**< Empty grid (no cards).             */
} ra8_widget_book_geom_t;

/**
 * @brief Inset the grid rect by its padding to the content rectangle.
 * @details Shrinks @p rect by @p pad on every side, so the cards tile inside the
 *          padded content area rather than against the grid edge. Split out so
 *          render and hit-testing derive the same content rect.
 * @param[in] rect The grid widget rect (non-NULL).
 * @param[in] pad  Inner padding, pixels.
 * @return The content rectangle (grid rect inset by @p pad on every side).
 * @retval r The inset content rect.
 * @pre @p rect is non-NULL.
 * @pre None.
 * @post The returned rect lies within @p rect.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_bg_content(const ra8_ui_rect_t* rect, int16_t pad)
{
  const int32_t p = (int32_t)pad;
  ra8_ui_rect_t r = {};
  r.x             = rect->x + p;
  r.y             = rect->y + p;
  r.w             = rect->w - (p + p);
  r.h             = rect->h - (p + p);
  return r;
}

/**
 * @brief Compute card @p idx's cell inside the content rect.
 * @details Tiles @p content into @p cols x @p rows equal cells separated by
 *          @p gap; card `idx` sits at column `idx % cols`, row `idx / cols`.
 *          Shared by render and hit-testing so the drawn card and its tap target
 *          coincide.
 * @param[in] content The grid content rect (non-NULL).
 * @param[in] idx     Card index.
 * @param[in] cols    Column count (>= 1).
 * @param[in] rows    Row count (>= 1).
 * @param[in] gap     Gap between cells, pixels.
 * @return The card's cell rectangle.
 * @retval cell The @p idx-th card cell.
 * @pre @p content is non-NULL; @p cols >= 1; @p rows >= 1.
 * @pre None.
 * @post The returned cell lies within @p content.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_bg_cell(const ra8_ui_rect_t* content,
                                      uint16_t             idx,
                                      uint16_t             cols,
                                      uint16_t             rows,
                                      int16_t              gap)
{
  const int32_t g      = (int32_t)gap;
  const int32_t col    = (int32_t)(idx % cols);
  const int32_t row    = (int32_t)(idx / cols);
  const int32_t cell_w = (content->w - (g * (int32_t)(cols - 1U))) / (int32_t)cols;
  const int32_t cell_h = (content->h - (g * (int32_t)(rows - 1U))) / (int32_t)rows;
  ra8_ui_rect_t cell   = {};
  cell.x               = content->x + (col * (cell_w + g));
  cell.y               = content->y + (row * (cell_h + g));
  cell.w               = cell_w;
  cell.h               = cell_h;
  return cell;
}

/**
 * @brief Draw one card label (title / author) left-aligned in its row.
 * @details Places @p text at the left inset of @p row via
 *          ::priv_widget_text_pos and draws it, skipping a NULL @p text. The
 *          title and author rows share this one branch-coverable helper.
 * @param[in] paint Draw backend (non-NULL; `draw_text` already checked non-NULL).
 * @param[in] row   The label row rect (non-NULL).
 * @param[in] text  Label text, or NULL to skip.
 * @param[in] fg    Text colour, 0xRRGGBB.
 * @param[in] bg    Background colour, 0xRRGGBB.
 * @return Nothing.
 * @pre @p paint, @p row non-NULL; @p paint->draw_text non-NULL.
 * @pre None.
 * @post At most one `draw_text` call is issued.
 * @post No draw call is issued when @p text is NULL.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bg_label(const ra8_widget_paint_t* paint,
                              const ra8_ui_rect_t*      row,
                              const char*               text,
                              uint32_t                  fg,
                              uint32_t                  bg)
{
  if (text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  priv_widget_text_pos(paint, row, text, (int16_t)0, k_ra8_widget_align_left, &tx, &ty);
  paint->draw_text(paint->user, tx, ty, text, fg, bg);
}

/**
 * @brief Paint one book card (cover / title / author / progress) into a cell.
 * @details Stacks a cover swatch (the remaining height above the labels), the
 *          `label_h`-tall title and author rows, and the `bar_h`-tall progress
 *          bar at the bottom. The cover is skipped when the cell is too short to
 *          hold one; the labels only draw when the backend has `draw_text`.
 * @param[in] g    The book grid descriptor (non-NULL; `paint` non-NULL).
 * @param[in] book The card's data record (non-NULL).
 * @param[in] cell The card's cell rectangle (non-NULL).
 * @return Nothing.
 * @pre @p g, @p book, @p cell are non-NULL; @p g->paint is non-NULL.
 * @pre None.
 * @post At most a cover fill, two label draws, and two bar fills run.
 * @post No pixels spill outside @p cell.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bg_card(const ra8_widget_book_grid_t* g,
                             const ra8_widget_book_t*      book,
                             const ra8_ui_rect_t*          cell)
{
  const ra8_widget_paint_t* p       = g->paint;
  const int32_t             bar_h   = (int32_t)g->bar_h;
  const int32_t             lbl_h   = (int32_t)g->label_h;
  const int32_t             bar_y   = (cell->y + cell->h) - bar_h;
  const int32_t             auth_y  = bar_y - lbl_h;
  const int32_t             title_y = auth_y - lbl_h;
  const ra8_ui_rect_t       cover   = {cell->x, cell->y, cell->w, title_y - cell->y};
  if (cover.h > 0) {
    priv_widget_fill_box(p, &cover, book->cover, book->cover, (int16_t)0);
  }
  if (p->draw_text != nullptr) {
    const ra8_ui_rect_t title = {cell->x, title_y, cell->w, lbl_h};
    internal_bg_label(p, &title, book->title, g->title_fg, book->cover);
    const ra8_ui_rect_t author = {cell->x, auth_y, cell->w, lbl_h};
    internal_bg_label(p, &author, book->author, g->author_fg, book->cover);
  }
  const ra8_ui_rect_t bar = {cell->x, bar_y, cell->w, bar_h};
  priv_widget_fill_box(p, &bar, g->bar_track, g->bar_track, (int16_t)0);
  const int32_t fw =
    priv_widget_fill_frac(book->percent, (uint32_t)k_ra8_widget_book_pct_full, cell->w);
  if ((fw > 0) && (p->fill_rect != nullptr)) {
    p->fill_rect(p->user, cell->x, bar_y, fw, bar_h, g->bar_fill);
  }
}

/**
 * @brief Row count for @p count cards laid @p cols wide (ceil division).
 * @details Rounds `count / cols` up so a partial final row still gets a row of
 *          height, which ::internal_bg_cell needs to size cells on the cross
 *          axis. Both render and hit-testing call it with the clamped `cols`.
 * @param[in] count Number of cards (>= 1).
 * @param[in] cols  Columns (>= 1).
 * @return The number of grid rows (>= 1).
 * @retval rows ceil(count / cols).
 * @pre @p count >= 1; @p cols >= 1.
 * @pre None.
 * @post The result is >= 1.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_bg_rows(uint16_t count, uint16_t cols)
{
  return (uint16_t)(((uint32_t)count + (uint32_t)cols - 1U) / (uint32_t)cols);
}

/**
 * @brief Book-grid vtable render: fill the background, tile + paint the cards.
 * @details Reads the ::ra8_widget_book_grid_t from `w->ctx`, fills the grid with
 *          `bg`, then tiles the `count` cards into `cols` columns and paints each
 *          via ::internal_bg_card. An empty grid or a missing paint backend /
 *          book array is a no-op.
 * @param[in] w The book-grid widget (its `ctx` is a ::ra8_widget_book_grid_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid grid descriptor or is NULL.
 * @post At most a background fill plus `count` card paints are issued.
 * @post No pixels are touched when the grid has no paint backend.
 * @note Not thread-safe; loop bounded by `count` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bg_render(ra8_widget_t* w)
{
  ra8_widget_book_grid_t* g = (ra8_widget_book_grid_t*)w->ctx;
  if (g == nullptr) {
    return;
  }
  if (g->paint == nullptr) {
    return;
  }
  priv_widget_fill_box(g->paint, &w->rect, g->bg, g->bg, (int16_t)0);
  if ((g->count == (uint16_t)k_ra8_widget_book_none) || (g->books == nullptr)) {
    return;
  }
  const uint16_t      cols    = (g->cols >= (uint16_t)k_ra8_widget_book_min_cols)
                                  ? g->cols
                                  : (uint16_t)k_ra8_widget_book_min_cols;
  const uint16_t      rows    = internal_bg_rows(g->count, cols);
  const ra8_ui_rect_t content = internal_bg_content(&w->rect, g->pad);
  for (uint16_t i = 0U; i < g->count; ++i) {
    const ra8_ui_rect_t cell = internal_bg_cell(&content, i, cols, rows, g->gap);
    internal_bg_card(g, &g->books[i], &cell);
  }
}

/**
 * @brief Book-grid vtable input: route a touch to the card it hit.
 * @details Recomputes the same grid tiling as render and finds the card whose
 *          cell contains the tap; on a hit it records `selected`, self-invalidates
 *          fast, fires the optional `on_open`, and consumes the event. A non-touch
 *          event, an empty grid, or a tap in no cell is declined.
 * @param[in] w  The book-grid widget (its `ctx` is a ::ra8_widget_book_grid_t).
 * @param[in] ev The event already routed to this grid.
 * @return true if a card touch was routed, false otherwise.
 * @retval true  @p ev was a touch on a card and it was routed.
 * @retval false Non-touch, empty grid, a tap in no card, or `ctx` is NULL.
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid grid descriptor or is NULL.
 * @post On a card touch `selected` holds the hit index and `w` is dirty/fast.
 * @post No state changes on a declined event.
 * @note Not thread-safe; loop bounded by `count` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_bg_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_book_grid_t* g = (ra8_widget_book_grid_t*)w->ctx;
  if (g == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  if (g->count == (uint16_t)k_ra8_widget_book_none) {
    return false;
  }
  const uint16_t      cols    = (g->cols >= (uint16_t)k_ra8_widget_book_min_cols)
                                  ? g->cols
                                  : (uint16_t)k_ra8_widget_book_min_cols;
  const uint16_t      rows    = internal_bg_rows(g->count, cols);
  const ra8_ui_rect_t content = internal_bg_content(&w->rect, g->pad);
  for (uint16_t i = 0U; i < g->count; ++i) {
    const ra8_ui_rect_t cell = internal_bg_cell(&content, i, cols, rows, g->gap);
    if (ra8_ui_rect_contains(&cell, ev->x, ev->y)) {
      g->selected = i;
      (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_fast);
      if (g->on_open != nullptr) {
        g->on_open(w, i);
      }
      return true;
    }
  }
  return false;
}

/** @brief The single immutable vtable shared by every book grid. */
static const ra8_widget_vtable_t s_bg_vt = {
  .measure  = nullptr,
  .render   = internal_bg_render,
  .on_input = internal_bg_on_input,
};

const ra8_widget_vtable_t* ra8_widget_book_grid_vtable(void)
{
  return &s_bg_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_book_grid_init(ra8_widget_t* w, ra8_widget_book_grid_t* grid)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(grid, s_tag, "grid must not be nullptr");
  w->vt      = ra8_widget_book_grid_vtable();
  w->ctx     = grid;
  w->visible = true;
  return k_ra8_ok;
}
