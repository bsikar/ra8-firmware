/**
 * @file ra8_reflow_types.h
 * @brief Reflow-engine data model: enums, structs, typedefs, and the handle.
 * @ingroup grp_ereader
 *
 * @details
 * This sub-header holds the data model for the `ra8_reflow` HTML / CSS reflow
 * + pagination engine: the compile-time limit enums, the parsed-token and
 * positioned-glyph structs, the dependency-injection loader function-pointer
 * typedefs, and the engine handle (::ra8_reflow_t) itself. It is split out of
 * the umbrella `ra8_reflow.h` so that header stays small; consumers still
 * include `ra8_reflow.h` and never reference this file directly.
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

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_glyph_atlas.h"  /* ra8_glyph_atlas_t for the Layer-3 glyph cache (#164) */
#include "ra8_reflow_css.h"   /* ra8_css_sheet_t for the content-CSS cascade (#111)   */
#include "ra8_reflow_image.h" /* ra8_img_arena_t for the decode scratch               */

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra8_reflow_limits_t
 * @brief Static-allocation caps for the reflow engine.
 *
 * @details
 * The engine holds a fixed number of parsed tokens, positioned glyphs,
 * and pages. Anything bigger is rejected with `k_ra8_err_no_mem`. Sized
 * to fit the EK-RA8D2's 2 MB SRAM with comfortable headroom.
 */
typedef enum : uint32_t {
  k_ra8_reflow_max_tokens         = 4096U,  /**< Max parsed token count.              */
  k_ra8_reflow_text_pool_bytes    = 65536U, /**< Bytes of text pool.                  */
  k_ra8_reflow_max_glyphs         = 32768U, /**< Total positioned glyphs.             */
  k_ra8_reflow_max_pages          = 256U,   /**< Max paginated pages.                 */
  k_ra8_reflow_max_lines_per_page = 96U,    /**< Lines per page upper bound.          */
  k_ra8_reflow_max_images         = 64U,    /**< Max laid-out `<img>` boxes.          */
  k_ra8_reflow_max_links          = 255U,   /**< Max distinct `<a href>` per chapter. */
  k_ra8_reflow_max_link_rects     = 512U,   /**< Max positioned link rectangles.      */
  k_ra8_reflow_max_anchors        = 256U,   /**< Max `id=` anchor positions.          */
  k_ra8_reflow_max_faces          = 8U,     /**< Max embedded `@font-face` typefaces. */
} ra8_reflow_limits_t;

/**
 * @enum ra8_reflow_layout_t
 * @brief Layout-axis named constants used by the line-break engine.
 *
 * @details
 * These values name the magic numbers that would otherwise show up in
 * the layout pass: page margin, line-spacing multiplier numerator /
 * denominator, default font size, and the heading multipliers H1..H6.
 */
typedef enum : uint16_t {
  k_ra8_reflow_margin_px        = 16U,  /**< Page edge inset, pixels.        */
  k_ra8_reflow_line_spacing_num = 12U,  /**< Line height numerator (= 1.2x). */
  k_ra8_reflow_line_spacing_den = 10U,  /**< Line height denominator.        */
  k_ra8_reflow_paragraph_gap_px = 8U,   /**< Vertical gap after a block.     */
  k_ra8_reflow_indent_px        = 24U,  /**< Indent for blockquote / li.     */
  k_ra8_reflow_default_font_px  = 18U,  /**< Default body font size.         */
  k_ra8_reflow_min_font_px      = 8U,   /**< Smallest accepted font size.    */
  k_ra8_reflow_max_font_px      = 96U,  /**< Largest accepted font size.     */
  k_ra8_reflow_min_font_bytes   = 16U,  /**< Smallest plausible font blob.   */
  k_ra8_reflow_h1_scale_pct     = 200U, /**< H1 size = 200 % of body.        */
  k_ra8_reflow_h2_scale_pct     = 175U, /**< H2 size = 175 % of body.        */
  k_ra8_reflow_h3_scale_pct     = 150U, /**< H3 size = 150 % of body.        */
  k_ra8_reflow_h4_scale_pct     = 125U, /**< H4 size = 125 % of body.        */
  k_ra8_reflow_h5_scale_pct     = 110U, /**< H5 size = 110 % of body.        */
  k_ra8_reflow_h6_scale_pct     = 100U, /**< H6 size = 100 % of body.        */
  k_ra8_reflow_pct_full         = 100U, /**< Percentage denominator.         */
} ra8_reflow_layout_t;

