/**
 * @file ra_reflow.h
 * @brief HTML / CSS reflow + paginate engine for the ra8d2 ereader.
 *
 * @details
 * `ra_reflow` is a small, hand-written HTML reflow + pagination engine
 * sitting between `libs/ra_epub` (which produces per-chapter XHTML
 * byte streams) and `libs/ra_gfx` (which owns the framebuffer). It is
 * the page-layout core of the ereader application.
 *
 * The engine is intentionally bare-metal friendly:
 *
 *   - **Zero dynamic allocation.** Every buffer is statically sized via
 *     a `ra_reflow_limits_t` enum and lives inside the engine handle.
 *   - **Greedy line-break.** Walks each text run word-by-word measuring
 *     glyph widths via `stb_truetype`; when adding the next word would
 *     exceed `viewport_w - 2 * k_ra_reflow_margin_px`, the engine
 *     breaks and starts a new line.
 *   - **Page-break-on-overflow.** When the accumulated line height
 *     would exceed `viewport_h - 2 * k_ra_reflow_margin_px`, the
 *     engine starts a new page.
 *   - **Caller-owned framebuffer.** Rendering blits each glyph via
 *     `ra_gfx_pixel()` so the same engine works on the GLCDC plane,
 *     an off-screen scratch buffer, or a host-test buffer.
 *
 * ## Supported HTML subset (v1)
 *
 *   - Block-flow tags:    `<p>`, `<h1>` .. `<h6>`, `<blockquote>`,
 *                          `<ul>`, `<ol>`, `<li>`, `<hr>`.
 *   - Inline tags:        `<em>`, `<strong>`, `<b>`, `<i>`, `<a>`,
 *                          `<br>`.
 *   - Replaced elements:  `<img>` -- rendered as a 1-pixel placeholder
 *                          rectangle (image decoding is deferred).
 *   - Everything else (CSS, scripts, `<div>`, `<span>`) is treated as
 *     a transparent flow-pass-through; child content is still laid
 *     out, the wrapping element itself contributes no styling.
 *
 * ## Lifecycle
 *
 *   1. `ra_reflow_init()` -- bind viewport, font, colours.
 *   2. `ra_reflow_layout_chapter()` -- parse + lay out one XHTML
 *      chapter, return total page count.
 *   3. `ra_reflow_render_page()` -- rasterise page N into the active
 *      ra_gfx framebuffer.
 *   4. (optional) `ra_reflow_set_font_size()` -- triggers a re-flow on
 *      the cached chapter.
 *   5. `ra_reflow_close()` -- mark engine unused.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_reflow_image.h" /* ra_img_arena_t for the decode scratch */

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra_reflow_limits_t
 * @brief Static-allocation caps for the reflow engine.
 *
 * @details
 * The engine holds a fixed number of parsed tokens, positioned glyphs,
 * and pages. Anything bigger is rejected with `k_ra_err_no_mem`. Sized
 * to fit the EK-RA8D2's 2 MB SRAM with comfortable headroom.
 */
typedef enum : uint32_t {
  k_ra_reflow_max_tokens         = 4096U,  /**< Max parsed token count.      */
  k_ra_reflow_text_pool_bytes    = 65536U, /**< Bytes of text pool.          */
  k_ra_reflow_max_glyphs         = 32768U, /**< Total positioned glyphs.     */
  k_ra_reflow_max_pages          = 256U,   /**< Max paginated pages.         */
  k_ra_reflow_max_lines_per_page = 96U,    /**< Lines per page upper bound.  */
  k_ra_reflow_max_images         = 64U,    /**< Max laid-out `<img>` boxes.  */
} ra_reflow_limits_t;

/**
 * @enum ra_reflow_layout_t
 * @brief Layout-axis named constants used by the line-break engine.
 *
 * @details
 * These values name the magic numbers that would otherwise show up in
 * the layout pass: page margin, line-spacing multiplier numerator /
 * denominator, default font size, and the heading multipliers H1..H6.
 */
