/**
 * @file ra_reflow_layout.c
 * @brief Greedy line-break + page-break engine for ra_reflow.
 *
 * @details
 * Consumes the token stream produced by `ra_reflow_parse_xhtml()` and
 * lays it out as a flat list of positioned glyphs grouped into pages.
 * The algorithm is a textbook greedy break-on-overflow:
 *
 *   1. Walk tokens in order.
 *   2. For a `text` token, split the run into words (`[A-Z]+` runs
 *      separated by spaces). Measure each word with stb_truetype's
 *      advance widths.
 *   3. If the cursor would cross the right margin, finalise the line.
 *   4. If the cursor would cross the bottom margin, finalise the page.
 *
 * Block-level tokens (`<p>`, `<h1>` ..) flush the current line, add
 * a paragraph gap, and (for headings) bump the active font size.
 *
 * No floating-point allocations and no recursion -- the recursion
 * already happened during the parse pass; layout is a flat loop.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_internal.h"
#include "stb_truetype.h"

/**
 * @brief Return true iff @p tag is a block-level indent tag.
 *
 * @details Pure helper factored out of @c priv_open_block (line 479)
 *          and @c priv_close_block (line 513) so the
 *          ``tag == li || tag == blockquote`` decision can be driven
 *          directly by host MC/DC tests via
 *          @ref ra_reflow_internal_is_indent_tag.
 *
 * @param[in] tag Token tag value (raw @c uint8_t storage of
 *                @ref ra_reflow_html_tag_t).
 *
 * @return Boolean indent-tag predicate.
 * @retval true  Tag is @c k_ra_reflow_tag_li or @c k_ra_reflow_tag_blockquote.
 * @retval false Otherwise.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on @p tag.
 *
 * @note Pure function; thread-safe.
 *
 * @since 0.1.0
 */
bool ra_reflow_internal_is_indent_tag(uint8_t tag)
{
  return (tag == (uint8_t)k_ra_reflow_tag_li) || (tag == (uint8_t)k_ra_reflow_tag_blockquote);
}

/**
 * @brief AND helper for the right-margin overflow break decision.
 * @details Promoted from inline expressions at original lines 404,
 *          468 and 605 so MC/DC tests can drive both arms of the
 *          ``(cur->x + advance > right_limit) && line_has_content``
 *          decision directly.
 * @param[in] cursor_x Pen x position in pixels.
 * @param[in] advance Width about to be emitted in pixels.
 * @param[in] right_limit Right edge in pixels.
 * @param[in] line_has_content Non-zero iff the line already has glyphs.
 * @return Boolean break-needed predicate.
 * @retval true Caller must call priv_newline before emitting.
 * @retval false Emitting in place is safe.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the four arguments.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
bool ra_reflow_internal_right_overflow_break(int32_t cursor_x,
                                             int32_t advance,
                                             int32_t right_limit,
                                             uint8_t line_has_content)
{
  return (cursor_x + advance > right_limit) && (line_has_content != 0U);
}

/**
 * @brief OR helper for the cached-XHTML invalid decision.
 * @details Promoted from line 953 in @c ra_reflow_set_font_size.
 * @param[in] xhtml_buf Cached buffer pointer (may be NULL).
 * @param[in] xhtml_len Cached buffer length (may be zero).
 * @return Boolean invalid-buffer predicate.
 * @retval true Buffer is unusable.
 * @retval false Buffer is usable.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
bool ra_reflow_internal_xhtml_invalid(const void* xhtml_buf, uint32_t xhtml_len)
{
  return (xhtml_buf == nullptr) || (xhtml_len == 0U);
}

/**
 * @brief AND helper for the synthesise-final-page decision.
 * @details Promoted from line 750 in @c ra_reflow_run_layout.
 * @param[in] page_count Number of pages flushed during the pass.
 * @param[in] token_count Total parsed-token count.
 * @return Boolean fixup-needed predicate.
 * @retval true Caller must synthesise a single final page.
 * @retval false No fixup required.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
bool ra_reflow_internal_final_page_needed(uint32_t page_count, uint32_t token_count)
{
  return (page_count == 0U) && (token_count > 0U);
}

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
static void priv_byte_zero(uint8_t* dst, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    dst[i] = 0U;
  }
}

/* ===========================================================================
 * Internal sizing constants (no magic numbers).
 * ===========================================================================
 */

/**
 * @enum priv_layout_consts_t
 * @brief Internal sizing knobs for the layout pass.
 */
typedef enum : uint16_t {
  k_priv_min_word_w_px        = 1U,  /**< Minimum word width before forcing a break. */
  k_priv_hr_thickness_px      = 2U,  /**< Pixel thickness of an `<hr>` (placeholder).*/
  k_priv_image_placeholder_px = 32U, /**< Side length of `<img>` placeholder.     */
  k_priv_min_chapter_pages    = 1U,  /**< Floor for non-empty input.                */
  k_priv_cell_pad_px          = 6U,  /**< Left/right inset inside a table cell.    */
  k_priv_row_gap_px           = 4U,  /**< Vertical gap between table rows.         */
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
  int32_t  x;                /**< Pixel column for next glyph. */
  int32_t  y;                /**< Pixel row baseline for next glyph. */
  int32_t  line_top;         /**< Pixel row of the current line's top. */
  uint16_t line_height_px;   /**< Active line height in pixels. */
  uint16_t active_font_px;   /**< Active font size (body or heading). */
  uint8_t  active_style;     /**< Active inline-style stamp. */
  uint16_t indent_px;        /**< Active left indent (li, blockquote). */
  uint32_t page_first_glyph; /**< First glyph index of the active page. */
  uint32_t page_first_image; /**< First image-box index of the active page. */
  uint32_t line_first_glyph; /**< First glyph index of the active line. */
  uint8_t  line_has_content; /**< 1 once any glyph landed on this line. */
  uint8_t  align;            /**< Active block alignment (ra_reflow_align_t). */
  uint8_t  reserved8[2];     /**< Padding. */
} priv_cursor_t;

/* ===========================================================================
 * Helpers
 * ===========================================================================
 */

