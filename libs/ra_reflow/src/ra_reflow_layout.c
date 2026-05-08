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
  uint8_t  in_link;          /**< 1 while inside an `<a>` block. */
  uint16_t indent_px;        /**< Active left indent (li, blockquote). */
  uint32_t page_first_glyph; /**< First glyph index of the active page. */
  uint8_t  line_has_content; /**< 1 once any glyph landed on this line. */
  uint8_t  reserved8[3];     /**< Padding. */
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
 * @brief Push one positioned glyph into the engine pool.
 *
 * @return false on overflow, true otherwise.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] x See implementation.
 * @param[in] y See implementation.
 * @param[in] cp See implementation.
 * @param[in] font_px See implementation.
 * @param[in] style See implementation.
 * @param[in] color See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool priv_push_glyph(ra_reflow_t* engine,
                            int32_t      x,
                            int32_t      y,
                            int32_t      cp,
                            uint16_t     font_px,
                            uint8_t      style,
                            uint32_t     color)
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
  g->reserved          = 0U;
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
  cur->y                = (int32_t)k_ra_reflow_margin_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
  return true;
}

/**
 * @brief Wrap to a new line, finishing a page if the bottom margin is hit.
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
static bool priv_newline(ra_reflow_t* engine, priv_cursor_t* cur)
{
  cur->y += (int32_t)cur->line_height_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;

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
 *        if it would overflow the viewport.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] font See implementation.
 * @param[in] cp See implementation.
 * @param[in] color See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_emit_char(ra_reflow_t*          engine,
                               priv_cursor_t*        cur,
                               const stbtt_fontinfo* font,
                               int32_t               cp,
                               uint32_t              color)
{
  const int32_t advance = priv_glyph_advance(font, cur->active_font_px, cp);
  const int32_t advance_clamped =
    (advance < (int32_t)k_priv_min_word_w_px) ? (int32_t)k_priv_min_word_w_px : advance;

  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
  if (ra_reflow_internal_right_overflow_break(cur->x,
                                              advance_clamped,
                                              right_limit,
                                              cur->line_has_content)) {
    if (!priv_newline(engine, cur)) {
      return k_ra_err_no_mem;
    }
  }
  if (!priv_push_glyph(engine, cur->x, cur->y, cp, cur->active_font_px, cur->active_style, color)) {
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
  const uint8_t* base  = engine->text_pool + tok->text_off;
  const uint32_t len   = tok->text_len;
  const uint32_t color = (cur->in_link != 0U) ? engine->link_color : engine->body_color;

  /* Pre-scan to find each word boundary. Greedy: if word_w + cursor_x
   * exceeds the right margin AND the line already has content, break
   * before emitting the word. */
  uint32_t i = 0U;
  while (i < len) {
    /* Emit any leading whitespace as one space (already collapsed by
     * the parser). */
    if (base[i] == ' ') {
      ra_err_t err = priv_emit_char(engine, cur, font, ' ', color);
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
      if (!priv_newline(engine, cur)) {
        return k_ra_err_no_mem;
      }
    }
    while (i < word_end) {
      ra_err_t err = priv_emit_char(engine, cur, font, (int32_t)base[i], color);
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
    if (!priv_newline(engine, cur)) {
      return false;
    }
  }
  cur->active_font_px = priv_block_font_px(engine->font_px, (ra_reflow_html_tag_t)tok->tag);
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
    if (!priv_newline(engine, cur)) {
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
    if (!priv_newline(engine, cur)) {
      return false;
    }
  }
  cur->y += (int32_t)k_priv_hr_thickness_px;
  cur->line_top = cur->y;
  return true;
}

/**
 * @brief Apply an `<img>` token: reserve a placeholder rectangle.
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
static bool priv_apply_image(ra_reflow_t* engine, priv_cursor_t* cur)
{
  const int32_t advance     = (int32_t)k_priv_image_placeholder_px;
  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra_reflow_margin_px;
  if (ra_reflow_internal_right_overflow_break(cur->x,
                                              advance,
                                              right_limit,
                                              cur->line_has_content)) {
    if (!priv_newline(engine, cur)) {
      return false;
    }
  }
  cur->x += advance;
  cur->line_has_content = 1U;
  return true;
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
  cur->active_style = tok->style;
  cur->in_link      = ((tok->style & k_ra_reflow_style_underline) != 0U) ? 1U : 0U;
  switch (tok->kind) {
    case k_ra_reflow_tok_block_start:
      return priv_open_block(engine, cur, tok) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_block_end:
      return priv_close_block(engine, cur, tok) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_text:
      return priv_layout_text(engine, cur, font, tok);
    case k_ra_reflow_tok_break:
      return priv_newline(engine, cur) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_rule:
      return priv_apply_rule(engine, cur) ? k_ra_ok : k_ra_err_no_mem;
    case k_ra_reflow_tok_image:
      return priv_apply_image(engine, cur) ? k_ra_ok : k_ra_err_no_mem;
    default:
      return k_ra_ok;
  }
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
    .in_link          = 0U,
    .indent_px        = 0U,
    .page_first_glyph = 0U,
    .line_has_content = 0U,
    .reserved8        = {0U, 0U, 0U},
  };

  for (uint32_t i = 0U; i < engine->token_count; ++i) {
    ra_err_t err = priv_apply_token(engine, &cur, font, &engine->tokens[i]);
    if (err != k_ra_ok) {
      return err;
    }
  }

  /* Flush the final page if it has content. */
  if (engine->glyph_count > cur.page_first_glyph) {
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

/* Implementation of ra_reflow_run_layout (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_reflow_run_layout(ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }

  /* Wipe glyph and page state but keep the token stream. */
  engine->glyph_count = 0U;
  engine->page_count  = 0U;

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
  return k_ra_ok;
}

/* ===========================================================================
 * Public API entry points -- lifecycle + chapter dispatch
 * ===========================================================================
 */

/* Implementation of ra_reflow_init (see header for full contract) -- see header for the documented contract. */
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

/* Implementation of ra_reflow_close (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_reflow_close(ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  engine->in_use         = 0U;
  engine->page_count     = 0U;
  engine->glyph_count    = 0U;
  engine->token_count    = 0U;
  engine->text_pool_used = 0U;
  engine->xhtml_buf      = nullptr;
  engine->xhtml_len      = 0U;
  return k_ra_ok;
}

/* Implementation of ra_reflow_layout_chapter (see header for full contract) -- see header for the documented contract. */
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

/* Implementation of ra_reflow_get_page_count (see header for full contract) -- see header for the documented contract. */
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

/* Implementation of ra_reflow_set_font_size (see header for full contract) -- see header for the documented contract. */
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
