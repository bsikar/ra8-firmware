/**
 * @file ra8_reflow_layout_internal.h
 * @brief Cross-TU shared declarations for the ra8_reflow layout engine.
 * @ingroup grp_ereader
 *
 * @details
 * The layout engine is split across three translation units so each stays
 * under the project file-size cap:
 *
 *   - `ra8_reflow_layout.c`        -- core line/page break + inline flow.
 *   - `ra8_reflow_layout_table.c`  -- `<table>` equal-column grid layout.
 *   - `ra8_reflow_layout_driver.c` -- the token-stream driver + public API.
 *
 * Any symbol referenced by more than one of those units lives here: the
 * mutable layout cursor type, the internal sizing constants, and the handful
 * of layout helpers that one unit calls across the file boundary. Symbols
 * used by a single unit stay private (`static`) in that unit.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_internal.h"
#include "stb_truetype.h"

/* ===========================================================================
 * Internal sizing constants (no magic numbers).
 * ===========================================================================
 */

/**
 * @enum priv_layout_consts_t
 * @brief Internal sizing knobs for the layout pass.
 */
typedef enum : uint16_t {
  k_priv_min_word_w_px        = 1U,  /**< Minimum word width before forcing a break.  */
  k_priv_hr_thickness_px      = 2U,  /**< Pixel thickness of an `<hr>` (placeholder). */
  k_priv_image_placeholder_px = 32U, /**< Side length of `<img>` placeholder.         */
  k_priv_min_chapter_pages    = 1U,  /**< Floor for non-empty input.                  */
  k_priv_cell_pad_px          = 6U,  /**< Left/right inset inside a table cell.       */
  k_priv_row_gap_px           = 4U,  /**< Vertical gap between table rows.            */
} priv_layout_consts_t;

/* ===========================================================================
 * Layout cursor
 * ===========================================================================
 */

/**
 * @struct priv_cursor_t
 * @brief Mutable state carried through the layout loop.
 */
typedef struct {
  int32_t  x;                /**< Pixel column for next glyph.                 */
  int32_t  y;                /**< Pixel row baseline for next glyph.           */
  int32_t  line_top;         /**< Pixel row of the current line's top.         */
  uint16_t line_height_px;   /**< Active line height in pixels.                */
  uint16_t active_font_px;   /**< Active font size (body or heading).          */
  uint8_t  active_style;     /**< Active inline-style stamp.                   */
  uint16_t indent_px;        /**< Active left indent (li, blockquote).         */
  uint32_t page_first_glyph; /**< First glyph index of the active page.        */
  uint32_t page_first_image; /**< First image-box index of the active page.    */
  uint32_t line_first_glyph; /**< First glyph index of the active line.        */
  uint8_t  line_has_content; /**< 1 once any glyph landed on this line.        */
  uint8_t  align;            /**< Active block alignment (ra8_reflow_align_t). */
  uint8_t  reserved8[2];     /**< Padding.                                     */
} priv_cursor_t;

/* ===========================================================================
 * Cross-TU layout helpers (defined in ra8_reflow_layout.c unless noted).
 * ===========================================================================
 */