/**
 * @enum ra8_reflow_html_tag_t
 * @brief Recognised HTML element kinds.
 *
 * @details
 * Tags outside this list are treated as transparent flow-through
 * containers (children still emit, the tag itself contributes no
 * styling).
 */
typedef enum : uint8_t {
  k_ra8_reflow_tag_unknown    = 0U,  /**< Anything not in this enum.       */
  k_ra8_reflow_tag_p          = 1U,  /**< Paragraph block.                 */
  k_ra8_reflow_tag_h1         = 2U,  /**< Heading level 1.                 */
  k_ra8_reflow_tag_h2         = 3U,  /**< Heading level 2.                 */
  k_ra8_reflow_tag_h3         = 4U,  /**< Heading level 3.                 */
  k_ra8_reflow_tag_h4         = 5U,  /**< Heading level 4.                 */
  k_ra8_reflow_tag_h5         = 6U,  /**< Heading level 5.                 */
  k_ra8_reflow_tag_h6         = 7U,  /**< Heading level 6.                 */
  k_ra8_reflow_tag_em         = 8U,  /**< Italic emphasis (inline).        */
  k_ra8_reflow_tag_strong     = 9U,  /**< Bold emphasis (inline).          */
  k_ra8_reflow_tag_b          = 10U, /**< Bold (inline).                   */
  k_ra8_reflow_tag_i          = 11U, /**< Italic (inline).                 */
  k_ra8_reflow_tag_br         = 12U, /**< Line break (void).               */
  k_ra8_reflow_tag_hr         = 13U, /**< Horizontal rule (void).          */
  k_ra8_reflow_tag_ul         = 14U, /**< Unordered list block.            */
  k_ra8_reflow_tag_ol         = 15U, /**< Ordered list block.              */
  k_ra8_reflow_tag_li         = 16U, /**< List item.                       */
  k_ra8_reflow_tag_blockquote = 17U, /**< Blockquote indent.               */
  k_ra8_reflow_tag_a          = 18U, /**< Anchor (renders underlined).     */
  k_ra8_reflow_tag_img        = 19U, /**< Image (placeholder rect).        */
  k_ra8_reflow_tag_table      = 20U, /**< Table (grid layout).             */
  k_ra8_reflow_tag_tr         = 21U, /**< Table row.                       */
  k_ra8_reflow_tag_td         = 22U, /**< Table cell.                      */
  k_ra8_reflow_tag_th         = 23U, /**< Table header cell.               */
  k_ra8_reflow_tag_link       = 24U, /**< `<link>` (void; stylesheet ref). */
} ra8_reflow_html_tag_t;

/**
 * @enum ra8_reflow_font_style_t
 * @brief Inline font-style flags (bitfield).
 *
 * @details
 * Used by both the parse pass (active style on the cursor) and the
 * render pass (per-glyph style stamp). v1 does not yet load separate
 * bold / italic TTFs; the flag is stored so the renderer can later
 * pick the right face without reflow.
 */
typedef enum : uint8_t {
  k_ra8_reflow_style_normal    = 0U,       /**< No emphasis.    */
  k_ra8_reflow_style_bold      = 1U << 0U, /**< Bold face.      */
  k_ra8_reflow_style_italic    = 1U << 1U, /**< Italic face.    */
  k_ra8_reflow_style_underline = 1U << 2U, /**< Underlined run. */
} ra8_reflow_font_style_t;

/**
 * @enum ra8_reflow_face_pack_t
 * @brief How a per-run embedded-face index is packed into a glyph/token `style`.
 *
 * @details The emphasis bits (bold/italic/underline) occupy bits 0-2 of the
 * `style` byte; bits 4-7 are free, so the selected embedded-face index
 * (0 = the engine's bound default face, 1..::k_ra8_reflow_max_faces = a
 * registered `@font-face`) rides in the high nibble (#109). Single-face content
 * registers no faces, so the nibble is always 0 -- the cached / default render
 * path is byte-identical.
 */
typedef enum : uint8_t {
  k_ra8_reflow_face_shift = 4U,    /**< Bit position of the face index in `style`. */
  k_ra8_reflow_face_mask  = 0x0FU, /**< Face-index mask once shifted down.         */
} ra8_reflow_face_pack_t;