/**
 * @brief Initialise an `stbtt_fontinfo` from `engine->font_data`.
 *
 * @return `k_ra_ok` if the blob parses, `k_ra_err_validation_failed`
 *         on a malformed TTF.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] out_font See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_init_font(const ra_reflow_t* engine, stbtt_fontinfo* out_font)
{
  const int32_t offset = stbtt_GetFontOffsetForIndex(engine->font_data, 0);
  if (offset < 0) {
    return k_ra_err_validation_failed;
  }
  if (stbtt_InitFont(out_font, engine->font_data, offset) == 0) {
    return k_ra_err_validation_failed;
  }
  return k_ra_ok;
}

/**
 * @brief Compute the line height in pixels for a given font size.
 *
 * @details
 * `line_height = font_px * num / den` with the constants picked from
 * the public enum so the value is searchable.
 *
 * @param[in] font_px See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t priv_line_height(uint16_t font_px)
{
  const uint32_t lh = ((uint32_t)font_px * (uint32_t)k_ra_reflow_line_spacing_num) /
                      (uint32_t)k_ra_reflow_line_spacing_den;
  return (uint16_t)lh;
}

/**
 * @brief Pick the font size for a given block tag.
 *
 * @return Pixel size for headings, or the body size otherwise.
 *
 * @details See implementation.
 * @param[in] body_px See implementation.
 * @param[in] tag See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t priv_block_font_px(uint16_t body_px, ra_reflow_html_tag_t tag)
{
  uint32_t pct = k_ra_reflow_pct_full;
  switch (tag) {
    case k_ra_reflow_tag_h1:
      pct = k_ra_reflow_h1_scale_pct;
      break;
    case k_ra_reflow_tag_h2:
      pct = k_ra_reflow_h2_scale_pct;
      break;
    case k_ra_reflow_tag_h3:
      pct = k_ra_reflow_h3_scale_pct;
      break;
    case k_ra_reflow_tag_h4:
      pct = k_ra_reflow_h4_scale_pct;
      break;
    case k_ra_reflow_tag_h5:
      pct = k_ra_reflow_h5_scale_pct;
      break;
    case k_ra_reflow_tag_h6:
      pct = k_ra_reflow_h6_scale_pct;
      break;
    default:
      pct = k_ra_reflow_pct_full;
      break;
  }
  return (uint16_t)(((uint32_t)body_px * pct) / k_ra_reflow_pct_full);
}

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
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static int32_t priv_glyph_advance(const stbtt_fontinfo* font, uint16_t font_px, int32_t cp)
{
  const float scale         = stbtt_ScaleForPixelHeight(font, (float)font_px);
  int         advance_units = 0;
  int         lsb           = 0;
  stbtt_GetCodepointHMetrics(font, cp, &advance_units, &lsb);
  return (int32_t)((float)advance_units * scale);
}

/**
 * @brief Push one positioned glyph into the engine glyph pool.
 *
 * @details Appends a new @ref ra_reflow_glyph_t entry at
 * `engine->glyphs[engine->glyph_count]`, populates all fields from the
 * supplied arguments, and increments `engine->glyph_count`. The @p link_id
 * byte is stored in the `reserved` field of the glyph so that the
 * post-layout link-rect pass can identify link runs. Returns @c false
 * immediately -- without mutating state -- if the pool is already at
 * capacity (`engine->glyph_count >= k_ra_reflow_max_glyphs`).
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
 * @pre `engine->glyph_count <= k_ra_reflow_max_glyphs`.
 * @post On true, `engine->glyphs[engine->glyph_count - 1]` holds the new glyph.
 * @post On false, no state mutated.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static bool priv_push_glyph(ra_reflow_t* engine,
                            int32_t      x,
                            int32_t      y,
                            int32_t      cp,
                            uint16_t     font_px,
                            uint8_t      style,
                            uint32_t     color,
                            uint8_t      link_id)
{
  if (engine->glyph_count >= k_ra_reflow_max_glyphs) {
    return false;
  }
  ra_reflow_glyph_t* g = &engine->glyphs[engine->glyph_count];
  g->x                 = x;
  g->y                 = y;
  g->cp                = cp;
  g->color             = color;
  g->font_px           = font_px;
  g->style             = style;
  g->reserved          = link_id; /* 1-based `<a>` link id (0 = not a link) */
  engine->glyph_count++;
  return true;
}

