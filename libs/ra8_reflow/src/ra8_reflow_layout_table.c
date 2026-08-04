/**
 * @file ra8_reflow_layout_table.c
 * @brief `<table>` equal-column grid layout for the ra8_reflow engine (#107).
 *
 * @details
 * Splits the table-layout sub-responsibility out of `ra8_reflow_layout.c` so
 * each translation unit stays under the project file-size cap. The grid is an
 * equal-column model: the content width is divided evenly by the maximum
 * per-row cell count, each cell flows its plain-text tokens greedily within
 * its column box, and rows page-break as a unit (rolling the tentative glyphs
 * back and re-laying the row at the top of a fresh page when it would
 * overflow).
 *
 * The driver enters this module through @ref ra8_reflow_layout_table; the
 * core inline-flow helpers it reuses (line height, glyph advance/push, line
 * wrap, page flush) are shared via `ra8_reflow_layout_internal.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_internal.h"
#include "ra8_reflow_layout_internal.h"
#include "stb_truetype.h"

/* ===========================================================================
 * Table layout (#107): equal-column grid, per-cell text flow, row page-break
 * ===========================================================================
 */

/**
 * @brief Find the block-end token matching the block-start at @p start.
 *
 * @details Scans `engine->tokens[]` forward from `start + 1` to `limit - 1`,
 * matching only tokens whose `tag` equals the tag at @p start. Each nested
 * block-start increments a depth counter; each block-end decrements it. When
 * depth reaches zero the index of that block-end is returned. If the scan
 * reaches @p limit without a match (malformed input), @p limit is returned so
 * callers can treat the entire remaining range as the cell or row content.
 *
 * @param[in] engine Engine holding the token stream.
 * @param[in] start  Index of the block-start token whose matching end is sought.
 * @param[in] limit  Exclusive upper bound for the scan (e.g. table block-end).
 *
 * @return Index of the matching block-end token, or @p limit if not found.
 * @retval limit No matching block-end token was found before @p limit.
 *
 * @pre `start < limit` and `start < engine->token_count`.
 * @pre `engine->tokens[start].kind == k_ra8_reflow_tok_block_start`.
 * @post Return value is in the range `[start + 1, limit]`.
 * @post Returned index (if < @p limit) refers to a block-end with the same tag.
 *
 * @note Pure read on the token stream; not thread-safe if the stream is concurrently
 *       mutated.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_match_block_end(const ra8_reflow_t* engine, uint32_t start, uint32_t limit)
{
  const uint8_t tag   = engine->tokens[start].tag;
  int32_t       depth = 1;
  for (uint32_t i = start + 1U; i < limit; ++i) {
    const ra8_reflow_token_t* tok = &engine->tokens[i];
    if (tok->tag != tag) {
      continue;
    }
    if (tok->kind == (uint8_t)k_ra8_reflow_tok_block_start) {
      depth++;
    } else if (tok->kind == (uint8_t)k_ra8_reflow_tok_block_end) {
      depth--;
      if (depth == 0) {
        return i;
      }
    }
  }
  return limit;
}

/**
 * @brief True iff token @p i opens a table cell (`<td>` or `<th>`).
 *
 * @details Checks both the token kind (must be `block_start`) and the tag
 * (must be `k_ra8_reflow_tag_td` or `k_ra8_reflow_tag_th`). Used by
 * `priv_table_columns` and `priv_row_cells` to locate cell boundaries in the
 * flat token stream without recursion.
 *
 * @param[in] engine Engine holding the token stream.
 * @param[in] i      Index into `engine->tokens[]` to test.
 *
 * @return Boolean cell-start predicate.
 * @retval true  Token at @p i is a `<td>` or `<th>` block-start.
 * @retval false Otherwise.
 *
 * @pre `i < engine->token_count`.
 * @pre `engine->tokens` is a valid pointer.
 * @post No state mutated.
 * @post Return value depends solely on `engine->tokens[i]`.
 *
 * @note Pure read; not thread-safe if the token stream is concurrently mutated.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_cell_start(const ra8_reflow_t* engine, uint32_t i)
{
  const ra8_reflow_token_t* tok = &engine->tokens[i];
  return (tok->kind == (uint8_t)k_ra8_reflow_tok_block_start) &&
         ((tok->tag == (uint8_t)k_ra8_reflow_tag_td) || (tok->tag == (uint8_t)k_ra8_reflow_tag_th));
}

/**
 * @brief True iff token @p i opens a table row (`<tr>`).
 *
 * @details Checks both the token kind (must be `block_start`) and the tag
 * (must be `k_ra8_reflow_tag_tr`). Used by `priv_table_columns` and
 * `ra8_reflow_layout_table` to locate row boundaries while scanning the flat
 * token stream between a table block-start and its matching block-end.
 *
 * @param[in] engine Engine holding the token stream.
 * @param[in] i      Index into `engine->tokens[]` to test.
 *
 * @return Boolean row-start predicate.
 * @retval true  Token at @p i is a `<tr>` block-start.
 * @retval false Otherwise.
 *
 * @pre `i < engine->token_count`.
 * @pre `engine->tokens` is a valid pointer.
 * @post No state mutated.
 * @post Return value depends solely on `engine->tokens[i]`.
 *
 * @note Pure read; not thread-safe if the token stream is concurrently mutated.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_row_start(const ra8_reflow_t* engine, uint32_t i)
{
  const ra8_reflow_token_t* tok = &engine->tokens[i];
  return (tok->kind == (uint8_t)k_ra8_reflow_tok_block_start) &&
         (tok->tag == (uint8_t)k_ra8_reflow_tag_tr);
}

/**
 * @brief Column count = the maximum number of cells in any row of the table.
 *
 * @details Scans the token range `(start, end)` for `<tr>` block-starts.
 * For each row it counts `<td>` / `<th>` block-start tokens between the row
 * open and its matching close (via @ref priv_match_block_end). The maximum
 * across all rows is returned; zero is returned for a table with no cells.
 * The scan advances by full row spans so deeply nested same-tag elements do
 * not distort the count.
 *
 * @param[in] engine Engine holding the parsed token stream.
 * @param[in] start  Index of the `<table>` block-start token.
 * @param[in] end    Index of the `<table>` block-end token (exclusive bound).
 *
 * @return Maximum cell count across all rows; 0 if the table has no cells.
 * @retval 0 The table span contains no cells.
 *
 * @pre `start < end` and `end <= engine->token_count`.
 * @pre `engine->tokens[start]` is a table block-start token.
 * @post No state mutated; pure scan.
 * @post Return value is the maximum per-row cell count or 0.
 *
 * @note Pure read on the token stream; not thread-safe if concurrently mutated.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_table_columns(const ra8_reflow_t* engine, uint32_t start, uint32_t end)
{
  uint32_t cols = 0U;
  uint32_t i    = start + 1U;
  while (i < end) {
    if (!priv_is_row_start(engine, i)) {
      i++;
      continue;
    }
    const uint32_t tr_end = priv_match_block_end(engine, i, end);
    uint32_t       cells  = 0U;
    for (uint32_t j = i + 1U; j < tr_end; ++j) {
      if (priv_is_cell_start(engine, j)) {
        cells++;
      }
    }
    if (cells > cols) {
      cols = cells;
    }
    i = tr_end + 1U;
  }
  return cols;
}

/**
 * @brief Flow one text token inside a cell box, wrapping at the column edge.
 *
 * @details Performs a greedy word-wrap of the text in @p tok within the pixel
 * range `[cell_x, cell_right)`, starting from the running pen at `(*cx, *cy)`.
 * Spaces are emitted individually only if the pen is past the left edge and the
 * space fits before @p cell_right; otherwise the space is silently dropped (no
 * leading whitespace on wrapped lines). Each word is pre-measured; if the word
 * plus the current pen x would exceed @p cell_right and the pen is not already
 * at the left edge, the pen wraps to `cell_x` on the next line and
 * `(*lines)++`. Glyphs are pushed via @ref ra8_reflow_layout_push_glyph with
 * style 0 and link_id 0 (table cells are always plain body text in v1).
 *
 * @param[in,out] engine     Engine whose glyph pool grows.
 * @param[in]     font       Font metrics for advance measurement.
 * @param[in]     tok        Text token to flow; only `text_off` / `text_len`
 *                           are used.
 * @param[in]     cell_x     Cell content left edge in pixels.
 * @param[in]     cell_right Cell content right edge in pixels.
 * @param[in]     font_px    Glyph size in pixels.
 * @param[in]     color      Packed ARGB glyph colour.
 * @param[in,out] cx         Running pen x position; updated in place.
 * @param[in,out] cy         Running pen baseline y; updated on line wrap.
 * @param[in,out] lines      Running line count; incremented on each wrap.
 *
 * @return Nothing.
 *
 * @pre `engine != nullptr`, `font != nullptr`, `tok != nullptr`.
 * @pre `cell_right > cell_x` and `cx != nullptr` and `cy != nullptr` and
 *      `lines != nullptr`.
 * @post `*lines >= 1` (starts at 1 before the first call, never decremented).
 * @post All glyphs from @p tok land in the range `[cell_x, cell_right)`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_cell_text(ra8_reflow_t*             engine,
                           const stbtt_fontinfo*     font,
                           const ra8_reflow_token_t* tok,
                           int32_t                   cell_x,
                           int32_t                   cell_right,
                           uint16_t                  font_px,
                           uint32_t                  color,
                           int32_t*                  cx,
                           int32_t*                  cy,
                           uint32_t*                 lines)
{
  const int32_t  line_h = (int32_t)ra8_reflow_layout_line_height(font_px);
  const uint8_t* base   = engine->text_pool + tok->text_off;
  const uint32_t len    = tok->text_len;
  uint32_t       i      = 0U;
  while (i < len) {
    if (base[i] == ' ') {
      const int32_t adv = ra8_reflow_layout_glyph_advance(font, font_px, (int32_t)' ');
      /* mcdc-deactivated: ra8_reflow_tok_stash_run collapses leading whitespace of every text run (last_ws starts true), so a cell text token never begins with a space, and the pen always advances past cell_x after a word is emitted; a space is therefore only ever seen with *cx > cell_x, making the (*cx > cell_x) false arm unreachable. */
      if ((*cx > cell_x) && ((*cx + adv) <= cell_right)) {
        (void)ra8_reflow_layout_push_glyph(engine, *cx, *cy, (int32_t)' ', font_px, 0U, color, 0U);
        *cx += adv;
      }
      ++i;
      continue;
    }
    uint32_t word_end = i;
    int32_t  word_w   = 0;
    while ((word_end < len) && (base[word_end] != ' ')) {
      word_w += ra8_reflow_layout_glyph_advance(font, font_px, (int32_t)base[word_end]);
      ++word_end;
    }
    if (((*cx + word_w) > cell_right) && (*cx > cell_x)) {
      *cx = cell_x;
      *cy += line_h;
      (*lines)++;
    }
    while (i < word_end) {
      const int32_t adv = ra8_reflow_layout_glyph_advance(font, font_px, (int32_t)base[i]);
      (void)
        ra8_reflow_layout_push_glyph(engine, *cx, *cy, (int32_t)base[i], font_px, 0U, color, 0U);
      *cx += adv;
      ++i;
    }
  }
}