/**
 * @enum ra8_reflow_align_t
 * @brief Block text alignment (from an inline `style="text-align:..."`).
 *
 * @details Carried on a block-start token (its `reserved` byte) and applied as
 * a per-line pass: centre/right shift every glyph on the line; justify spreads
 * the slack across inter-word gaps on full (wrapped) lines, leaving the last
 * line of a paragraph left-aligned. The default is left (identical to the
 * historical greedy layout, so unaligned content is byte-stable).
 */
typedef enum : uint8_t {
  k_ra8_reflow_align_left    = 0U, /**< Left (default, ragged right).       */
  k_ra8_reflow_align_right   = 1U, /**< Right-aligned.                      */
  k_ra8_reflow_align_center  = 2U, /**< Centred.                            */
  k_ra8_reflow_align_justify = 3U, /**< Justified (last line left-aligned). */
} ra8_reflow_align_t;

/**
 * @enum ra8_reflow_token_kind_t
 * @brief Parsed token classification.
 *
 * @details
 * The parser converts the markup into a flat token stream. Block-start
 * and block-end tokens carry an `ra8_reflow_html_tag_t`; inline tokens
 * carry text (a slice of the engine text pool) plus a font-style
 * stamp.
 */
typedef enum : uint8_t {
  k_ra8_reflow_tok_block_start = 0U, /**< Open of a block-flow element.  */
  k_ra8_reflow_tok_block_end   = 1U, /**< Close of a block-flow element. */
  k_ra8_reflow_tok_text        = 2U, /**< Text run (slice into pool).    */
  k_ra8_reflow_tok_break       = 3U, /**< Forced line break (`<br>`).    */
  k_ra8_reflow_tok_rule        = 4U, /**< Horizontal rule (`<hr>`).      */
  k_ra8_reflow_tok_image       = 5U, /**< Image placeholder (`<img>`).   */
} ra8_reflow_token_kind_t;

/* ===========================================================================
 * Forward declarations (opaque payloads)
 * ===========================================================================
 */

/**
 * @enum ra8_reflow_color_t
 * @brief Sentinel marking a token / run with no CSS colour.
 *
 * @details A text token's `color` field holds a 0xRRGGBB CSS colour (#111 /
 * #140) or this sentinel, in which case the renderer falls back to the engine
 * body colour (or link colour for an `<a>` run). 0xFFFFFFFF is outside the
 * 24-bit RGB range, so it can never collide with a real colour.
 */
typedef enum : uint32_t {
  k_ra8_reflow_color_inherit = 0xFFFFFFFFU, /**< No per-run CSS colour. */
} ra8_reflow_color_t;

/**
 * @struct ra8_reflow_token_t
 * @brief One parsed token in the engine's token stream.
 *
 * @details
 * Internal-but-exposed so the engine handle can statically allocate an
 * array of these. Treat fields as read-only outside of `ra8_reflow_*`.
 */
typedef struct {
  uint8_t  kind;        /**< `ra8_reflow_token_kind_t`.                          */
  uint8_t  tag;         /**< `ra8_reflow_html_tag_t` (0 if N/A).                 */
  uint8_t  style;       /**< Font-style bitmask.                                 */
  uint8_t  reserved;    /**< Block align / `<a>` link id.                        */
  uint32_t text_off;    /**< Byte offset into the text pool.                     */
  uint32_t text_len;    /**< Byte length within the text pool.                   */
  uint32_t color;       /**< 0xRRGGBB CSS colour, or k_ra8_reflow_color_inherit. */
  uint16_t css_font_px; /**< Block-start CSS font px (0 = none -> UA default).   */
  uint16_t reserved16;  /**< Padding to 4-byte stride.                           */
} ra8_reflow_token_t;

/**
 * @struct ra8_reflow_glyph_t
 * @brief One positioned glyph after layout.
 *
 * @details
 * Stored per-page so render is a flat walk. `cp` is a Unicode code
 * point (only ASCII is exercised by v1); `font_px` lets headings and
 * body text mix on the same page without re-scanning style state.
 */
typedef struct {
  int32_t  x;        /**< Pixel column of glyph baseline-left. */
  int32_t  y;        /**< Pixel row of glyph baseline.         */
  int32_t  cp;       /**< Unicode code point.                  */
  uint32_t color;    /**< 32-bit RGB colour.                   */
  uint16_t font_px;  /**< Pixel size used for this glyph.      */
  uint8_t  style;    /**< Font-style bitmask.                  */
  uint8_t  reserved; /**< Padding.                             */
} ra8_reflow_glyph_t;