/**
 * @brief Finalise the current page and start a new one.
 *
 * @return false if the page pool overflowed, true otherwise.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_finish_page(ra_reflow_t* engine, priv_cursor_t* cur)
{
  if (engine->page_count >= k_ra_reflow_max_pages) {
    return false;
  }
  const uint32_t    count = engine->glyph_count - cur->page_first_glyph;
  ra_reflow_page_t* page  = &engine->pages[engine->page_count];
  page->glyph_first       = cur->page_first_glyph;
  page->glyph_count       = count;
  engine->page_count++;
  cur->page_first_glyph = engine->glyph_count;
  cur->page_first_image = engine->image_box_count;
  cur->line_first_glyph = engine->glyph_count;
  cur->y                = (int32_t)k_ra_reflow_margin_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
  return true;
}

/**
 * @brief Spread @p slack across the inter-word gaps of glyphs `[lo, hi)`.
 *
 * @details Each space glyph absorbs an equal share of the slack (with the
 * remainder spread one pixel at a time across the leftmost gaps); every glyph
 * shifts right by the accumulated widening to its left, so the run's right edge
 * lands at the margin with an even gap distribution. If the run contains no
 * space glyphs the function returns early without modifying anything.
 *
 * @param[in,out] engine Engine whose glyph x positions are adjusted.
 * @param[in]     lo     First glyph index (inclusive).
 * @param[in]     hi     One past the last glyph index.
 * @param[in]     slack  Total pixels to distribute (> 0).
 *
 * @return Nothing.
 *
 * @pre `hi > lo`; the run contains at least one space to justify against.
 * @pre `slack > 0`.
 * @post Glyph x positions in `[lo, hi)` are widened by up to @p slack total.
 * @post Glyphs with no space neighbour are shifted right by the cumulative delta.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static void priv_justify_glyphs(ra_reflow_t* engine, uint32_t lo, uint32_t hi, int32_t slack)
{
  uint32_t spaces = 0U;
  for (uint32_t i = lo; i < hi; ++i) {
    if (engine->glyphs[i].cp == (int32_t)' ') {
      spaces++;
    }
  }
  if (spaces == 0U) {
    return;
  }
  const int32_t per   = slack / (int32_t)spaces;
  const int32_t rem   = slack % (int32_t)spaces;
  int32_t       added = 0;
  uint32_t      seen  = 0U;
  for (uint32_t i = lo; i < hi; ++i) {
    engine->glyphs[i].x += added;
    if (engine->glyphs[i].cp == (int32_t)' ') {
      added += per;
      if ((int32_t)seen < rem) {
        added += 1;
      }
      seen++;
    }
  }
}

/**
 * @brief Apply the active block alignment to the just-completed line.
 *
 * @details Left alignment is a no-op (the default). Centre and right shift every
 * glyph in the range `[cur->line_first_glyph, engine->glyph_count)` so the
 * content edge meets the centre or right margin. Justify distributes the slack
 * across inter-word gaps via @ref priv_justify_glyphs -- but only when
 * @p allow_justify is true (wrapped lines); the last line of a paragraph keeps
 * its left alignment. A trailing space at the break point is excluded from the
 * content extent before computing the slack so justification does not over-expand.
 *
 * @param[in,out] engine        Engine whose laid-out glyphs are aligned.
 * @param[in]     cur           Cursor (line range + alignment + pen x).
 * @param[in]     allow_justify True on a wrapped line, false on a paragraph end.
 *
 * @return Nothing.
 *
 * @pre `cur->line_first_glyph <= engine->glyph_count`.
 * @pre `cur->align` is a valid `ra_reflow_align_t` value.
 * @post Glyph x positions in the line range are adjusted per the alignment.
 * @post For justify mode with no space glyphs, positions are left unchanged.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static void priv_finish_line(ra_reflow_t* engine, priv_cursor_t* cur, bool allow_justify)
{
  if (cur->align == (uint8_t)k_ra_reflow_align_left) {
    return;
  }
  const uint32_t lo = cur->line_first_glyph;
  uint32_t       hi = engine->glyph_count;
  if (hi <= lo) {
    return;
  }
  int32_t content_right = cur->x;
  if (engine->glyphs[hi - 1U].cp == (int32_t)' ') {
    content_right = engine->glyphs[hi - 1U].x;
    hi--;
  }
  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
  const int32_t slack       = right_limit - content_right;
  if ((hi <= lo) || (slack <= 0)) {
    return;
  }
  if (cur->align == (uint8_t)k_ra_reflow_align_justify) {
    if (allow_justify) {
      priv_justify_glyphs(engine, lo, hi, slack);
    }
    return;
  }
  const int32_t offset = (cur->align == (uint8_t)k_ra_reflow_align_center) ? (slack / 2) : slack;
  for (uint32_t i = lo; i < hi; ++i) {
    engine->glyphs[i].x += offset;
  }
}

/**
 * @brief Wrap to a new line, finishing a page if the bottom margin is hit.
 *
 * @details Calls @ref priv_finish_line to apply alignment to the just-completed
 * line, then advances `cur->y` by `cur->line_height_px`, resets `cur->x` to
 * the left margin plus the active indent, clears `cur->line_has_content`, and
 * updates `cur->line_first_glyph`. If the new baseline plus one line height
 * would exceed the bottom margin it calls @ref priv_finish_page to flush the
 * current page and reset the cursor to the top of a fresh page.
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
 * @post On false, `engine->page_count == k_ra_reflow_max_pages`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static bool priv_newline(ra_reflow_t* engine, priv_cursor_t* cur, bool allow_justify)
{
  priv_finish_line(engine, cur, allow_justify);
  cur->y += (int32_t)cur->line_height_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
  cur->line_first_glyph = engine->glyph_count;

  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra_reflow_margin_px;
  if (cur->y + (int32_t)cur->line_height_px > bottom_limit) {
    if (!priv_finish_page(engine, cur)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Append one ASCII code point at the current cursor, wrapping
 *        if it would overflow the right margin.
 *
 * @details Measures the advance width of @p cp via @ref priv_glyph_advance,
 * clamps it to at least `k_priv_min_word_w_px` to prevent zero-width stalls,
 * and checks the right-overflow predicate
 * (@ref ra_reflow_internal_right_overflow_break). If a line break is needed
 * and the line already has content, @ref priv_newline is called before the
 * glyph is pushed. The glyph is then appended via @ref priv_push_glyph and
 * `cur->x` is advanced by the clamped advance. `cur->line_has_content` is set
 * to 1 after the first glyph lands.
 *
 * @param[in,out] engine  Engine whose glyph pool grows.
 * @param[in,out] cur     Layout cursor; x and line state are updated.
 * @param[in]     font    Font metrics for advance measurement.
 * @param[in]     cp      Unicode code point to emit (ASCII range in practice).
 * @param[in]     color   Packed ARGB glyph colour.
 * @param[in]     link_id 1-based link identifier; 0 means not a link.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok       Glyph emitted successfully.
 * @retval k_ra_err_no_mem Page pool or glyph pool overflowed.
 *
 * @pre `engine != nullptr`, `cur != nullptr`, `font != nullptr`.
 * @pre `cur->active_font_px > 0`.
 * @post On `k_ra_ok`, `engine->glyph_count` has increased by one.
 * @post On `k_ra_ok`, `cur->line_has_content == 1`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static ra_err_t priv_emit_char(ra_reflow_t*          engine,
                               priv_cursor_t*        cur,
                               const stbtt_fontinfo* font,
                               int32_t               cp,
                               uint32_t              color,
                               uint8_t               link_id)
{
  const int32_t advance = priv_glyph_advance(font, cur->active_font_px, cp);
  const int32_t advance_clamped =
    (advance < (int32_t)k_priv_min_word_w_px) ? (int32_t)k_priv_min_word_w_px : advance;

  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
  if (ra_reflow_internal_right_overflow_break(cur->x,
                                              advance_clamped,
                                              right_limit,
                                              cur->line_has_content)) {
    if (!priv_newline(engine, cur, true)) { /* wrap -> previous line may justify */
      return k_ra_err_no_mem;
    }
  }
  if (!priv_push_glyph(engine,
                       cur->x,
                       cur->y,
                       cp,
                       cur->active_font_px,
                       cur->active_style,
                       color,
                       link_id)) {
    return k_ra_err_no_mem;
  }
  cur->x += advance_clamped;
  cur->line_has_content = 1U;
  return k_ra_ok;
}

