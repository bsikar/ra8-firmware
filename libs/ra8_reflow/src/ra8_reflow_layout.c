/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_reflow_layout.c
 * @brief Greedy line-break + page-break engine for ra8_reflow.
 *
 * @details
 * Consumes the token stream produced by `ra8_reflow_parse_xhtml()` and
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
 *
 * [Ring 4 / Reflow]
 * {World: NS}
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

/**
 * @brief Return true iff @p tag is a block-level indent tag.
 *
 * @details Pure helper factored out of @c priv_open_block (line 479)
 *          and @c priv_close_block (line 513) so the
 *          ``tag == li || tag == blockquote`` decision can be driven
 *          directly by host MC/DC tests via
 *          @ref ra8_reflow_internal_is_indent_tag.
 *
 * @param[in] tag Token tag value (raw @c uint8_t storage of
 *                @ref ra8_reflow_html_tag_t).
 *
 * @return Boolean indent-tag predicate.
 * @retval true  Tag is @c k_ra8_reflow_tag_li or @c k_ra8_reflow_tag_blockquote.
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
bool ra8_reflow_internal_is_indent_tag(uint8_t tag)
{
  return (tag == (uint8_t)k_ra8_reflow_tag_li) || (tag == (uint8_t)k_ra8_reflow_tag_blockquote);
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
 * @retval true Caller must call ra8_reflow_layout_newline before emitting.
 * @retval false Emitting in place is safe.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the four arguments.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
bool ra8_reflow_internal_right_overflow_break(int32_t cursor_x,
                                              int32_t advance,
                                              int32_t right_limit,
                                              uint8_t line_has_content)
{
  return (cursor_x + advance > right_limit) && (line_has_content != 0U);
}

/**
 * @brief OR helper for the cached-XHTML invalid decision.
 * @details Promoted from line 953 in @c ra8_reflow_set_font_size.
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
bool ra8_reflow_internal_xhtml_invalid(const void* xhtml_buf, size_t xhtml_len)
{
  return (xhtml_buf == nullptr) || (xhtml_len == 0U);
}

/**
 * @brief AND helper for the synthesise-final-page decision.
 * @details Promoted from line 750 in @c ra8_reflow_run_layout.
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
bool ra8_reflow_internal_final_page_needed(uint32_t page_count, uint32_t token_count)
{
  return (page_count == 0U) && (token_count > 0U);
}

/** @brief Implementation of `ra8_reflow_layout_byte_zero()` -- bounded byte-walk. */
void ra8_reflow_layout_byte_zero(uint8_t* dst, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    dst[i] = 0U;
  }
}

/* ===========================================================================
 * Helpers
 *
 * The internal sizing constants (`priv_layout_consts_t`) and the mutable
 * layout cursor (`priv_cursor_t`) are shared across the layout translation
 * units and live in `ra8_reflow_layout_internal.h`.
 * ===========================================================================
 */