/**
 * @struct ra8_reflow_page_t
 * @brief Index range of glyphs that belong to one page.
 */
typedef struct {
  uint32_t glyph_first; /**< Index of first glyph in this page. */
  uint32_t glyph_count; /**< Number of glyphs in this page.     */
} ra8_reflow_page_t;

/**
 * @struct ra8_reflow_image_box_t
 * @brief One laid-out `<img>` rectangle (decoded + blitted at render time).
 *
 * @details
 * Produced by the layout pass when an image loader + decode arena are bound
 * (see ra8_reflow_set_image_loader()). Coordinates are page-local -- the same
 * space as ra8_reflow_glyph_t -- so render offsets them by the page origin.
 * The `src_off` / `src_len` slice indexes the engine text pool and is handed
 * back to the loader at render time to re-fetch the encoded bytes (the decoded
 * pixels are never stored -- decode is on-demand per page flip).
 */
typedef struct {
  int32_t  x;          /**< Page-local left edge, pixels.     */
  int32_t  y;          /**< Page-local top edge, pixels.      */
  int32_t  w;          /**< Scaled box width, pixels.         */
  int32_t  h;          /**< Scaled box height, pixels.        */
  uint32_t src_off;    /**< Href slice offset into text pool. */
  uint32_t src_len;    /**< Href slice length, bytes.         */
  uint32_t page_index; /**< Page this image belongs to.       */
  uint32_t reserved;   /**< Padding to 32-byte stride.        */
} ra8_reflow_image_box_t;

/**
 * @brief Resolve an `<img src>` href to its encoded image bytes.
 *
 * @details Dependency-injection seam (NASA P10 Rule 9 deviation: function
 * pointer for testability / DIP). The consumer wires this to its EPUB resource
 * loader; the engine calls it at layout time (to probe intrinsic size) and at
 * render time (to fetch bytes for the decode). The returned buffer must remain
 * valid until the call returns and is only read, never freed, by the engine.
 *
 * @param[in]  ctx       Opaque context passed to ra8_reflow_set_image_loader().
 * @param[in]  href      Image src string (not NUL-terminated).
 * @param[in]  href_len  Length of @p href, bytes.
 * @param[out] out_bytes Receives a pointer to the encoded image bytes.
 * @param[out] out_len   Receives the encoded byte count.
 *
 * @return ra8_err_t; ::k_ra8_ok on success, any error to skip the image.
 *
 * @note The engine treats any non-::k_ra8_ok return as "image unavailable" and
 *       falls back to a placeholder advance.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_reflow_image_loader_fn)(void*           ctx,
                                                const char*     href,
                                                uint32_t        href_len,
                                                const uint8_t** out_bytes,
                                                size_t*         out_len);

/**
 * @brief External-stylesheet byte loader for `<link rel="stylesheet" href>`.
 *
 * @details Dependency-injection seam (NASA P10 Rule 9 deviation: function
 * pointer for testability / DIP), mirroring ::ra8_reflow_image_loader_fn. The
 * consumer wires this to its EPUB resource loader; the engine calls it while
 * tokenizing a chapter, when a `<link rel="stylesheet" href="X">` is seen in
 * document order, and parses the returned CSS into the chapter's sheet at that
 * position (so a later `<style>` block / inline `style=` correctly override it).
 * The returned buffer is only read, never freed, by the engine and need only
 * stay valid until the call returns.
 *
 * @param[in]  ctx       Opaque context passed to ra8_reflow_set_css_loader().
 * @param[in]  href      Stylesheet href string (not NUL-terminated).
 * @param[in]  href_len  Length of @p href, bytes.
 * @param[out] out_bytes Receives a pointer to the CSS text bytes.
 * @param[out] out_len   Receives the CSS byte count.
 *
 * @return ra8_err_t; ::k_ra8_ok on success, any error to skip the stylesheet.
 *
 * @note The engine treats any non-::k_ra8_ok return as "stylesheet unavailable"
 *       and continues with only the inline rules.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_reflow_css_loader_fn)(void*           ctx,
                                              const char*     href,
                                              uint32_t        href_len,
                                              const uint8_t** out_bytes,
                                              size_t*         out_len);

/**
 * @struct ra8_reflow_link_target_t
 * @brief One distinct `<a href>` destination interned during a parse.
 *
 * @details The href string is stored as a slice into the engine text pool.
 * Multiple positioned rectangles (::ra8_reflow_link_rect_t) can reference the
 * same target by index (a wrapped link spans several rects).
 */