typedef enum : uint16_t {
  k_ra_reflow_margin_px        = 16U,  /**< Page edge inset, pixels.        */
  k_ra_reflow_line_spacing_num = 12U,  /**< Line height numerator (= 1.2x). */
  k_ra_reflow_line_spacing_den = 10U,  /**< Line height denominator.        */
  k_ra_reflow_paragraph_gap_px = 8U,   /**< Vertical gap after a block.     */
  k_ra_reflow_indent_px        = 24U,  /**< Indent for blockquote / li.     */
  k_ra_reflow_default_font_px  = 18U,  /**< Default body font size.         */
  k_ra_reflow_min_font_px      = 8U,   /**< Smallest accepted font size.    */
  k_ra_reflow_max_font_px      = 96U,  /**< Largest accepted font size.     */
  k_ra_reflow_h1_scale_pct     = 200U, /**< H1 size = 200 % of body.       */
  k_ra_reflow_h2_scale_pct     = 175U, /**< H2 size = 175 % of body.       */
  k_ra_reflow_h3_scale_pct     = 150U, /**< H3 size = 150 % of body.       */
  k_ra_reflow_h4_scale_pct     = 125U, /**< H4 size = 125 % of body.       */
  k_ra_reflow_h5_scale_pct     = 110U, /**< H5 size = 110 % of body.       */
  k_ra_reflow_h6_scale_pct     = 100U, /**< H6 size = 100 % of body.       */
  k_ra_reflow_pct_full         = 100U, /**< Percentage denominator.        */
} ra_reflow_layout_t;

/**
 * @enum ra_reflow_html_tag_t
 * @brief Recognised HTML element kinds.
 *
 * @details
 * Tags outside this list are treated as transparent flow-through
 * containers (children still emit, the tag itself contributes no
 * styling).
 */
typedef enum : uint8_t {
  k_ra_reflow_tag_unknown    = 0U,  /**< Anything not in this enum.       */
  k_ra_reflow_tag_p          = 1U,  /**< Paragraph block.                 */
  k_ra_reflow_tag_h1         = 2U,  /**< Heading level 1.                 */
  k_ra_reflow_tag_h2         = 3U,  /**< Heading level 2.                 */
  k_ra_reflow_tag_h3         = 4U,  /**< Heading level 3.                 */
  k_ra_reflow_tag_h4         = 5U,  /**< Heading level 4.                 */
  k_ra_reflow_tag_h5         = 6U,  /**< Heading level 5.                 */
  k_ra_reflow_tag_h6         = 7U,  /**< Heading level 6.                 */
  k_ra_reflow_tag_em         = 8U,  /**< Italic emphasis (inline).        */
  k_ra_reflow_tag_strong     = 9U,  /**< Bold emphasis (inline).          */
  k_ra_reflow_tag_b          = 10U, /**< Bold (inline).                   */
  k_ra_reflow_tag_i          = 11U, /**< Italic (inline).                 */
  k_ra_reflow_tag_br         = 12U, /**< Line break (void).               */
  k_ra_reflow_tag_hr         = 13U, /**< Horizontal rule (void).          */
  k_ra_reflow_tag_ul         = 14U, /**< Unordered list block.            */
  k_ra_reflow_tag_ol         = 15U, /**< Ordered list block.              */
  k_ra_reflow_tag_li         = 16U, /**< List item.                       */
  k_ra_reflow_tag_blockquote = 17U, /**< Blockquote indent.               */
  k_ra_reflow_tag_a          = 18U, /**< Anchor (renders underlined).     */
  k_ra_reflow_tag_img        = 19U, /**< Image (placeholder rect).        */
} ra_reflow_html_tag_t;

/**
 * @enum ra_reflow_font_style_t
 * @brief Inline font-style flags (bitfield).
 *
 * @details
 * Used by both the parse pass (active style on the cursor) and the
 * render pass (per-glyph style stamp). v1 does not yet load separate
 * bold / italic TTFs; the flag is stored so the renderer can later
 * pick the right face without reflow.
 */