/**
 * @brief Lay out one text token: walk byte-by-byte, breaking at
 *        whitespace, and emit each character through `priv_emit_char`.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] font See implementation.
 * @param[in] tok See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_layout_text(ra_reflow_t*             engine,
                                 priv_cursor_t*           cur,
                                 const stbtt_fontinfo*    font,
                                 const ra_reflow_token_t* tok)
{
  const uint8_t* base    = engine->text_pool + tok->text_off;
  const uint32_t len     = tok->text_len;
  const uint8_t  link_id = tok->reserved; /* 1-based `<a>` link id (0 = none) */
  /* Colour precedence: an explicit CSS `color` (#140) wins; otherwise a link run
   * uses the link colour and ordinary text the body colour. Link colour is keyed
   * on actual link membership, not the underline style bit, so CSS
   * `text-decoration: underline` (#111) on non-link text keeps the body colour. */
  const uint32_t fallback = (link_id != 0U) ? engine->link_color : engine->body_color;
  const uint32_t color =
    (tok->color != (uint32_t)k_ra_reflow_color_inherit) ? tok->color : fallback;

  /* Pre-scan to find each word boundary. Greedy: if word_w + cursor_x
   * exceeds the right margin AND the line already has content, break
   * before emitting the word. */
  uint32_t i = 0U;
  while (i < len) {
    /* Emit any leading whitespace as one space (already collapsed by
     * the parser). */
    if (base[i] == ' ') {
      ra_err_t err = priv_emit_char(engine, cur, font, ' ', color, link_id);
      if (err != k_ra_ok) {
        return err;
      }
      ++i;
      continue;
    }

    /* Word: bytes up to the next space. Pre-measure for greedy break. */
    uint32_t word_end = i;
    int32_t  word_w   = 0;
    while (word_end < len && base[word_end] != ' ') {
      word_w += priv_glyph_advance(font, cur->active_font_px, (int32_t)base[word_end]);
      ++word_end;
    }
    const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
    if (ra_reflow_internal_right_overflow_break(cur->x,
                                                word_w,
                                                right_limit,
                                                cur->line_has_content)) {
      if (!priv_newline(engine, cur, true)) { /* wrap -> previous line may justify */
        return k_ra_err_no_mem;
      }
    }
    while (i < word_end) {
      ra_err_t err = priv_emit_char(engine, cur, font, (int32_t)base[i], color, link_id);
      if (err != k_ra_ok) {
        return err;
      }
      ++i;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Apply a `block_start` token: flush current line, set heading
 *        font size, set indent.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_open_block(ra_reflow_t* engine, priv_cursor_t* cur, const ra_reflow_token_t* tok)
{
  if (cur->line_has_content != 0U) {
    if (!priv_newline(engine, cur, false)) {
      return false;
    }
  }
  /* The block's text alignment travels in the block-start token's reserved byte
   * (set by the tokenizer from `style="text-align:..."`). */
  cur->align = tok->reserved;
  /* Record an `id=` anchor (carried in the block-start token's slice) at this
   * block's page + top, for same-chapter `#fragment` jumps. */
  if ((tok->text_len > 0U) && (engine->anchor_count < (uint32_t)k_ra_reflow_max_anchors)) {
    ra_reflow_anchor_t* anchor = &engine->anchors[engine->anchor_count];
    anchor->id_off             = tok->text_off;
    anchor->id_len             = tok->text_len;
    anchor->page_index         = engine->page_count;
    anchor->y                  = cur->y;
    engine->anchor_count++;
  }
  /* CSS `font-size` (#140) on the block-start token wins; else the UA default
   * (body size or heading scale). 0 = no CSS font, so unstyled content is
   * byte-identical. */
  cur->active_font_px = (tok->css_font_px != 0U)
                          ? tok->css_font_px
                          : priv_block_font_px(engine->font_px, (ra_reflow_html_tag_t)tok->tag);
  cur->line_height_px = priv_line_height(cur->active_font_px);
  if (ra_reflow_internal_is_indent_tag((uint8_t)tok->tag)) {
    cur->indent_px = k_ra_reflow_indent_px;
    cur->x         = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  }
  return true;
}

/**
 * @brief Apply a `block_end` token: flush line + add paragraph gap.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_close_block(ra_reflow_t* engine, priv_cursor_t* cur, const ra_reflow_token_t* tok)
{
  if (cur->line_has_content != 0U) {
    if (!priv_newline(engine, cur, false)) {
      return false;
    }
  }
  /* Paragraph gap. */
  cur->y += (int32_t)k_ra_reflow_paragraph_gap_px;
  cur->line_top = cur->y;
  cur->x        = (int32_t)k_ra_reflow_margin_px;
  if (ra_reflow_internal_is_indent_tag((uint8_t)tok->tag)) {
    cur->indent_px = 0U;
  }
  cur->active_font_px = engine->font_px;
  cur->line_height_px = priv_line_height(cur->active_font_px);

  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra_reflow_margin_px;
  if (cur->y + (int32_t)cur->line_height_px > bottom_limit) {
    if (!priv_finish_page(engine, cur)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Apply a `<hr>` rule token: flush the line and bump the cursor.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_apply_rule(ra_reflow_t* engine, priv_cursor_t* cur)
{
  if (cur->line_has_content != 0U) {
    if (!priv_newline(engine, cur, false)) {
      return false;
    }
  }
  cur->y += (int32_t)k_priv_hr_thickness_px;
  cur->line_top = cur->y;
  return true;
}

/**
 * @brief Apply an `<img>` token without a bound loader: placeholder advance.
 *
 * @details See implementation. Historical v1 behaviour, kept so image-free
 * content (and content laid out before a loader is bound) is byte-identical.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_apply_image_placeholder(ra_reflow_t* engine, priv_cursor_t* cur)
{
  const int32_t advance     = (int32_t)k_priv_image_placeholder_px;
  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
  if (ra_reflow_internal_right_overflow_break(cur->x,
                                              advance,
                                              right_limit,
                                              cur->line_has_content)) {
    if (!priv_newline(engine, cur, false)) {
      return false;
    }
  }
  cur->x += advance;
  cur->line_has_content = 1U;
  return true;
}

/**
 * @brief True iff the page under construction already holds glyphs or images.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @return Boolean.
 * @retval true Page has content.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post No state mutated.
 * @post No state mutated.
 * @note Pure read.
 * @since 0.1.0
 */
static bool priv_page_has_content(const ra_reflow_t* engine, const priv_cursor_t* cur)
{
  return (engine->glyph_count > cur->page_first_glyph) ||
         (engine->image_box_count > cur->page_first_image);
}

/**
 * @brief Scale an intrinsic image size to fit the text column (no upscaling).
 *
 * @details See implementation. Caps width at the column, height at one page;
 * preserves aspect ratio with int64 products to avoid overflow.
 * @param[in] iw See implementation.
 * @param[in] ih See implementation.
 * @param[in] col_w See implementation.
 * @param[in] avail_h See implementation.
 * @param[out] out_w See implementation.
 * @param[out] out_h See implementation.
 * @return None.
 * @pre `iw > 0` and `ih > 0`.
 * @pre `col_w > 0` and `avail_h > 0`.
 * @post `*out_w` in [1, col_w] and `*out_h` in [1, avail_h].
 * @post Aspect ratio preserved within integer rounding.
 * @note Pure function.
 * @since 0.1.0
 */
static void priv_image_fit(int32_t  iw,
                           int32_t  ih,
                           int32_t  col_w,
                           int32_t  avail_h,
                           int32_t* out_w,
                           int32_t* out_h)
{
  int32_t bw = (iw < col_w) ? iw : col_w;
  int32_t bh = (int32_t)(((int64_t)ih * (int64_t)bw) / (int64_t)iw);
  if (bh > avail_h) {
    bh = avail_h;
    bw = (int32_t)(((int64_t)iw * (int64_t)bh) / (int64_t)ih);
  }
  *out_w = (bw < 1) ? 1 : bw;
  *out_h = (bh < 1) ? 1 : bh;
}

/**
 * @brief Resolve an image token to a column-fitted box size via the loader.
 *
 * @details See implementation. Calls the bound loader for the encoded bytes,
 * probes the intrinsic size (zero-alloc), and fits it to the column.
 * @param[in] engine See implementation.
 * @param[in] tok See implementation.
 * @param[out] out_w See implementation.
 * @param[out] out_h See implementation.
 * @return Boolean.
 * @retval true Box size resolved.
 * @retval false Loader failed, probe failed, or viewport too small.
 * @pre `engine->img_loader != nullptr`.
 * @pre `tok->text_len > 0`.
 * @post On true, `*out_w`/`*out_h` are a valid column-fit box.
 * @post On false, no box is produced (caller falls back).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_image_resolve_size(ra_reflow_t*             engine,
                                    const ra_reflow_token_t* tok,
                                    int32_t*                 out_w,
                                    int32_t*                 out_h)
{
  const char*    href  = (const char*)&engine->text_pool[tok->text_off];
  const uint8_t* bytes = nullptr;
  size_t         blen  = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, tok->text_len, &bytes, &blen) != k_ra_ok) {
    return false;
  }
  int32_t iw = 0;
  int32_t ih = 0;
  if ((ra_img_probe_size(bytes, blen, &iw, &ih) != k_ra_ok) || (iw <= 0) || (ih <= 0)) {
    return false;
  }
  const int32_t col_w   = (int32_t)engine->viewport_w - (2 * (int32_t)k_ra_reflow_margin_px);
  const int32_t avail_h = (int32_t)engine->viewport_h - (2 * (int32_t)k_ra_reflow_margin_px);
  if ((col_w < 1) || (avail_h < 1)) {
    return false;
  }
  priv_image_fit(iw, ih, col_w, avail_h, out_w, out_h);
  return true;
}

/**
 * @brief Record a laid-out image box and advance the cursor below it.
 *
 * @details See implementation. Stores the box at the left margin / current
 * baseline tagged with the active page, then drops the cursor past it.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @param[in] bw See implementation.
 * @param[in] bh See implementation.
 * @return None.
 * @pre `engine->image_box_count < k_ra_reflow_max_images`.
 * @pre `bw >= 1` and `bh >= 1`.
 * @post One image box appended; `image_box_count` incremented.
 * @post Cursor advanced below the image, line reset to the left margin.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_image_record(ra_reflow_t*             engine,
                              priv_cursor_t*           cur,
                              const ra_reflow_token_t* tok,
                              int32_t                  bw,
                              int32_t                  bh)
{
  ra_reflow_image_box_t* box = &engine->image_boxes[engine->image_box_count];
  box->x                     = (int32_t)k_ra_reflow_margin_px;
  box->y                     = cur->y;
  box->w                     = bw;
  box->h                     = bh;
  box->src_off               = tok->text_off;
  box->src_len               = tok->text_len;
  box->page_index            = engine->page_count;
  box->reserved              = 0U;
  engine->image_box_count++;

  cur->y += bh + (int32_t)k_ra_reflow_paragraph_gap_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
}

/**
 * @brief Lay out a real `<img>` as a block: size it, page-break, record it.
 *
 * @details See implementation. Returns false (caller falls back to the
 * placeholder) on a full image pool, unresolved src, or a flush overflow.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @return Boolean.
 * @retval true Image placed and recorded.
 * @retval false Could not place; caller uses the placeholder.
 * @pre `engine->img_loader != nullptr` and `engine->img_arena != nullptr`.
 * @pre `tok->text_len > 0`.
 * @post On true, one image box exists and the cursor sits below it.
 * @post On false, engine image state is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_place_image(ra_reflow_t* engine, priv_cursor_t* cur, const ra_reflow_token_t* tok)
{
  if (engine->image_box_count >= (uint32_t)k_ra_reflow_max_images) {
    return false;
  }
  int32_t bw = 0;
  int32_t bh = 0;
  if (!priv_image_resolve_size(engine, tok, &bw, &bh)) {
    return false;
  }
  /* Block-level: drop below the current line, then page-break if needed. */
  if (cur->line_has_content != 0U) {
    if (!priv_newline(engine, cur, false)) {
      return false;
    }
  }
  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra_reflow_margin_px;
  if (((cur->y + bh) > bottom_limit) && priv_page_has_content(engine, cur)) {
    if (!priv_finish_page(engine, cur)) {
      return false;
    }
  }
  priv_image_record(engine, cur, tok, bw, bh);
  if (((cur->y + (int32_t)cur->line_height_px) > bottom_limit) &&
      priv_page_has_content(engine, cur)) {
    if (!priv_finish_page(engine, cur)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Apply an `<img>` token: real image when a loader is bound, else a
 *        placeholder.
 *
 * @details See implementation.
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
static bool priv_apply_image(ra_reflow_t* engine, priv_cursor_t* cur, const ra_reflow_token_t* tok)
{
  if ((engine->img_loader != nullptr) && (engine->img_arena != nullptr) && (tok->text_len > 0U)) {
    if (priv_place_image(engine, cur, tok)) {
      return true;
    }
    /* Resolution / pool overflow: fall back to the placeholder advance. */
  }
  return priv_apply_image_placeholder(engine, cur);
}

/**
 * @brief Record one tappable link rect spanning glyphs `[lo, hi)`.
 *
 * @details The glyphs are a same-link, same-baseline run on one page. The rect
 * spans from the first glyph's left edge to the last glyph's right edge
 * (measured by `priv_glyph_advance`), with a generous vertical band
 * (approximately 1.5 em centered on the baseline) for forgiving tap targets.
 * The `target` field is stored 0-based (link - 1). If the link-rect pool is
 * already full the function returns immediately without modifying state.
 *
 * @param[in,out] engine Engine whose link-rect pool grows by one entry.
 * @param[in]     font   Font metrics for last-glyph advance measurement.
 * @param[in]     lo     First glyph index in `engine->glyphs[]` (inclusive).
 * @param[in]     hi     One past the last glyph index (exclusive).
 * @param[in]     link   1-based link identifier; stored as `link - 1`.
 * @param[in]     page   Page index the rect belongs to.
 *
 * @return Nothing.
 *
 * @pre `hi > lo` and both indices are within `engine->glyphs[]`.
 * @pre `link > 0`.
 * @post One link rect appended and `engine->link_rect_count` incremented, or
 *       pool full and no mutation occurred.
 * @post `rect->target == link - 1` when a rect is appended.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static void priv_emit_link_rect(ra_reflow_t*          engine,
                                const stbtt_fontinfo* font,
                                uint32_t              lo,
                                uint32_t              hi,
                                uint8_t               link,
                                uint32_t              page)
{
  if (engine->link_rect_count >= (uint32_t)k_ra_reflow_max_link_rects) {
    return;
  }
  const ra_reflow_glyph_t* gfirst = &engine->glyphs[lo];
  const ra_reflow_glyph_t* glast  = &engine->glyphs[hi - 1U];
  const int32_t            fpx    = (int32_t)gfirst->font_px;
  const int32_t            x1     = glast->x + priv_glyph_advance(font, glast->font_px, glast->cp);
  ra_reflow_link_rect_t*   rect   = &engine->link_rects[engine->link_rect_count];
  rect->x                         = gfirst->x;
  rect->y                         = gfirst->y - fpx;
  rect->w                         = (x1 > gfirst->x) ? (x1 - gfirst->x) : 1;
  rect->h                         = fpx + (fpx / 2);
  rect->target                    = (uint32_t)(link - 1U); /* 1-based -> 0-based */
  rect->page_index                = page;
  engine->link_rect_count++;
}

/**
 * @brief Post-layout pass: group link-tagged glyphs into tappable rects.
 *
 * @details Iterates over every page in `engine->pages[]`. For each page,
 * walks the glyph run in order; when a glyph carries a non-zero `reserved`
 * (link id) it scans forward to find the end of the consecutive same-link,
 * same-baseline-y run, then calls @ref priv_emit_link_rect for that span.
 * A multi-line anchor link therefore produces one rect per wrapped line.
 * Glyphs with `reserved == 0` are skipped.
 *
 * @param[in,out] engine Engine holding laid-out glyphs and pages; its
 *                       link-rect pool grows.
 * @param[in]     font   Font metrics for last-glyph advance measurement.
 *
 * @return Nothing.
 *
 * @pre `engine->pages[]` and `engine->glyphs[]` are fully populated by the
 *      layout pass.
 * @pre `engine->link_rect_count == 0` on entry (reset by `ra_reflow_run_layout`).
 * @post `engine->link_rects[]` contains page-local tappable rectangles for
 *       every link run found.
 * @post `engine->link_rect_count` reflects the total number of rects emitted.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
static void priv_build_link_rects(ra_reflow_t* engine, const stbtt_fontinfo* font)
{
  for (uint32_t p = 0U; p < engine->page_count; ++p) {
    const ra_reflow_page_t* page = &engine->pages[p];
    const uint32_t          base = page->glyph_first;
    uint32_t                k    = 0U;
    while (k < page->glyph_count) {
      const uint8_t link = engine->glyphs[base + k].reserved;
      if (link == 0U) {
        ++k;
        continue;
      }
      const int32_t y = engine->glyphs[base + k].y;
      uint32_t      e = k + 1U;
      while ((e < page->glyph_count) && (engine->glyphs[base + e].reserved == link) &&
             (engine->glyphs[base + e].y == y)) {
        ++e;
      }
      priv_emit_link_rect(engine, font, base + k, base + e, link, p);
      k = e;
    }
  }
}

/**
 * @brief Dispatch one parsed token through the layout cursor.
 *
 * @details
 * Splits the per-token switch out of `priv_layout_tokens()` to keep
 * the outer loop under clang-tidy's cognitive-complexity threshold
 * (Rule 4 / 25-statement budget).
 *
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] font See implementation.
 * @param[in] tok See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_apply_token(ra_reflow_t*             engine,
                                 priv_cursor_t*           cur,
                                 const stbtt_fontinfo*    font,
                                 const ra_reflow_token_t* tok)
{
  /* Pack the per-run embedded-face index (stamped on the token by the cascade,
   * #109) into the free high nibble of the style stamp; bits 0-2 keep the
   * bold/italic/underline emphasis. Single-face content carries face 0, so the
   * style byte is unchanged. */
  cur->active_style =
    (uint8_t)(tok->style | (uint8_t)(((uint32_t)tok->reserved16 & (uint32_t)k_ra_reflow_face_mask)
                                     << (uint32_t)k_ra_reflow_face_shift));
  switch (tok->kind) {
    case k_ra_reflow_tok_block_start:
      return priv_open_block(engine, cur, tok) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_block_end:
      return priv_close_block(engine, cur, tok) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_text:
      return priv_layout_text(engine, cur, font, tok);
    case k_ra_reflow_tok_break:
      return priv_newline(engine, cur, false) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_rule:
      return priv_apply_rule(engine, cur) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_image:
      return priv_apply_image(engine, cur, tok) ? k_ra_ok : k_ra_err_no_mem;
    default:
      return k_ra_ok;
  }
}

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
 * @pre `engine->tokens[start].kind == k_ra_reflow_tok_block_start`.
 * @post Return value is in the range `[start + 1, limit]`.
 * @post Returned index (if < @p limit) refers to a block-end with the same tag.
 *
 * @note Pure read on the token stream; not thread-safe if the stream is concurrently
 *       mutated.
 * @since 0.1.0
 */
static uint32_t priv_match_block_end(const ra_reflow_t* engine, uint32_t start, uint32_t limit)
{
  const uint8_t tag   = engine->tokens[start].tag;
  int32_t       depth = 1;
  for (uint32_t i = start + 1U; i < limit; ++i) {
    const ra_reflow_token_t* tok = &engine->tokens[i];
    if (tok->tag != tag) {
      continue;
    }
    if (tok->kind == (uint8_t)k_ra_reflow_tok_block_start) {
      depth++;
    } else if (tok->kind == (uint8_t)k_ra_reflow_tok_block_end) {
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
 * (must be `k_ra_reflow_tag_td` or `k_ra_reflow_tag_th`). Used by
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
static bool priv_is_cell_start(const ra_reflow_t* engine, uint32_t i)
{
  const ra_reflow_token_t* tok = &engine->tokens[i];
  return (tok->kind == (uint8_t)k_ra_reflow_tok_block_start) &&
         ((tok->tag == (uint8_t)k_ra_reflow_tag_td) || (tok->tag == (uint8_t)k_ra_reflow_tag_th));
}

/**
 * @brief True iff token @p i opens a table row (`<tr>`).
 *
 * @details Checks both the token kind (must be `block_start`) and the tag
 * (must be `k_ra_reflow_tag_tr`). Used by `priv_table_columns` and
 * `priv_layout_table` to locate row boundaries while scanning the flat
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
static bool priv_is_row_start(const ra_reflow_t* engine, uint32_t i)
{
  const ra_reflow_token_t* tok = &engine->tokens[i];
  return (tok->kind == (uint8_t)k_ra_reflow_tok_block_start) &&
         (tok->tag == (uint8_t)k_ra_reflow_tag_tr);
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
static uint32_t priv_table_columns(const ra_reflow_t* engine, uint32_t start, uint32_t end)
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
 * `(*lines)++`. Glyphs are pushed via @ref priv_push_glyph with style 0 and
 * link_id 0 (table cells are always plain body text in v1).
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
static void priv_cell_text(ra_reflow_t*             engine,
                           const stbtt_fontinfo*    font,
                           const ra_reflow_token_t* tok,
                           int32_t                  cell_x,
                           int32_t                  cell_right,
                           uint16_t                 font_px,
                           uint32_t                 color,
                           int32_t*                 cx,
                           int32_t*                 cy,
                           uint32_t*                lines)
{
  const int32_t  line_h = (int32_t)priv_line_height(font_px);
  const uint8_t* base   = engine->text_pool + tok->text_off;
  const uint32_t len    = tok->text_len;
  uint32_t       i      = 0U;
  while (i < len) {
    if (base[i] == ' ') {
      const int32_t adv = priv_glyph_advance(font, font_px, (int32_t)' ');
      if ((*cx > cell_x) && ((*cx + adv) <= cell_right)) {
        (void)priv_push_glyph(engine, *cx, *cy, (int32_t)' ', font_px, 0U, color, 0U);
        *cx += adv;
      }
      ++i;
      continue;
    }
    uint32_t word_end = i;
    int32_t  word_w   = 0;
    while ((word_end < len) && (base[word_end] != ' ')) {
      word_w += priv_glyph_advance(font, font_px, (int32_t)base[word_end]);
      ++word_end;
    }
    if (((*cx + word_w) > cell_right) && (*cx > cell_x)) {
      *cx = cell_x;
      *cy += line_h;
      (*lines)++;
    }
    while (i < word_end) {
      const int32_t adv = priv_glyph_advance(font, font_px, (int32_t)base[i]);
      (void)priv_push_glyph(engine, *cx, *cy, (int32_t)base[i], font_px, 0U, color, 0U);
      *cx += adv;
      ++i;
    }
  }
}

/**
 * @brief Lay out one cell's text tokens within a fixed column box (left-flow).
 *
 * @details Iterates over tokens `[tstart, tend)` and, for each
 * `k_ra_reflow_tok_text` token, delegates to @ref priv_cell_text which
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
static uint32_t priv_layout_cell(ra_reflow_t*          engine,
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
    const ra_reflow_token_t* tok = &engine->tokens[t];
    if (tok->kind == (uint8_t)k_ra_reflow_tok_text) {
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
static uint32_t priv_row_cells(ra_reflow_t*          engine,
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
    const int32_t  cell_x   = (int32_t)k_ra_reflow_margin_px + ((int32_t)col * col_w) + pad;
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
 * flush is issued via @ref priv_finish_page, and the row is re-laid from the
 * top of the fresh page. The cursor is always advanced past the row by
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
static uint32_t priv_layout_row(ra_reflow_t*          engine,
                                priv_cursor_t*        cur,
                                const stbtt_fontinfo* font,
                                uint32_t              tr_start,
                                uint32_t              table_end,
                                int32_t               col_w)
{
  const uint32_t tr_end       = priv_match_block_end(engine, tr_start, table_end);
  const int32_t  line_h       = (int32_t)priv_line_height(engine->font_px);
  const int32_t  bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra_reflow_margin_px;

  const uint32_t glyphs_before = engine->glyph_count;
  uint32_t       row_lines     = priv_row_cells(engine, font, tr_start, tr_end, col_w, cur->y);
  int32_t        row_h         = (int32_t)row_lines * line_h;

  if (((cur->y + row_h) > bottom_limit) && (cur->y > (int32_t)k_ra_reflow_margin_px)) {
    engine->glyph_count = glyphs_before; /* roll the row back ... */
    (void)priv_finish_page(engine, cur); /* ... flush the page (cur->y -> margin) ... */
    row_lines = priv_row_cells(engine, font, tr_start, tr_end, col_w, cur->y); /* ... re-lay it */
    row_h     = (int32_t)row_lines * line_h;
  }
  cur->y += row_h + (int32_t)k_priv_row_gap_px;
  return tr_end + 1U;
}

/**
 * @brief Lay out a `<table>` token range as an equal-column grid.
 *
 * @details Flushes the pending line (if any) via @ref priv_newline, locates
 * the matching table block-end via @ref priv_match_block_end, and counts
 * the maximum column count via @ref priv_table_columns. If the table has no
 * cells the function advances @p out_next past the block-end and returns
 * immediately. Otherwise the content width is divided equally by column count
 * to derive @p col_w, then each `<tr>` block-start in the range is processed
 * by @ref priv_layout_row (which handles row-level page breaks). After all
 * rows are placed the cursor is repositioned to the left margin and a
 * paragraph gap is added so linear text flow resumes cleanly below the table.
 *
 * @param[in,out] engine   Engine whose glyph and page pools grow.
 * @param[in,out] cur      Layout cursor; x, y and line state are updated.
 * @param[in]     font     Font metrics for row and cell layout.
 * @param[in]     start    Token index of the `<table>` block-start.
 * @param[out]    out_next Receives the token index just past the table block-end.
 *
 * @return ra_err_t error code.
 * @retval k_ra_ok       Table laid out; @p out_next set.
 * @retval k_ra_err_no_mem Pending line flush overflowed the page pool.
 *
 * @pre `engine != nullptr`, `cur != nullptr`, `font != nullptr`,
 *      `out_next != nullptr`.
 * @pre `start < engine->token_count` and `engine->tokens[start]` is a
 *      `<table>` block-start.
 * @post On `k_ra_ok`, `*out_next > start` and points past the table block-end.
 * @post On `k_ra_ok`, `cur->align == k_ra_reflow_align_left` and
 *       `cur->line_has_content == 0`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine and @p cur.
 * @since 0.1.0
 */
static ra_err_t priv_layout_table(ra_reflow_t*          engine,
                                  priv_cursor_t*        cur,
                                  const stbtt_fontinfo* font,
                                  uint32_t              start,
                                  uint32_t*             out_next)
{
  if (cur->line_has_content != 0U) {
    if (!priv_newline(engine, cur, false)) {
      return k_ra_err_no_mem;
    }
  }
  const uint32_t end  = priv_match_block_end(engine, start, engine->token_count);
  const uint32_t cols = priv_table_columns(engine, start, end);
  if (cols == 0U) {
    *out_next = (end < engine->token_count) ? (end + 1U) : engine->token_count;
    return k_ra_ok;
  }
  const int32_t content_w = (int32_t)engine->viewport_w - (2 * (int32_t)k_ra_reflow_margin_px);
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
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->align            = (uint8_t)k_ra_reflow_align_left;
  cur->line_has_content = 0U;
  cur->y += (int32_t)k_ra_reflow_paragraph_gap_px;
  cur->line_top         = cur->y;
  cur->line_first_glyph = engine->glyph_count;
  *out_next             = (end < engine->token_count) ? (end + 1U) : engine->token_count;
  return k_ra_ok;
}

/**
 * @brief Run one pass over the token stream populating `engine->glyphs[]`
 *        and `engine->pages[]`.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] font See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_layout_tokens(ra_reflow_t* engine, const stbtt_fontinfo* font)
{
  priv_cursor_t cur = {
    .x                = (int32_t)k_ra_reflow_margin_px,
    .y                = (int32_t)k_ra_reflow_margin_px,
    .line_top         = (int32_t)k_ra_reflow_margin_px,
    .line_height_px   = priv_line_height(engine->font_px),
    .active_font_px   = engine->font_px,
    .active_style     = k_ra_reflow_style_normal,
    .indent_px        = 0U,
    .page_first_glyph = 0U,
    .page_first_image = 0U,
    .line_first_glyph = 0U,
    .line_has_content = 0U,
    .align            = (uint8_t)k_ra_reflow_align_left,
    .reserved8        = {0U, 0U},
  };

  uint32_t i = 0U;
  while (i < engine->token_count) {
    const ra_reflow_token_t* tok = &engine->tokens[i];
    if ((tok->kind == (uint8_t)k_ra_reflow_tok_block_start) &&
        (tok->tag == (uint8_t)k_ra_reflow_tag_table)) {
      ra_err_t err = priv_layout_table(engine, &cur, font, i, &i);
      if (err != k_ra_ok) {
        return err;
      }
    } else {
      ra_err_t err = priv_apply_token(engine, &cur, font, tok);
      if (err != k_ra_ok) {
        return err;
      }
      i++;
    }
  }

  /* Flush the final page if it has glyph or image content. */
  if ((engine->glyph_count > cur.page_first_glyph) ||
      (engine->image_box_count > cur.page_first_image)) {
    if (!priv_finish_page(engine, &cur)) {
      return k_ra_err_no_mem;
    }
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Public helpers (declared in ra_reflow.h)
 * ===========================================================================
 */

ra_err_t ra_reflow_run_layout(ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }

  /* Wipe glyph, page, image, link and anchor state but keep the token stream. */
  engine->glyph_count     = 0U;
  engine->page_count      = 0U;
  engine->image_box_count = 0U;
  engine->link_rect_count = 0U;
  engine->anchor_count    = 0U;

  stbtt_fontinfo font;
  ra_err_t       err = priv_init_font(engine, &font);
  if (err != k_ra_ok) {
    return err;
  }
  err = priv_layout_tokens(engine, &font);
  if (err != k_ra_ok) {
    return err;
  }
  /* Guarantee at least one page on non-empty input. */
  if (ra_reflow_internal_final_page_needed(engine->page_count, engine->token_count)) {
    if (engine->page_count >= k_ra_reflow_max_pages) {
      return k_ra_err_no_mem;
    }
    engine->pages[0].glyph_first = 0U;
    engine->pages[0].glyph_count = engine->glyph_count;
    engine->page_count           = (uint32_t)k_priv_min_chapter_pages;
  }
  /* Build tappable link rectangles from the link-tagged glyphs (#110). */
  priv_build_link_rects(engine, &font);
  return k_ra_ok;
}

/* ===========================================================================
 * Public API entry points -- lifecycle + chapter dispatch
 * ===========================================================================
 */

ra_err_t ra_reflow_init(uint16_t       viewport_w,
                        uint16_t       viewport_h,
                        const uint8_t* font_data,
                        size_t         font_len,
                        uint16_t       font_px,
                        uint32_t       body_color,
                        uint32_t       link_color,
                        ra_reflow_t*   out_engine)
{
  if (font_data == nullptr || out_engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (viewport_w == 0U || viewport_h == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (font_px < k_ra_reflow_min_font_px || font_px > k_ra_reflow_max_font_px) {
    return k_ra_err_invalid_arg;
  }
  if (font_len < 16U) {
    return k_ra_err_invalid_size;
  }

  priv_byte_zero((uint8_t*)out_engine, sizeof(*out_engine));

  out_engine->viewport_w = viewport_w;
  out_engine->viewport_h = viewport_h;
  out_engine->font_data  = font_data;
  out_engine->font_len   = font_len;
  out_engine->font_px    = font_px;
  out_engine->body_color = body_color;
  out_engine->link_color = link_color;
  out_engine->in_use     = 1U;
  return k_ra_ok;
}

ra_err_t ra_reflow_close(ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  engine->in_use          = 0U;
  engine->page_count      = 0U;
  engine->glyph_count     = 0U;
  engine->token_count     = 0U;
  engine->text_pool_used  = 0U;
  engine->image_box_count = 0U;
  engine->xhtml_buf       = nullptr;
  engine->xhtml_len       = 0U;
  return k_ra_ok;
}

ra_err_t ra_reflow_set_image_loader(ra_reflow_t*              engine,
                                    ra_reflow_image_loader_fn loader,
                                    void*                     ctx,
                                    ra_img_arena_t*           arena)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  engine->img_loader     = loader;
  engine->img_loader_ctx = ctx;
  engine->img_arena      = arena;
  return k_ra_ok;
}

ra_err_t ra_reflow_set_css_loader(ra_reflow_t* engine, ra_reflow_css_loader_fn loader, void* ctx)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  engine->css_loader     = loader;
  engine->css_loader_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_reflow_layout_chapter(ra_reflow_t*   engine,
                                  const uint8_t* xhtml_buf,
                                  size_t         xhtml_len,
                                  uint32_t*      out_total_pages)
{
  if (engine == nullptr || xhtml_buf == nullptr || out_total_pages == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  *out_total_pages = 0U;

  ra_err_t err = ra_reflow_parse_xhtml(engine, xhtml_buf, xhtml_len);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_reflow_run_layout(engine);
  if (err != k_ra_ok) {
    return err;
  }

  /* Cache the pointer so set_font_size() can re-flow without the
   * caller resupplying the buffer. */
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  *out_total_pages  = engine->page_count;
  return k_ra_ok;
}

ra_err_t ra_reflow_get_page_count(const ra_reflow_t* engine, uint32_t* out_count)
{
  if (engine == nullptr || out_count == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  *out_count = engine->page_count;
  return k_ra_ok;
}

ra_err_t ra_reflow_set_font_size(ra_reflow_t* engine, uint16_t new_font_px)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (new_font_px < k_ra_reflow_min_font_px || new_font_px > k_ra_reflow_max_font_px) {
    return k_ra_err_invalid_arg;
  }
  if (ra_reflow_internal_xhtml_invalid(engine->xhtml_buf, engine->xhtml_len)) {
    return k_ra_err_invalid_state;
  }
  engine->font_px = new_font_px;

  /* Re-flow against the cached buffer. */
  uint32_t pages = 0U;
  return ra_reflow_layout_chapter(engine, engine->xhtml_buf, engine->xhtml_len, &pages);
}

/** @brief Implementation of `ra_reflow_bind_font()` -- validate then swap. */
ra_err_t ra_reflow_bind_font(ra_reflow_t* engine, const uint8_t* font_data, size_t font_len)
{
  if (engine == nullptr || font_data == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (font_len < (size_t)k_ra_reflow_min_font_bytes) {
    return k_ra_err_invalid_size;
  }

  /* Validate the blob BEFORE swapping; on failure the current face is kept
   * (graceful degradation). The probe fontinfo is a stack temporary -- the
   * layout / render paths re-init their own per-call fontinfo from font_data. */
  const int32_t  offset = stbtt_GetFontOffsetForIndex(font_data, 0);
  stbtt_fontinfo probe;
  if (offset < 0 || stbtt_InitFont(&probe, font_data, offset) == 0) {
    return k_ra_err_not_supported;
  }

  engine->font_data = font_data;
  engine->font_len  = font_len;

  /* If a chapter is already laid out, re-flow it against the new face so the
   * change is immediate (mirrors ra_reflow_set_font_size); otherwise the next
   * layout picks it up. */
  if (!ra_reflow_internal_xhtml_invalid(engine->xhtml_buf, engine->xhtml_len)) {
    uint32_t pages = 0U;
    return ra_reflow_layout_chapter(engine, engine->xhtml_buf, engine->xhtml_len, &pages);
  }
  return k_ra_ok;
}

/** @brief Implementation of `ra_reflow_register_face()` -- validate then append. */
ra_err_t
ra_reflow_register_face(ra_reflow_t* engine, uint8_t css_face_idx, const uint8_t* blob, size_t len)
{
  if (engine == nullptr || blob == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (len < (size_t)k_ra_reflow_min_font_bytes) {
    return k_ra_err_invalid_size;
  }
  if (engine->face_count >= (uint8_t)k_ra_reflow_max_faces) {
    return k_ra_err_no_mem;
  }
  /* Validate the blob before recording it; a bad face is rejected so layout falls
   * back to the default face (graceful degradation). The probe is a stack
   * temporary -- the render pass re-inits its own per-face fontinfo. */
  const int32_t  offset = stbtt_GetFontOffsetForIndex(blob, 0);
  stbtt_fontinfo probe;
  if (offset < 0 || stbtt_InitFont(&probe, blob, offset) == 0) {
    return k_ra_err_not_supported;
  }
  ra_reflow_face_t* face = &engine->faces[engine->face_count];
  face->blob             = blob;
  face->len              = len;
  face->css_face_idx     = css_face_idx;
  engine->face_count++;
  return k_ra_ok;
}