typedef struct {
  uint32_t href_off; /**< Href slice offset into the text pool. */
  uint32_t href_len; /**< Href slice length, bytes.             */
} ra8_reflow_link_target_t;

/**
 * @struct ra8_reflow_link_rect_t
 * @brief One tappable rectangle covering a run of `<a>` glyphs on a page.
 *
 * @details Page-local coordinates (the same space as ra8_reflow_glyph_t). A link
 * that wraps across lines produces one rect per line segment, all pointing at
 * the same `target` index in `engine->link_targets[]`.
 */
typedef struct {
  int32_t  x;          /**< Page-local left edge, pixels.        */
  int32_t  y;          /**< Page-local top edge, pixels.         */
  int32_t  w;          /**< Rect width, pixels.                  */
  int32_t  h;          /**< Rect height, pixels.                 */
  uint32_t target;     /**< Index into `engine->link_targets[]`. */
  uint32_t page_index; /**< Page this rect belongs to.           */
} ra8_reflow_link_rect_t;

/**
 * @struct ra8_reflow_anchor_t
 * @brief A laid-out element `id`, for same-chapter `#fragment` jumps.
 *
 * @details Records where a block element carrying `id="..."` landed, so a
 * fragment link can navigate to its page. The id string is a text-pool slice.
 */
typedef struct {
  uint32_t id_off;     /**< Id slice offset into the text pool. */
  uint32_t id_len;     /**< Id slice length, bytes.             */
  uint32_t page_index; /**< Page the id landed on.              */
  int32_t  y;          /**< Page-local top y of the element.    */
} ra8_reflow_anchor_t;

/**
 * @enum ra8_reflow_href_kind_t
 * @brief Classification of an in-content `<a href>` target.
 */
typedef enum : uint8_t {
  k_ra8_reflow_href_empty            = 0U, /**< Empty / whitespace-only href.       */
  k_ra8_reflow_href_fragment         = 1U, /**< "#id" -- same-chapter anchor.       */
  k_ra8_reflow_href_chapter          = 2U, /**< "path" -- another chapter, no frag. */
  k_ra8_reflow_href_chapter_fragment = 3U, /**< "path#id" -- chapter + anchor.      */
  k_ra8_reflow_href_external         = 4U, /**< Has a URI scheme (http:, mailto:).  */
} ra8_reflow_href_kind_t;

/* ===========================================================================
 * Engine handle
 * ===========================================================================
 */

/**
 * @enum ra8_reflow_face_pad_t
 * @brief Padding geometry for ::ra8_reflow_face_t.
 *
 * @details Names the trailing-pad byte count that rounds ::ra8_reflow_face_t up
 * to an 8-byte stride (one pointer, one `size_t`, one `uint8_t` index, then
 * `k_ra8_reflow_face_pad8` bytes of padding).
 *
 * @see ra8_reflow_face_t
 */
typedef enum : uint8_t {
  k_ra8_reflow_face_pad8 = 7, /**< Pad bytes to reach an 8-byte struct stride. */
} ra8_reflow_face_pad_t;

/**
 * @struct ra8_reflow_face_t
 * @brief One registered embedded `@font-face` typeface (#109).
 *
 * @details Maps a parsed `@font-face` table entry (`css_face_idx`, the value
 * ::ra8_css_match_face returns) to its caller-owned TTF/OTF bytes. The blob
 * outlives the engine (it points into the resident EPUB buffer -- zero-copy).
 * The engine holds up to ::k_ra8_reflow_max_faces of these; the render pass
 * builds one `stbtt_fontinfo` per registered face plus the default at index 0.
 *
 * @see ra8_reflow_register_face()
 */
typedef struct {
  const uint8_t* blob;         /**< TTF/OTF bytes; caller-owned, outlive the engine. */
  size_t         len;          /**< Length of `blob`, bytes.                         */
  uint8_t        css_face_idx; /**< `@font-face` table index this blob satisfies.    */
  uint8_t        pad8[k_ra8_reflow_face_pad8]; /**< Padding to an 8-byte stride. */
} ra8_reflow_face_t;