typedef enum : uint8_t {
  k_ra_reflow_style_normal    = 0U,       /**< No emphasis.            */
  k_ra_reflow_style_bold      = 1U << 0U, /**< Bold face.              */
  k_ra_reflow_style_italic    = 1U << 1U, /**< Italic face.            */
  k_ra_reflow_style_underline = 1U << 2U, /**< Underlined run.         */
} ra_reflow_font_style_t;

/**
 * @enum ra_reflow_token_kind_t
 * @brief Parsed token classification.
 *
 * @details
 * The parser converts the markup into a flat token stream. Block-start
 * and block-end tokens carry an `ra_reflow_html_tag_t`; inline tokens
 * carry text (a slice of the engine text pool) plus a font-style
 * stamp.
 */
typedef enum : uint8_t {
  k_ra_reflow_tok_block_start = 0U, /**< Open of a block-flow element. */
  k_ra_reflow_tok_block_end   = 1U, /**< Close of a block-flow element.*/
  k_ra_reflow_tok_text        = 2U, /**< Text run (slice into pool).   */
  k_ra_reflow_tok_break       = 3U, /**< Forced line break (`<br>`).   */
  k_ra_reflow_tok_rule        = 4U, /**< Horizontal rule (`<hr>`).     */
  k_ra_reflow_tok_image       = 5U, /**< Image placeholder (`<img>`).  */
} ra_reflow_token_kind_t;

/* ===========================================================================
 * Forward declarations (opaque payloads)
 * ===========================================================================
 */

/**
 * @struct ra_reflow_token_t
 * @brief One parsed token in the engine's token stream.
 *
 * @details
 * Internal-but-exposed so the engine handle can statically allocate an
 * array of these. Treat fields as read-only outside of `ra_reflow_*`.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  uint8_t kind; /**< `ra_reflow_token_kind_t`. */
  // cppcheck-suppress unusedStructMember
  uint8_t tag; /**< `ra_reflow_html_tag_t` (0 if N/A). */
  // cppcheck-suppress unusedStructMember
  uint8_t style; /**< Font-style bitmask.                 */
  // cppcheck-suppress unusedStructMember
  uint8_t reserved; /**< Padding to 4-byte align.            */
  // cppcheck-suppress unusedStructMember
  uint32_t text_off; /**< Byte offset into the text pool.     */
  // cppcheck-suppress unusedStructMember
  uint32_t text_len; /**< Byte length within the text pool.   */
} ra_reflow_token_t;

/**
 * @struct ra_reflow_glyph_t
 * @brief One positioned glyph after layout.
 *
 * @details
 * Stored per-page so render is a flat walk. `cp` is a Unicode code
 * point (only ASCII is exercised by v1); `font_px` lets headings and
 * body text mix on the same page without re-scanning style state.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  int32_t x; /**< Pixel column of glyph baseline-left. */
  // cppcheck-suppress unusedStructMember
  int32_t y; /**< Pixel row of glyph baseline.         */
  // cppcheck-suppress unusedStructMember
  int32_t cp; /**< Unicode code point.                  */
  // cppcheck-suppress unusedStructMember
  uint32_t color; /**< 32-bit RGB colour.                   */
  // cppcheck-suppress unusedStructMember
  uint16_t font_px; /**< Pixel size used for this glyph.      */
  // cppcheck-suppress unusedStructMember
  uint8_t style; /**< Font-style bitmask.                  */
  // cppcheck-suppress unusedStructMember
  uint8_t reserved; /**< Padding.                             */
} ra_reflow_glyph_t;

/**
 * @struct ra_reflow_page_t
 * @brief Index range of glyphs that belong to one page.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  uint32_t glyph_first; /**< Index of first glyph in this page. */
  // cppcheck-suppress unusedStructMember
  uint32_t glyph_count; /**< Number of glyphs in this page.     */
} ra_reflow_page_t;

