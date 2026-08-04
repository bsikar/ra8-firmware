/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_book.h
 * @brief Book-card + book-grid leaf widget for the ra8_widget tree (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The e-reader Library screen is a grid of book cards: each card is a cover
 * swatch above a title, an author line, and a read-progress bar, and tapping a
 * card opens the book. ::ra8_widget_book_grid_t extracts that whole grid into one
 * reusable ::ra8_widget leaf, driven by an array of ::ra8_widget_book_t records.
 * `render` fills the grid background, tiles the cards into an N-column grid, and
 * paints each card (cover / title / author / progress) through the injected
 * ::ra8_widget_paint_t backend, so the widget carries no `ra8_gfx` dependency.
 * `on_input` maps a touch to the card it lands on, records it in `selected`, and
 * fires the optional `on_open` callback.
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
#include "ra8_widget.h"

/**
 * @struct ra8_widget_book_t
 * @brief One book card's data: cover colour, title, author, read progress.
 *
 * @details
 * Pure data consumed by ::ra8_widget_book_grid_t -- the grid owns the paint
 * backend and the colours, so a card record is just the per-book content. The
 * cover is a solid `cover` swatch (a real build would blit cover art here);
 * `percent` is the 0..100 read progress shown in the card's bar (values above
 * 100 saturate to a full bar).
 *
 * @invariant `percent` is treated as a 0..100 percentage (clamped on render).
 *
 * @see ra8_widget_book_grid_t
 * @since 0.1.0
 */
typedef struct ra8_widget_book {
  const char* title;   /**< Title line (NUL-terminated; NULL -> none).  */
  const char* author;  /**< Author line (NUL-terminated; NULL -> none). */
  uint32_t    cover;   /**< Cover swatch fill colour, 0xRRGGBB.         */
  uint16_t    percent; /**< Read progress 0..100 (saturates at 100).    */
} ra8_widget_book_t;

/**
 * @struct ra8_widget_book_grid_t
 * @brief A tappable N-column grid of book cards.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_book_grid_init. `books` is a caller-owned array of `count`
 * records tiled into `cols` columns (rows follow from the count), inset by `pad`
 * and separated by `gap`. Each card stacks a cover, a `label_h`-tall title row, a
 * `label_h`-tall author row, and a `bar_h`-tall progress bar. `render` paints the
 * background then every card; `on_input` records the tapped card in `selected`
 * and fires `on_open`.
 *
 * @invariant `cols >= 1` (a `cols` of 0 is treated as 1 column).
 * @invariant `selected` holds a valid card index after any touch is consumed.
 *
 * @par Example:
 * @code
 * static const ra8_widget_book_t books[] = {
 *   {.title = "Dune", .author = "Herbert", .cover = 0x00806040U, .percent = 42},
 *   {.title = "1984", .author = "Orwell",  .cover = 0x00404060U, .percent = 100},
 * };
 * ra8_widget_book_grid_t grid = {.paint = &paint, .books = books, .count = 2,
 *                                .cols = 2, .bg = 0x00FFFFFFU, .pad = 8,
 *                                .gap = 8, .label_h = 16, .bar_h = 4, ...};
 * ra8_widget_t w = {};
 * (void)ra8_widget_book_grid_init(&w, &grid);
 * @endcode
 *
 * @see ra8_widget_book_grid_init
 * @see ra8_widget_book_t
 * @since 0.1.0
 */
typedef struct ra8_widget_book_grid {
  const ra8_widget_paint_t* paint; /**< Draw backend (NULL -> draws nothing). */
  const ra8_widget_book_t*  books; /**< Array of `count` book records.        */
  /** Tap callback (opt/NULL). */
  void (*on_open)(struct ra8_widget* w, uint16_t index);
  uint32_t bg;        /**< Grid background colour, 0xRRGGBB.     */
  uint32_t title_fg;  /**< Title text colour, 0xRRGGBB.          */
  uint32_t author_fg; /**< Author text colour, 0xRRGGBB.         */
  uint32_t bar_track; /**< Card progress track colour, 0xRRGGBB. */
  uint32_t bar_fill;  /**< Card progress fill colour, 0xRRGGBB.  */
  uint16_t count;     /**< Number of book cards.                 */
  uint16_t cols;      /**< Grid columns (>= 1).                  */
  uint16_t selected;  /**< Last-tapped card index (observable).  */
  int16_t  pad;       /**< Grid inner padding, pixels.           */
  int16_t  gap;       /**< Gap between cards, pixels.            */
  int16_t  label_h;   /**< Title / author row height, pixels.    */
  int16_t  bar_h;     /**< Card progress-bar height, pixels.     */
} ra8_widget_book_grid_t;

/**
 * @brief Return the shared vtable backing every book-grid widget.
 *
 * @details
 * One immutable vtable serves all book grids: `render` fills the background and
 * tiles + paints every card through the grid's ::ra8_widget_paint_t; `on_input`
 * maps a touch to a card and fires `on_open`; `measure` is NULL.
 * ::ra8_widget_book_grid_init binds it.
 *
 * @return Non-NULL pointer to the static book-grid vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_book_grid_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_book_grid_vtable(void);

/**
 * @brief Bind a widget instance to a book grid.
 *
 * @details
 * Wires @p w to render + route @p grid: sets its vtable to
 * ::ra8_widget_book_grid_vtable, points its `ctx` at @p grid, and makes it
 * visible. The caller still sets @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w    Widget to turn into a book grid (non-NULL).
 * @param[in]     grid Book-grid descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p grid.
 * @retval k_ra8_err_null_ptr @p w or @p grid is NULL.
 *
 * @pre @p w and @p grid are non-NULL.
 * @pre @p grid outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_book_grid_vtable()`, `w->ctx == grid`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_book_grid_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_book_grid_init(ra8_widget_t* w, ra8_widget_book_grid_t* grid);

#ifdef __cplusplus
}
#endif