/**
 * @brief Lay out one cell's text tokens within a fixed column box (left-flow).
 *
 * @details Iterates over tokens `[tstart, tend)` and, for each
 * `k_ra8_reflow_tok_text` token, delegates to @ref priv_cell_text which
 * performs greedy word-wrap within `[cell_x, cell_x + cell_w)` from @p top_y.
 * Non-text tokens inside the cell are ignored (v1 table cells hold plain text).
 * The running pen and line counter are local to this call; the return value
 * conveys the height consumed so the row can allocate vertical space for the
 * tallest cell.
 *
 * @param[in,out] engine  Engine whose glyph pool grows.
 * @param[in]     font    Font metrics for advance measurement.
 * @param[in]     cell_x  Cell content left edge in pixels.
 * @param[in]     cell_w  Cell content width in pixels (not including padding).
 * @param[in]     top_y   Cell top baseline in pixels.
 * @param[in]     color   Packed ARGB glyph colour for all cell text.
 * @param[in]     tstart  First token index of the cell content (inclusive).
 * @param[in]     tend    One past the last token index (exclusive).
 *
 * @return Number of text lines the cell occupies (>= 1).
 * @retval 1 The cell fits on a single text line (the minimum).
 *
 * @pre `tstart <= tend` and `tend <= engine->token_count`.
 * @pre `cell_w > 0`.
 * @post Return value is >= 1 even for an empty cell (floor at 1 line).
 * @post All pushed glyphs have x in `[cell_x, cell_x + cell_w)`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_layout_cell(ra8_reflow_t*         engine,
                                 const stbtt_fontinfo* font,
                                 int32_t               cell_x,
                                 int32_t               cell_w,
                                 int32_t               top_y,
                                 uint32_t              color,
                                 uint32_t              tstart,
                                 uint32_t              tend)
{
  const int32_t  cell_right = cell_x + cell_w;
  const uint16_t font_px    = engine->font_px;
  int32_t        cx         = cell_x;
  int32_t        cy         = top_y;
  uint32_t       lines      = 1U;
  for (uint32_t t = tstart; t < tend; ++t) {
    const ra8_reflow_token_t* tok = &engine->tokens[t];
    if (tok->kind == (uint8_t)k_ra8_reflow_tok_text) {
      priv_cell_text(engine, font, tok, cell_x, cell_right, font_px, color, &cx, &cy, &lines);
    }
  }
  return lines;
}

/**
 * @brief Lay out every cell of one row at @p row_y; return the row's line count.
 *
 * @details Scans the token range `(tr_start, tr_end)` for `<td>` / `<th>`
 * block-starts. Each cell maps to a zero-based column index; the content area
 * starts at `margin + col * col_w + pad` and has width `col_w - 2 * pad`
 * (with `pad = k_priv_cell_pad_px`). Each cell is flowed by
 * @ref priv_layout_cell and the returned line count updates the per-row
 * maximum. Cells beyond the column count are placed but may overflow the
 * viewport (caller ensures the column count is non-zero before calling).
 *
 * @param[in,out] engine   Engine whose glyph pool grows.
 * @param[in]     font     Font metrics for advance measurement.
 * @param[in]     tr_start Index of the row's `<tr>` block-start token.
 * @param[in]     tr_end   Index of the row's `<tr>` block-end token.
 * @param[in]     col_w    Column width in pixels (total, before padding).
 * @param[in]     row_y    Row top baseline in pixels.
 *
 * @return Maximum cell line count in the row (>= 1).
 * @retval 1 Every cell in the row fits on a single text line (the minimum).
 *
 * @pre `tr_start < tr_end` and both are within `engine->token_count`.
 * @pre `col_w > 0`.
 * @post Return value >= 1 even for rows with no text content.
 * @post All pushed glyphs have x within `[margin, margin + cols * col_w)`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_row_cells(ra8_reflow_t*         engine,
                               const stbtt_fontinfo* font,
                               uint32_t              tr_start,
                               uint32_t              tr_end,
                               int32_t               col_w,
                               int32_t               row_y)
{
  const int32_t  pad   = (int32_t)k_priv_cell_pad_px;
  const uint32_t color = engine->body_color;
  uint32_t       rows  = 1U;
  uint32_t       col   = 0U;
  uint32_t       i     = tr_start + 1U;
  while (i < tr_end) {
    if (!priv_is_cell_start(engine, i)) {
      i++;
      continue;
    }
    const uint32_t cell_end = priv_match_block_end(engine, i, tr_end);
    const int32_t  cell_x   = (int32_t)k_ra8_reflow_margin_px + ((int32_t)col * col_w) + pad;
    const int32_t  cell_w   = col_w - (2 * pad);
    const uint32_t lines =
      priv_layout_cell(engine, font, cell_x, cell_w, row_y, color, i + 1U, cell_end);
    if (lines > rows) {
      rows = lines;
    }
    col++;
    i = cell_end + 1U;
  }
  return rows;
}

/**
 * @brief Lay out one table row, page-breaking before it if it would overflow.
 *
 * @details First lays the row tentatively at `cur->y` via @ref priv_row_cells,
 * computing `row_h = lines * line_h`. If the row's bottom `(cur->y + row_h)`
 * exceeds the page bottom margin and the cursor is not already at the top margin
 * (i.e. there is content above to preserve), the tentative glyphs are rolled
 * back by restoring `engine->glyph_count` to its value before the call, a page
 * flush is issued via @ref ra8_reflow_layout_finish_page, and the row is re-laid
 * from the top of the fresh page. The cursor is always advanced past the row by
 * `row_h + k_priv_row_gap_px` before returning.
 *
 * @param[in,out] engine    Engine whose glyph and page pools grow.
 * @param[in,out] cur       Layout cursor; `cur->y` is advanced past the row.
 * @param[in]     font      Font metrics for cell advance measurement.
 * @param[in]     tr_start  Index of the row's `<tr>` block-start token.
 * @param[in]     table_end Index of the enclosing table block-end (scan bound).
 * @param[in]     col_w     Column width in pixels.
 *
 * @return Token index just past the row's `<tr>` block-end (`tr_end + 1`).
 * @retval tr_end+1 The token index immediately after the row's `<tr>` block-end.
 *
 * @pre `tr_start < table_end` and both are within `engine->token_count`.
 * @pre `col_w > 0` and `cur->y >= 0`.
 * @post `cur->y` has advanced by `row_h + k_priv_row_gap_px` on return.
 * @post If a page break occurred, `engine->page_count` has increased by one.
 *
 * @note Not thread-safe; caller must serialize access to @p engine and @p cur.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_layout_row(ra8_reflow_t*         engine,
                                priv_cursor_t*        cur,
                                const stbtt_fontinfo* font,
                                uint32_t              tr_start,
                                uint32_t              table_end,
                                int32_t               col_w)
{
  const uint32_t tr_end       = priv_match_block_end(engine, tr_start, table_end);
  const int32_t  line_h       = (int32_t)ra8_reflow_layout_line_height(engine->font_px);
  const int32_t  bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra8_reflow_margin_px;

  const uint32_t glyphs_before = engine->glyph_count;
  uint32_t       row_lines     = priv_row_cells(engine, font, tr_start, tr_end, col_w, cur->y);
  int32_t        row_h         = (int32_t)row_lines * line_h;

  if (((cur->y + row_h) > bottom_limit) && (cur->y > (int32_t)k_ra8_reflow_margin_px)) {
    engine->glyph_count = glyphs_before; /* roll the row back ... */
    (void)ra8_reflow_layout_finish_page(engine,
                                        cur); /* ... flush the page (cur->y -> margin) ... */
    /* ... re-lay it */
    row_lines = priv_row_cells(engine, font, tr_start, tr_end, col_w, cur->y);
    row_h     = (int32_t)row_lines * line_h;
  }
  cur->y += row_h + (int32_t)k_priv_row_gap_px;
  return tr_end + 1U;
}