/**
 * @struct ra_reflow_image_box_t
 * @brief One laid-out `<img>` rectangle (decoded + blitted at render time).
 *
 * @details
 * Produced by the layout pass when an image loader + decode arena are bound
 * (see ra_reflow_set_image_loader()). Coordinates are page-local -- the same
 * space as ra_reflow_glyph_t -- so render offsets them by the page origin.
 * The `src_off` / `src_len` slice indexes the engine text pool and is handed
 * back to the loader at render time to re-fetch the encoded bytes (the decoded
 * pixels are never stored -- decode is on-demand per page flip).
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  int32_t x; /**< Page-local left edge, pixels.       */
  // cppcheck-suppress unusedStructMember
  int32_t y; /**< Page-local top edge, pixels.        */
  // cppcheck-suppress unusedStructMember
  int32_t w; /**< Scaled box width, pixels.           */
  // cppcheck-suppress unusedStructMember
  int32_t h; /**< Scaled box height, pixels.          */
  // cppcheck-suppress unusedStructMember
  uint32_t src_off; /**< Href slice offset into text pool.   */
  // cppcheck-suppress unusedStructMember
  uint32_t src_len; /**< Href slice length, bytes.           */
  // cppcheck-suppress unusedStructMember
  uint32_t page_index; /**< Page this image belongs to.         */
  // cppcheck-suppress unusedStructMember
  uint32_t reserved; /**< Padding to 32-byte stride.          */
} ra_reflow_image_box_t;

/**
 * @brief Resolve an `<img src>` href to its encoded image bytes.
 *
 * @details Dependency-injection seam (NASA P10 Rule 9 deviation: function
 * pointer for testability / DIP). The consumer wires this to its EPUB resource
 * loader; the engine calls it at layout time (to probe intrinsic size) and at
 * render time (to fetch bytes for the decode). The returned buffer must remain
 * valid until the call returns and is only read, never freed, by the engine.
 *
 * @param[in]  ctx       Opaque context passed to ra_reflow_set_image_loader().
 * @param[in]  href      Image src string (not NUL-terminated).
 * @param[in]  href_len  Length of @p href, bytes.
 * @param[out] out_bytes Receives a pointer to the encoded image bytes.
 * @param[out] out_len   Receives the encoded byte count.
 *
 * @return ra_err_t; ::k_ra_ok on success, any error to skip the image.
 *
 * @note The engine treats any non-::k_ra_ok return as "image unavailable" and
 *       falls back to a placeholder advance.
 * @since 0.1.0
 */
typedef ra_err_t (*ra_reflow_image_loader_fn)(void*           ctx,
                                              const char*     href,
                                              uint32_t        href_len,
                                              const uint8_t** out_bytes,
                                              size_t*         out_len);

/* ===========================================================================
 * Engine handle
 * ===========================================================================
 */

/**
 * @struct ra_reflow_t
 * @brief Reflow / pagination engine state.
 *
 * @details
 * Allocated by the caller (typically as a `static` in the ereader app)
 * and bound via `ra_reflow_init()`. Treat all fields as read-only;
 * mutate only through the `ra_reflow_*` API.
 *
 * @invariant `in_use == 1` while initialized; cleared by `ra_reflow_close()`.
 *
 * @see ra_reflow_init()
 * @see ra_reflow_close()
 */