/**
 * @struct ra8_reflow_t
 * @brief Reflow / pagination engine state.
 *
 * @details
 * Allocated by the caller (typically as a `static` in the ereader app)
 * and bound via `ra8_reflow_init()`. Treat all fields as read-only;
 * mutate only through the `ra8_reflow_*` API.
 *
 * @invariant `in_use == 1` while initialized; cleared by `ra8_reflow_close()`.
 *
 * @see ra8_reflow_init()
 * @see ra8_reflow_close()
 */
typedef struct {
  /* --- viewport + style ------------------------------------------------ */
  uint16_t viewport_w; /**< Viewport width, pixels.        */
  uint16_t viewport_h; /**< Viewport height, pixels.       */
  uint16_t font_px;    /**< Body font size in pixels.      */
  uint16_t reserved16; /**< Padding.                       */
  uint32_t body_color; /**< Body text colour (0xRRGGBB).   */
  uint32_t link_color; /**< Anchor text colour (0xRRGGBB). */

  /* --- font (caller-owned TTF blob) ------------------------------------ */
  const uint8_t*    font_data;                     /**< TTF blob; outlives the engine. */
  size_t            font_len;                      /**< Length of `font_data`, bytes.  */
  ra8_reflow_face_t faces[k_ra8_reflow_max_faces]; /**< Embedded `@font-face` set.     */
  uint8_t           face_count; /**< Registered embedded faces (0 = single-face). */

  /* --- cached chapter input ------------------------------------------- */
  const uint8_t* xhtml_buf; /**< Last `layout_chapter` input. */
  size_t         xhtml_len; /**< Length of `xhtml_buf`.       */

  /* --- token stream --------------------------------------------------- */
  ra8_reflow_token_t tokens[k_ra8_reflow_max_tokens];         /**< Parsed tokens.               */
  uint32_t           token_count;                             /**< Tokens used.                 */
  uint8_t            text_pool[k_ra8_reflow_text_pool_bytes]; /**< Text bytes.                  */
  uint32_t           text_pool_used;                          /**< Bytes consumed in text_pool. */

  /* --- laid-out glyphs ------------------------------------------------ */
  ra8_reflow_glyph_t glyphs[k_ra8_reflow_max_glyphs]; /**< Positioned glyphs. */
  uint32_t           glyph_count;                     /**< Glyphs used.       */
  ra8_reflow_page_t  pages[k_ra8_reflow_max_pages];   /**< Page index ranges. */
  uint32_t           page_count;                      /**< Pages used.        */

  /* --- image rendering (#106) ---------------------------------------- */
  ra8_reflow_image_loader_fn img_loader;     /**< `<img>` byte loader (NULL = off). */
  void*                      img_loader_ctx; /**< Opaque context for `img_loader`.  */
  ra8_img_arena_t*           img_arena;      /**< Decode scratch (NULL = off).      */
  ra8_reflow_css_loader_fn   css_loader;     /**< `<link>` CSS loader (NULL = off). */
  void*                      css_loader_ctx; /**< Opaque context for `css_loader`.  */
  ra8_reflow_image_box_t     image_boxes[k_ra8_reflow_max_images]; /**< Laid-out images.  */
  uint32_t                   image_box_count;                      /**< Image boxes used. */

  /* --- hyperlinks + anchors (#110) ----------------------------------- */
  ra8_reflow_link_target_t link_targets[k_ra8_reflow_max_links];    /**< Distinct hrefs.        */
  uint32_t                 link_target_count;                       /**< Interned link targets. */
  ra8_reflow_link_rect_t   link_rects[k_ra8_reflow_max_link_rects]; /**< Tappable rects.        */
  uint32_t                 link_rect_count;                   /**< Positioned link rects used. */
  ra8_reflow_anchor_t      anchors[k_ra8_reflow_max_anchors]; /**< id= anchor positions.       */
  uint32_t                 anchor_count;                      /**< Anchor positions used.      */

  /* --- content CSS cascade (#111) ------------------------------------ */
  ra8_css_sheet_t css; /**< Parsed `<style>` rules for the chapter. */

  /* --- glyph atlas (Layer-3 cache, #164) ----------------------------- */
  ra8_glyph_atlas_t* glyph_atlas; /**< Glyph bitmap cache, or NULL for direct raster. */

  /* --- lifecycle ------------------------------------------------------ */
  uint8_t in_use;       /**< 1 = initialized, 0 = closed. */
  uint8_t reserved8[3]; /**< Padding.                     */
} ra8_reflow_t;