/** @brief Implementation of `ra8_reflow_layout_init_font()` -- parse the TTF blob. */
ra8_err_t ra8_reflow_layout_init_font(const ra8_reflow_t* engine, stbtt_fontinfo* out_font)
{
  const int32_t offset = stbtt_GetFontOffsetForIndex(engine->font_data, 0);
  if (offset < 0) {
    return k_ra8_err_validation_failed;
  }
  if (stbtt_InitFont(out_font, engine->font_data, (int)engine->font_len, offset) == 0) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_reflow_layout_line_height()` -- scaled integer ratio. */
uint16_t ra8_reflow_layout_line_height(uint16_t font_px)
{
  const uint32_t lh = ((uint32_t)font_px * (uint32_t)k_ra8_reflow_line_spacing_num) /
                      (uint32_t)k_ra8_reflow_line_spacing_den;
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t priv_block_font_px(uint16_t body_px, ra8_reflow_html_tag_t tag)
{
  uint32_t pct = k_ra8_reflow_pct_full;
  switch (tag) {
    case k_ra8_reflow_tag_h1:
      pct = k_ra8_reflow_h1_scale_pct;
      break;
    case k_ra8_reflow_tag_h2:
      pct = k_ra8_reflow_h2_scale_pct;
      break;
    case k_ra8_reflow_tag_h3:
      pct = k_ra8_reflow_h3_scale_pct;
      break;
    case k_ra8_reflow_tag_h4:
      pct = k_ra8_reflow_h4_scale_pct;
      break;
    case k_ra8_reflow_tag_h5:
      pct = k_ra8_reflow_h5_scale_pct;
      break;
    case k_ra8_reflow_tag_h6:
      pct = k_ra8_reflow_h6_scale_pct;
      break;
    default:
      pct = k_ra8_reflow_pct_full;
      break;
  }
  return (uint16_t)(((uint32_t)body_px * pct) / k_ra8_reflow_pct_full);
}

/** @brief Implementation of `ra8_reflow_layout_glyph_advance()` -- scaled hmetrics. */
int32_t ra8_reflow_layout_glyph_advance(const stbtt_fontinfo* font, uint16_t font_px, int32_t cp)
{
  const float scale         = stbtt_ScaleForPixelHeight(font, (float)font_px);
  int         advance_units = 0;
  int         lsb           = 0;
  stbtt_GetCodepointHMetrics(font, cp, &advance_units, &lsb);
  return (int32_t)((float)advance_units * scale);
}

/** @brief Implementation of `ra8_reflow_layout_push_glyph()` -- append + bounds check. */
bool ra8_reflow_layout_push_glyph(ra8_reflow_t* engine,
                                  int32_t       x,
                                  int32_t       y,
                                  int32_t       cp,
                                  uint16_t      font_px,
                                  uint8_t       style,
                                  uint32_t      color,
                                  uint8_t       link_id)
{
  if (engine->glyph_count >= k_ra8_reflow_max_glyphs) {
    return false;
  }
  ra8_reflow_glyph_t* g = &engine->glyphs[engine->glyph_count];
  g->x                  = x;
  g->y                  = y;
  g->cp                 = cp;
  g->color              = color;
  g->font_px            = font_px;
  g->style              = style;
  g->reserved           = link_id; /* 1-based `<a>` link id (0 = not a link) */
  engine->glyph_count++;
  return true;
}

/** @brief Implementation of `ra8_reflow_layout_finish_page()` -- flush + reset cursor. */
bool ra8_reflow_layout_finish_page(ra8_reflow_t* engine, priv_cursor_t* cur)
{
  if (engine->page_count >= k_ra8_reflow_max_pages) {
    return false;
  }
  const uint32_t     count = engine->glyph_count - cur->page_first_glyph;
  ra8_reflow_page_t* page  = &engine->pages[engine->page_count];
  page->glyph_first        = cur->page_first_glyph;
  page->glyph_count        = count;
  engine->page_count++;
  cur->page_first_glyph = engine->glyph_count;
  cur->page_first_image = engine->image_box_count;
  cur->line_first_glyph = engine->glyph_count;
  cur->y                = (int32_t)k_ra8_reflow_margin_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra8_reflow_margin_px + (int32_t)cur->indent_px;
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
RA8_INTERNAL
static void priv_justify_glyphs(ra8_reflow_t* engine, uint32_t lo, uint32_t hi, int32_t slack)
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
 * @pre `cur->align` is a valid `ra8_reflow_align_t` value.
 * @post Glyph x positions in the line range are adjusted per the alignment.
 * @post For justify mode with no space glyphs, positions are left unchanged.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_finish_line(ra8_reflow_t* engine, priv_cursor_t* cur, bool allow_justify)
{
  if (cur->align == (uint8_t)k_ra8_reflow_align_left) {
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
  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra8_reflow_margin_px;
  const int32_t slack       = right_limit - content_right;
  if ((hi <= lo) || (slack <= 0)) {
    return;
  }
  if (cur->align == (uint8_t)k_ra8_reflow_align_justify) {
    if (allow_justify) {
      priv_justify_glyphs(engine, lo, hi, slack);
    }
    return;
  }
  const int32_t offset = (cur->align == (uint8_t)k_ra8_reflow_align_center) ? (slack / 2) : slack;
  for (uint32_t i = lo; i < hi; ++i) {
    engine->glyphs[i].x += offset;
  }
}

/** @brief Implementation of `ra8_reflow_layout_newline()` -- align, advance, page-break. */
bool ra8_reflow_layout_newline(ra8_reflow_t* engine, priv_cursor_t* cur, bool allow_justify)
{
  priv_finish_line(engine, cur, allow_justify);
  cur->y += (int32_t)cur->line_height_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra8_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
  cur->line_first_glyph = engine->glyph_count;

  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra8_reflow_margin_px;
  if (cur->y + (int32_t)cur->line_height_px > bottom_limit) {
    if (!ra8_reflow_layout_finish_page(engine, cur)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Append one ASCII code point at the current cursor, wrapping
 *        if it would overflow the right margin.
 *
 * @details Measures the advance width of @p cp via @ref ra8_reflow_layout_glyph_advance,
 * clamps it to at least `k_priv_min_word_w_px` to prevent zero-width stalls,
 * and checks the right-overflow predicate
 * (@ref ra8_reflow_internal_right_overflow_break). If a line break is needed
 * and the line already has content, @ref ra8_reflow_layout_newline is called before the
 * glyph is pushed. The glyph is then appended via @ref ra8_reflow_layout_push_glyph and
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
 * @return ra8_err_t error code.
 * @retval k_ra8_ok       Glyph emitted successfully.
 * @retval k_ra8_err_no_mem Page pool or glyph pool overflowed.
 *
 * @pre `engine != nullptr`, `cur != nullptr`, `font != nullptr`.
 * @pre `cur->active_font_px > 0`.
 * @post On `k_ra8_ok`, `engine->glyph_count` has increased by one.
 * @post On `k_ra8_ok`, `cur->line_has_content == 1`.
 *
 * @note Not thread-safe; caller must serialize access to @p engine.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_emit_char(ra8_reflow_t*         engine,
                                priv_cursor_t*        cur,
                                const stbtt_fontinfo* font,
                                int32_t               cp,
                                uint32_t              color,
                                uint8_t               link_id)
{
  const int32_t advance = ra8_reflow_layout_glyph_advance(font, cur->active_font_px, cp);
  const int32_t advance_clamped =
    (advance < (int32_t)k_priv_min_word_w_px) ? (int32_t)k_priv_min_word_w_px : advance;

  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra8_reflow_margin_px;
  if (ra8_reflow_internal_right_overflow_break(cur->x,
                                               advance_clamped,
                                               right_limit,
                                               cur->line_has_content)) {
    if (!ra8_reflow_layout_newline(engine, cur, true)) { /* wrap -> previous line may justify */
      return k_ra8_err_no_mem;
    }
  }
  if (!ra8_reflow_layout_push_glyph(engine,
                                    cur->x,
                                    cur->y,
                                    cp,
                                    cur->active_font_px,
                                    cur->active_style,
                                    color,
                                    link_id)) {
    return k_ra8_err_no_mem;
  }
  cur->x += advance_clamped;
  cur->line_has_content = 1U;
  return k_ra8_ok;
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_layout_text(ra8_reflow_t*             engine,
                                  priv_cursor_t*            cur,
                                  const stbtt_fontinfo*     font,
                                  const ra8_reflow_token_t* tok)
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
    (tok->color != (uint32_t)k_ra8_reflow_color_inherit) ? tok->color : fallback;

  /* Pre-scan to find each word boundary. Greedy: if word_w + cursor_x
   * exceeds the right margin AND the line already has content, break
   * before emitting the word. */
  uint32_t i = 0U;
  while (i < len) {
    /* Emit any leading whitespace as one space (already collapsed by
     * the parser). */
    if (base[i] == ' ') {
      ra8_err_t err = priv_emit_char(engine, cur, font, ' ', color, link_id);
      if (err != k_ra8_ok) {
        return err;
      }
      ++i;
      continue;
    }

    /* Word: bytes up to the next space. Pre-measure for greedy break. */
    uint32_t word_end = i;
    int32_t  word_w   = 0;
    while (word_end < len && base[word_end] != ' ') {
      word_w += ra8_reflow_layout_glyph_advance(font, cur->active_font_px, (int32_t)base[word_end]);
      ++word_end;
    }
    const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra8_reflow_margin_px;
    if (ra8_reflow_internal_right_overflow_break(cur->x,
                                                 word_w,
                                                 right_limit,
                                                 cur->line_has_content)) {
      if (!ra8_reflow_layout_newline(engine, cur, true)) { /* wrap -> previous line may justify */
        return k_ra8_err_no_mem;
      }
    }
    while (i < word_end) {
      ra8_err_t err = priv_emit_char(engine, cur, font, (int32_t)base[i], color, link_id);
      if (err != k_ra8_ok) {
        return err;
      }
      ++i;
    }
  }
  return k_ra8_ok;
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_open_block(ra8_reflow_t* engine, priv_cursor_t* cur, const ra8_reflow_token_t* tok)
{
  if (cur->line_has_content != 0U) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return false;
    }
  }
  /* The block's text alignment travels in the block-start token's reserved byte
   * (set by the tokenizer from `style="text-align:..."`). */
  cur->align = tok->reserved;
  /* Record an `id=` anchor (carried in the block-start token's slice) at this
   * block's page + top, for same-chapter `#fragment` jumps. */
  if ((tok->text_len > 0U) && (engine->anchor_count < (uint32_t)k_ra8_reflow_max_anchors)) {
    ra8_reflow_anchor_t* anchor = &engine->anchors[engine->anchor_count];
    anchor->id_off              = tok->text_off;
    anchor->id_len              = tok->text_len;
    anchor->page_index          = engine->page_count;
    anchor->y                   = cur->y;
    engine->anchor_count++;
  }
  /* CSS `font-size` (#140) on the block-start token wins; else the UA default
   * (body size or heading scale). 0 = no CSS font, so unstyled content is
   * byte-identical. */
  cur->active_font_px = (tok->css_font_px != 0U)
                          ? tok->css_font_px
                          : priv_block_font_px(engine->font_px, (ra8_reflow_html_tag_t)tok->tag);
  cur->line_height_px = ra8_reflow_layout_line_height(cur->active_font_px);
  if (ra8_reflow_internal_is_indent_tag((uint8_t)tok->tag)) {
    cur->indent_px = k_ra8_reflow_indent_px;
    cur->x         = (int32_t)k_ra8_reflow_margin_px + (int32_t)cur->indent_px;
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_close_block(ra8_reflow_t* engine, priv_cursor_t* cur, const ra8_reflow_token_t* tok)
{
  if (cur->line_has_content != 0U) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return false;
    }
  }
  /* Paragraph gap. */
  cur->y += (int32_t)k_ra8_reflow_paragraph_gap_px;
  cur->line_top = cur->y;
  cur->x        = (int32_t)k_ra8_reflow_margin_px;
  if (ra8_reflow_internal_is_indent_tag((uint8_t)tok->tag)) {
    cur->indent_px = 0U;
  }
  cur->active_font_px = engine->font_px;
  cur->line_height_px = ra8_reflow_layout_line_height(cur->active_font_px);

  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra8_reflow_margin_px;
  if (cur->y + (int32_t)cur->line_height_px > bottom_limit) {
    if (!ra8_reflow_layout_finish_page(engine, cur)) {
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_apply_rule(ra8_reflow_t* engine, priv_cursor_t* cur)
{
  if (cur->line_has_content != 0U) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return false;
    }
  }
  cur->y += (int32_t)k_priv_hr_thickness_px;
  cur->line_top = cur->y;
  return true;
}

/**
 * @brief Record one tappable link rect spanning glyphs `[lo, hi)`.
 *
 * @details The glyphs are a same-link, same-baseline run on one page. The rect
 * spans from the first glyph's left edge to the last glyph's right edge
 * (measured by `ra8_reflow_layout_glyph_advance`), with a generous vertical band
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
RA8_INTERNAL
static void priv_emit_link_rect(ra8_reflow_t*         engine,
                                const stbtt_fontinfo* font,
                                uint32_t              lo,
                                uint32_t              hi,
                                uint8_t               link,
                                uint32_t              page)
{
  if (engine->link_rect_count >= (uint32_t)k_ra8_reflow_max_link_rects) {
    return;
  }
  const ra8_reflow_glyph_t* gfirst = &engine->glyphs[lo];
  const ra8_reflow_glyph_t* glast  = &engine->glyphs[hi - 1U];
  const int32_t             fpx    = (int32_t)gfirst->font_px;
  const int32_t x1 = glast->x + ra8_reflow_layout_glyph_advance(font, glast->font_px, glast->cp);
  ra8_reflow_link_rect_t* rect = &engine->link_rects[engine->link_rect_count];
  rect->x                      = gfirst->x;
  rect->y                      = gfirst->y - fpx;
  rect->w                      = (x1 > gfirst->x) ? (x1 - gfirst->x) : 1;
  rect->h                      = fpx + (fpx / 2);
  rect->target                 = (uint32_t)(link - 1U); /* 1-based -> 0-based */
  rect->page_index             = page;
  engine->link_rect_count++;
}