/**
 * @brief Bounded zero-fill used in place of `memset(0)`.
 *
 * @details
 * Clang-tidy's `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
 * rule rejects `memset()`. We use a tiny private byte-walk so the
 * engine handle can be wiped without dragging the C runtime's
 * deprecated string API into the analyser.
 *
 * @param[in] dst See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void ra8_reflow_layout_byte_zero(uint8_t* dst, size_t n);

/**
 * @brief Initialise an `stbtt_fontinfo` from `engine->font_data`.
 *
 * @return `k_ra8_ok` if the blob parses, `k_ra8_err_validation_failed`
 *         on a malformed TTF.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] out_font See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_reflow_layout_init_font(const ra8_reflow_t* engine,
                                               stbtt_fontinfo*     out_font);

/**
 * @brief Compute the line height in pixels for a given font size.
 *
 * @details
 * `line_height = font_px * num / den` with the constants picked from
 * the public enum so the value is searchable.
 *
 * @param[in] font_px See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV uint16_t ra8_reflow_layout_line_height(uint16_t font_px);

/**
 * @brief Measure a single ASCII code point's advance width in pixels.
 *
 * @details
 * Uses `stbtt_GetCodepointHMetrics()` (advance is in font-units; we
 * scale by `stbtt_ScaleForPixelHeight`).
 *
 * @param[in] font See implementation.
 * @param[in] font_px See implementation.
 * @param[in] cp See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_reflow_layout_glyph_advance(const stbtt_fontinfo* font,
                                                 uint16_t              font_px,
                                                 int32_t               cp);

/**
 * @brief Push one positioned glyph into the engine glyph pool.
 *
 * @details Appends a new @ref ra8_reflow_glyph_t entry at
 * `engine->glyphs[engine->glyph_count]`, populates all fields from the
 * supplied arguments, and increments `engine->glyph_count`. The @p link_id
 * byte is stored in the `reserved` field of the glyph so that the
 * post-layout link-rect pass can identify link runs. Returns @c false
 * immediately -- without mutating state -- if the pool is already at
 * capacity (`engine->glyph_count >= k_ra8_reflow_max_glyphs`).
 *
 * @param[in,out] engine  Engine whose glyph pool grows by one entry.
 * @param[in]     x       Horizontal pen position in pixels.
 * @param[in]     y       Baseline row in pixels.
 * @param[in]     cp      Unicode code point (ASCII range in practice).
 * @param[in]     font_px Active font size in pixels.
 * @param[in]     style   Inline style stamp (bold/italic/underline + face id).
 * @param[in]     color   Packed ARGB glyph colour.
 * @param[in]     link_id 1-based link identifier; 0 means not a link.
 *
 * @return Boolean success flag.
 * @retval true  Glyph appended; `engine->glyph_count` incremented.
 * @retval false Pool full; engine state is unchanged.
 *
 * @pre `engine != nullptr`.
 * @pre `engine->glyph_count <= k_ra8_reflow_max_glyphs`.
 * @post On true, `engine->glyphs[engine->glyph_count - 1]` holds the new glyph.
 * @post On false, no state mutated.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_layout_push_glyph(ra8_reflow_t* engine,
                                           int32_t       x,
                                           int32_t       y,
                                           int32_t       cp,
                                           uint16_t      font_px,
                                           uint8_t       style,
                                           uint32_t      color,
                                           uint8_t       link_id);

/**
 * @brief Finalise the current page and start a new one.
 *
 * @return false if the page pool overflowed, true otherwise.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_layout_finish_page(ra8_reflow_t* engine, priv_cursor_t* cur);

/**
 * @brief Wrap to a new line, finishing a page if the bottom margin is hit.
 *
 * @details Calls `priv_finish_line` to apply alignment to the just-completed
 * line, then advances `cur->y` by `cur->line_height_px`, resets `cur->x` to
 * the left margin plus the active indent, clears `cur->line_has_content`, and
 * updates `cur->line_first_glyph`. If the new baseline plus one line height
 * would exceed the bottom margin it calls @ref ra8_reflow_layout_finish_page to
 * flush the current page and reset the cursor to the top of a fresh page.
 *
 * @param[in,out] engine        Engine whose page pool may grow by one entry.
 * @param[in,out] cur           Layout cursor; x, y, line state are updated.
 * @param[in]     allow_justify Passed to `priv_finish_line`; true on a wrapped
 *                              line, false at an explicit paragraph end.
 *
 * @return Boolean success flag.
 * @retval true  Line advanced; page flushed if needed.
 * @retval false Page pool overflowed; no further layout should proceed.
 *
 * @pre `engine != nullptr` and `cur != nullptr`.
 * @pre `cur->line_height_px > 0`.
 * @post `cur->line_has_content == 0` on true return.
 * @post On false, `engine->page_count == k_ra8_reflow_max_pages`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_PRIV bool
ra8_reflow_layout_newline(ra8_reflow_t* engine, priv_cursor_t* cur, bool allow_justify);

/**
 * @brief Dispatch one parsed token through the layout cursor.
 *
 * @details
 * Splits the per-token switch out of the layout driver to keep the outer
 * loop under clang-tidy's cognitive-complexity threshold (Rule 4 /
 * 25-statement budget).
 *
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] font See implementation.
 * @param[in] tok See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_reflow_layout_apply_token(ra8_reflow_t*             engine,
                                                 priv_cursor_t*            cur,
                                                 const stbtt_fontinfo*     font,
                                                 const ra8_reflow_token_t* tok);

/**
 * @brief Post-layout pass: group link-tagged glyphs into tappable rects.
 *
 * @details Iterates over every page in `engine->pages[]`. For each page,
 * walks the glyph run in order; when a glyph carries a non-zero `reserved`
 * (link id) it scans forward to find the end of the consecutive same-link,
 * same-baseline-y run, then emits a link rect for that span. A multi-line
 * anchor link therefore produces one rect per wrapped line. Glyphs with
 * `reserved == 0` are skipped.
 *
 * @param[in,out] engine Engine holding laid-out glyphs and pages; its
 *                       link-rect pool grows.
 * @param[in]     font   Font metrics for last-glyph advance measurement.
 *
 * @return Nothing.
 *
 * @pre `engine->pages[]` and `engine->glyphs[]` are fully populated by the
 *      layout pass.
 * @pre `engine->link_rect_count == 0` on entry (reset by `ra8_reflow_run_layout`).
 * @post `engine->link_rects[]` contains page-local tappable rectangles for
 *       every link run found.
 * @post `engine->link_rect_count` reflects the total number of rects emitted.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_PRIV void ra8_reflow_layout_build_link_rects(ra8_reflow_t* engine, const stbtt_fontinfo* font);

/**
 * @brief Apply an `<img>` token: real image when a loader is bound, else a
 *        placeholder.
 *
 * @details Defined in `ra8_reflow_layout_image.c`. When the engine has a bound
 * image loader + arena and the token carries a source slice, the image is sized
 * to the text column, page-broken as a block, and recorded as an image box; any
 * failure (full pool, unresolved src, flush overflow) falls back to the historic
 * fixed-size placeholder advance so image-free content stays byte-identical.
 *
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @return Boolean.
 * @retval true Token applied (image placed or placeholder reserved).
 * @retval false Pool overflow propagated from a sub-step.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_layout_apply_image(ra8_reflow_t*             engine,
                                            priv_cursor_t*            cur,
                                            const ra8_reflow_token_t* tok);

/**
 * @brief Lay out a `<table>` token range as an equal-column grid.
 *
 * @details Flushes the pending line (if any) via @ref ra8_reflow_layout_newline,
 * locates the matching table block-end, and counts the maximum column count.
 * If the table has no cells the function advances @p out_next past the
 * block-end and returns immediately. Otherwise the content width is divided
 * equally by column count to derive `col_w`, then each `<tr>` block-start in
 * the range is processed (which handles row-level page breaks). After all
 * rows are placed the cursor is repositioned to the left margin and a
 * paragraph gap is added so linear text flow resumes cleanly below the table.
 *
 * @param[in,out] engine   Engine whose glyph and page pools grow.
 * @param[in,out] cur      Layout cursor; x, y and line state are updated.
 * @param[in]     font     Font metrics for row and cell layout.
 * @param[in]     start    Token index of the `<table>` block-start.
 * @param[out]    out_next Receives the token index just past the table block-end.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok       Table laid out; @p out_next set.
 * @retval k_ra8_err_no_mem Pending line flush overflowed the page pool.
 *
 * @pre `engine != nullptr`, `cur != nullptr`, `font != nullptr`,
 *      `out_next != nullptr`.
 * @pre `start < engine->token_count` and `engine->tokens[start]` is a
 *      `<table>` block-start.
 * @post On `k_ra8_ok`, `*out_next > start` and points past the table block-end.
 * @post On `k_ra8_ok`, `cur->align == k_ra8_reflow_align_left` and
 *       `cur->line_has_content == 0`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine and @p cur.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_reflow_layout_table(ra8_reflow_t*         engine,
                                           priv_cursor_t*        cur,
                                           const stbtt_fontinfo* font,
                                           uint32_t              start,
                                           uint32_t*             out_next);