typedef struct {
  /* --- viewport + style ------------------------------------------------ */
  // cppcheck-suppress unusedStructMember
  uint16_t viewport_w; /**< Viewport width, pixels.             */
  // cppcheck-suppress unusedStructMember
  uint16_t viewport_h; /**< Viewport height, pixels.            */
  // cppcheck-suppress unusedStructMember
  uint16_t font_px; /**< Body font size in pixels.           */
  // cppcheck-suppress unusedStructMember
  uint16_t reserved16; /**< Padding.                            */
  // cppcheck-suppress unusedStructMember
  uint32_t body_color; /**< Body text colour (0xRRGGBB).        */
  // cppcheck-suppress unusedStructMember
  uint32_t link_color; /**< Anchor text colour (0xRRGGBB).      */

  /* --- font (caller-owned TTF blob) ------------------------------------ */
  // cppcheck-suppress unusedStructMember
  const uint8_t* font_data; /**< TTF blob; outlives the engine.   */
  // cppcheck-suppress unusedStructMember
  size_t font_len; /**< Length of `font_data`, bytes.    */

  /* --- cached chapter input ------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  const uint8_t* xhtml_buf; /**< Last `layout_chapter` input.     */
  // cppcheck-suppress unusedStructMember
  size_t xhtml_len; /**< Length of `xhtml_buf`.           */

  /* --- token stream --------------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  ra_reflow_token_t tokens[k_ra_reflow_max_tokens]; /**< Parsed tokens. */
  // cppcheck-suppress unusedStructMember
  uint32_t token_count; /**< Tokens used.   */
  // cppcheck-suppress unusedStructMember
  uint8_t text_pool[k_ra_reflow_text_pool_bytes]; /**< Text bytes. */
  // cppcheck-suppress unusedStructMember
  uint32_t text_pool_used; /**< Bytes consumed in text_pool.       */

  /* --- laid-out glyphs ------------------------------------------------ */
  // cppcheck-suppress unusedStructMember
  ra_reflow_glyph_t glyphs[k_ra_reflow_max_glyphs]; /**< Positioned glyphs. */
  // cppcheck-suppress unusedStructMember
  uint32_t glyph_count; /**< Glyphs used.                          */
  // cppcheck-suppress unusedStructMember
  ra_reflow_page_t pages[k_ra_reflow_max_pages]; /**< Page index ranges. */
  // cppcheck-suppress unusedStructMember
  uint32_t page_count; /**< Pages used.                           */

  /* --- image rendering (#106) ---------------------------------------- */
  // cppcheck-suppress unusedStructMember
  ra_reflow_image_loader_fn img_loader; /**< `<img>` byte loader (NULL = off).  */
  // cppcheck-suppress unusedStructMember
  void* img_loader_ctx; /**< Opaque context for `img_loader`.   */
  // cppcheck-suppress unusedStructMember
  ra_img_arena_t* img_arena; /**< Decode scratch (NULL = off).       */
  // cppcheck-suppress unusedStructMember
  ra_reflow_image_box_t image_boxes[k_ra_reflow_max_images]; /**< Laid-out images. */
  // cppcheck-suppress unusedStructMember
  uint32_t image_box_count; /**< Image boxes used.                  */

  /* --- lifecycle ------------------------------------------------------ */
  // cppcheck-suppress unusedStructMember
  uint8_t in_use; /**< 1 = initialized, 0 = closed. */
  // cppcheck-suppress unusedStructMember
  uint8_t reserved8[3]; /**< Padding.               */
} ra_reflow_t;

/* ===========================================================================
 * Public API -- lifecycle
 * ===========================================================================
 */