ra8_err_t ra8_reflow_layout_table(ra8_reflow_t*         engine,
                                  priv_cursor_t*        cur,
                                  const stbtt_fontinfo* font,
                                  uint32_t              start,
                                  uint32_t*             out_next)
{
  if (cur->line_has_content != 0U) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return k_ra8_err_no_mem;
    }
  }
  const uint32_t end  = priv_match_block_end(engine, start, engine->token_count);
  const uint32_t cols = priv_table_columns(engine, start, end);
  if (cols == 0U) {
    *out_next = (end < engine->token_count) ? (end + 1U) : engine->token_count;
    return k_ra8_ok;
  }
  const int32_t content_w = (int32_t)engine->viewport_w - (2 * (int32_t)k_ra8_reflow_margin_px);
  const int32_t col_w     = content_w / (int32_t)cols;

  uint32_t i = start + 1U;
  while (i < end) {
    if (priv_is_row_start(engine, i)) {
      i = priv_layout_row(engine, cur, font, i, end, col_w);
    } else {
      i++;
    }
  }

  /* Resume linear flow below the table. */
  cur->x                = (int32_t)k_ra8_reflow_margin_px + (int32_t)cur->indent_px;
  cur->align            = (uint8_t)k_ra8_reflow_align_left;
  cur->line_has_content = 0U;
  cur->y += (int32_t)k_ra8_reflow_paragraph_gap_px;
  cur->line_top         = cur->y;
  cur->line_first_glyph = engine->glyph_count;
  *out_next             = (end < engine->token_count) ? (end + 1U) : engine->token_count;
  return k_ra8_ok;
}