/** @brief Implementation of `ra8_reflow_layout_build_link_rects()` -- per-page link runs. */
void ra8_reflow_layout_build_link_rects(ra8_reflow_t* engine, const stbtt_fontinfo* font)
{
  for (uint32_t p = 0U; p < engine->page_count; ++p) {
    const ra8_reflow_page_t* page = &engine->pages[p];
    const uint32_t           base = page->glyph_first;
    uint32_t                 k    = 0U;
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

/** @brief Implementation of `ra8_reflow_layout_apply_token()` -- per-token switch. */
ra8_err_t ra8_reflow_layout_apply_token(ra8_reflow_t*             engine,
                                        priv_cursor_t*            cur,
                                        const stbtt_fontinfo*     font,
                                        const ra8_reflow_token_t* tok)
{
  /* Pack the per-run embedded-face index (stamped on the token by the cascade,
   * #109) into the free high nibble of the style stamp; bits 0-2 keep the
   * bold/italic/underline emphasis. Single-face content carries face 0, so the
   * style byte is unchanged. */
  cur->active_style =
    (uint8_t)(tok->style | (uint8_t)(((uint32_t)tok->reserved16 & (uint32_t)k_ra8_reflow_face_mask)
                                     << (uint32_t)k_ra8_reflow_face_shift));
  switch (tok->kind) {
    case k_ra8_reflow_tok_block_start:
      return priv_open_block(engine, cur, tok) ? k_ra8_ok : k_ra8_err_no_mem;
    case k_ra8_reflow_tok_block_end:
      return priv_close_block(engine, cur, tok) ? k_ra8_ok : k_ra8_err_no_mem;
    case k_ra8_reflow_tok_text:
      return priv_layout_text(engine, cur, font, tok);
    case k_ra8_reflow_tok_break:
      return ra8_reflow_layout_newline(engine, cur, false) ? k_ra8_ok : k_ra8_err_no_mem;
    case k_ra8_reflow_tok_rule:
      return priv_apply_rule(engine, cur) ? k_ra8_ok : k_ra8_err_no_mem;
    case k_ra8_reflow_tok_image:
      return ra8_reflow_layout_apply_image(engine, cur, tok) ? k_ra8_ok : k_ra8_err_no_mem;
    default:
      return k_ra8_ok;
  }
}