/**
 * @brief Initialise a reflow engine for a given viewport / font.
 *
 * @details
 * Records the viewport size, font handle and colour palette into the
 * engine handle and clears the cached chapter / glyph / page state.
 *
 * @param[in]  viewport_w  Viewport width, pixels (1..4096).
 * @param[in]  viewport_h  Viewport height, pixels (1..4096).
 * @param[in]  font_data   TTF blob; must outlive the engine.
 * @param[in]  font_len    Length of `font_data`, bytes (>= 16).
 * @param[in]  font_px     Initial font size, pixels
 *                         (`k_ra_reflow_min_font_px` ..
 *                          `k_ra_reflow_max_font_px`).
 * @param[in]  body_color  Body text colour (0xRRGGBB).
 * @param[in]  link_color  Anchor colour (0xRRGGBB).
 * @param[out] out_engine  Engine handle to populate.
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Initialized.
 * @retval k_ra_err_null_ptr        `font_data` or `out_engine` is NULL.
 * @retval k_ra_err_invalid_arg     Viewport or font size out of range.
 * @retval k_ra_err_invalid_size    `font_len` too small.
 *
 * @pre  `font_data` non-NULL, `out_engine` non-NULL.
 * @pre  Viewport and font size in their documented ranges.
 * @post On success, `out_engine->in_use == 1` and
 *       `out_engine->page_count == 0`.
 * @post On failure, `*out_engine` is zero-initialized.
 *
 * @note Not thread-safe. Single-threaded init context.
 *
 * @see ra_reflow_close()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_init(uint16_t       viewport_w,
                                      uint16_t       viewport_h,
                                      const uint8_t* font_data,
                                      size_t         font_len,
                                      uint16_t       font_px,
                                      uint32_t       body_color,
                                      uint32_t       link_color,
                                      ra_reflow_t*   out_engine);

/**
 * @brief Release a previously initialized engine.
 *
 * @param[in,out] engine Engine returned by `ra_reflow_init()`.
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Closed.
 * @retval k_ra_err_null_ptr        `engine` is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 *
 * @pre  `engine` non-NULL.
 * @pre  `engine->in_use == 1`.
 * @post `engine->in_use == 0`.
 * @post `engine->page_count == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_close(ra_reflow_t* engine);

/**
 * @brief Bind an `<img>` byte loader + decode arena to enable image rendering.
 *
 * @details
 * Without this binding (the default), `<img>` elements reserve a small
 * placeholder advance and draw nothing -- the historical v1 behaviour, kept so
 * image-free content lays out byte-identically. Once a loader + arena are
 * bound, the layout pass resolves each `<img src>` to its intrinsic size,
 * reserves a scaled block (text flows below), and the render pass decodes +
 * blits the image on demand. Pass @p loader == NULL or @p arena == NULL to
 * disable again.
 *
 * @param[in,out] engine Initialized engine handle.
 * @param[in]     loader Resolves an href to encoded image bytes (NULL = off).
 * @param[in]     ctx    Opaque context handed back to @p loader.
 * @param[in]     arena  Caller-owned decode scratch (NULL = off); sized for the
 *                       largest image (a few KiB SRAM .. a few MiB SDRAM).
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Binding recorded.
 * @retval k_ra_err_null_ptr        @p engine is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 *
 * @pre  @p engine is non-NULL and initialized.
 * @pre  If non-NULL, @p arena->base addresses @p arena->cap writable bytes.
 * @post On success the engine uses (@p loader, @p ctx, @p arena) for `<img>`.
 * @post Binding takes effect on the next ra_reflow_layout_chapter() / re-flow.
 *
 * @note Not thread-safe; bind before laying out.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_set_image_loader(ra_reflow_t*              engine,
                                                  ra_reflow_image_loader_fn loader,
                                                  void*                     ctx,
                                                  ra_img_arena_t*           arena);

/* ===========================================================================
 * Public API -- layout
 * ===========================================================================
 */

/**
 * @brief Parse + lay out one chapter of XHTML.
 *
 * @details
 * Walks the XHTML once with the no-heap streaming tokenizer, emits a token stream, then runs
 * the greedy line-break + page-break engine to produce a flat list of
 * positioned glyphs grouped by page. The engine caches the input
 * buffer so `ra_reflow_set_font_size()` can re-flow without the caller
 * re-supplying it.
 *
 * Algorithm summary:
 *   1. Tokenize the XHTML in a single no-heap forward pass, emitting
 *      tokens (`block_start` / `text` / `break` / ...).
 *   2. For each token, if it is a text run, walk word-by-word.
 *   3. Measure `word_width = sum(stbtt advance per glyph)`.
 *   4. If `cursor_x + word_width > viewport_w - margins` then break
 *      to a new line.
 *   5. If `cursor_y + line_height > viewport_h - margins` then start
 *      a new page.
 *   6. After all tokens, record `page_count`.
 *
 * @param[in,out] engine          Initialized engine handle.
 * @param[in]     xhtml_buf       UTF-8 / ASCII XHTML source bytes.
 * @param[in]     xhtml_len       Length of `xhtml_buf`, bytes (>0).
 * @param[out]    out_total_pages Total page count (>= 1 on success).
 *
 * @return ra_err_t
 * @retval k_ra_ok                    Laid out.
 * @retval k_ra_err_null_ptr          Any required pointer is NULL.
 * @retval k_ra_err_not_initialized   `engine->in_use == 0`.
 * @retval k_ra_err_invalid_size      `xhtml_len == 0`.
 * @retval k_ra_err_validation_failed XHTML did not parse.
 * @retval k_ra_err_no_mem            Token / glyph / page pool full.
 *
 * @pre  `engine`, `xhtml_buf`, `out_total_pages` non-NULL.
 * @pre  `engine->in_use == 1`.
 * @post On success, `*out_total_pages == engine->page_count >= 1`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_layout_chapter(ra_reflow_t*   engine,
                                                const uint8_t* xhtml_buf,
                                                size_t         xhtml_len,
                                                uint32_t*      out_total_pages);

/**
 * @brief Render one page into the active `ra_gfx` framebuffer.
 *
 * @details
 * Walks the slice of `engine->glyphs[]` that belongs to `page_idx`,
 * rasterises each code point through the two-step
 * `stbtt_GetCodepointBitmapBox()` + `stbtt_MakeCodepointBitmap()` path
 * (glyph bitmap into a fixed buffer; stb scratch via the no-heap arena in
 * `ra_stbtt_alloc.c`), and blits the alpha-8 mask into the framebuffer
 * with `ra_gfx_pixel()`.
 *
 * The framebuffer must already be bound by `ra_gfx_init()`. The
 * background is NOT cleared; the caller chooses the background colour
 * with a prior `ra_gfx_clear()`.
 *
 * @param[in]     engine      Laid-out engine.
 * @param[in]     page_idx    Page to render (`[0, page_count)`).
 * @param[in,out] framebuffer Reserved for future use; pass NULL while
 *                            ra_gfx is bound. (Forward-compat hook so
 *                            future builds can blit straight into a
 *                            non-active buffer without re-binding
 *                            ra_gfx.)
 *
 * @return ra_err_t
 * @retval k_ra_ok                    Rendered.
 * @retval k_ra_err_null_ptr          `engine` is NULL.
 * @retval k_ra_err_not_initialized   `engine->in_use == 0`.
 * @retval k_ra_err_out_of_range      `page_idx >= page_count`.
 * @retval k_ra_err_validation_failed Font blob malformed.
 *
 * @pre  `ra_gfx_init()` has been called.
 * @pre  A chapter has been laid out.
 * @post Glyph pixels of the requested page are blitted into the bound
 *       framebuffer.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_reflow_render_page(const ra_reflow_t* engine, uint32_t page_idx, void* framebuffer);

/**
 * @brief Render one page offset by `(origin_x, origin_y)` into the bound
 *        framebuffer.
 *
 * @details
 * Identical to `ra_reflow_render_page()` except every glyph (and link
 * underline) is shifted by the given origin before it is blitted with
 * `ra_gfx_pixel()`. This lets the engine paint into a sub-region of a
 * larger panel -- e.g. an e-reader Reading body inset below a status bar
 * and above a footer -- without the layout knowing about the chrome:
 * lay out against the body's `viewport_w`/`viewport_h`, then render at
 * the body's top-left. `ra_reflow_render_page()` is exactly this with a
 * `(0, 0)` origin. ra_gfx owns the framebuffer stride, so only an origin
 * (not a stride) is required.
 *
 * @param[in] engine   Laid-out engine.
 * @param[in] page_idx Page to render (`[0, page_count)`).
 * @param[in] origin_x Pixel offset added to every glyph's x coordinate.
 * @param[in] origin_y Pixel offset added to every glyph's y coordinate.
 *
 * @return ra_err_t
 * @retval k_ra_ok                    Rendered.
 * @retval k_ra_err_null_ptr          `engine` is NULL.
 * @retval k_ra_err_not_initialized   `engine->in_use == 0`.
 * @retval k_ra_err_out_of_range      `page_idx >= page_count`.
 * @retval k_ra_err_validation_failed Font blob malformed.
 *
 * @pre  `ra_gfx_init()` has been called and a chapter laid out.
 * @pre  The offset region lies within the bound framebuffer.
 * @post Glyph pixels of the requested page are blitted at the offset.
 * @post Pixels that fall outside the framebuffer are dropped by ra_gfx.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_render_page_at(const ra_reflow_t* engine,
                                                uint32_t           page_idx,
                                                int32_t            origin_x,
                                                int32_t            origin_y);

/**
 * @brief Report the laid-out page count.
 *
 * @param[in]  engine    Laid-out engine.
 * @param[out] out_count Page count (0 if no chapter laid out yet).
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Reported.
 * @retval k_ra_err_null_ptr        Any pointer is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_get_page_count(const ra_reflow_t* engine, uint32_t* out_count);

/**
 * @brief Change the body font size and re-flow the cached chapter.
 *
 * @details
 * Re-runs the line-break + page-break engine against the most recent
 * chapter handed to `ra_reflow_layout_chapter()`. Glyph and page state
 * are rebuilt from scratch; the previous page count and page-glyph
 * ranges are invalidated.
 *
 * @param[in,out] engine       Initialized + laid-out engine.
 * @param[in]     new_font_px  New body font size, pixels
 *                             (`k_ra_reflow_min_font_px` ..
 *                              `k_ra_reflow_max_font_px`).
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Re-flowed.
 * @retval k_ra_err_null_ptr        `engine` is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 * @retval k_ra_err_invalid_state   No chapter cached yet.
 * @retval k_ra_err_invalid_arg     `new_font_px` out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_set_font_size(ra_reflow_t* engine, uint16_t new_font_px);

/* ===========================================================================
 * Internal -- exposed for the parse / layout / render TUs
 * ===========================================================================
 */

/**
 * @brief Parse the XHTML buffer into the engine's token stream.
 *
 * @details
 * Internal helper used by `ra_reflow_layout_chapter()`. Implemented by the
 * no-heap streaming tokenizer in `ra_reflow_tokenize.c`. Declared here so
 * `ra_reflow_layout.c` can call it without a forward declaration.
 *
 * @param[in,out] engine     Engine whose token / text pools will be
 *                           populated.
 * @param[in]     xhtml_buf  XHTML source bytes.
 * @param[in]     xhtml_len  Length of `xhtml_buf`.
 *
 * @return ra_err_t
 * @retval k_ra_ok                    Tokens emitted.
 * @retval k_ra_err_validation_failed XHTML did not parse.
 * @retval k_ra_err_no_mem            Token or text pool full.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_reflow_parse_xhtml(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

/**
 * @brief Run the line-break + page-break pass over `engine->tokens[]`.
 *
 * @details
 * Internal helper. Consumes the token stream populated by
 * `ra_reflow_parse_xhtml()` and writes positioned glyphs into
 * `engine->glyphs[]` plus page index ranges into `engine->pages[]`.
 *
 * @param[in,out] engine Engine in `in_use == 1` state with a populated
 *                       token stream.
 *
 * @return ra_err_t
 * @retval k_ra_ok       Layout complete.
 * @retval k_ra_err_no_mem Glyph or page pool full.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_run_layout(ra_reflow_t* engine);

#ifdef __cplusplus
}
#endif
