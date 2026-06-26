/**
 * @file ra_reflow_api.h
 * @brief Reflow-engine public + internal function prototypes.
 *
 * @details
 * This sub-header holds the `ra_reflow` engine's callable surface: the
 * lifecycle entry points (`ra_reflow_init` / `_close`), the loader-binding
 * setters, the hit-test / anchor / href helpers, the layout + render passes,
 * and the parse / layout internals exposed to the engine's TUs. It is split
 * out of the umbrella `ra_reflow.h` so that header stays small; consumers
 * still include `ra_reflow.h` and never reference this file directly. The
 * data model these functions operate on lives in `ra_reflow_types.h`.
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

#include "ra_err.h"
#include "ra_reflow_image.h" /* ra_img_arena_t for the decode scratch */
#include "ra_reflow_types.h" /* ra_reflow_t + supporting data model   */

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * @brief Bind the external-stylesheet loader for `<link rel="stylesheet">`.
 *
 * @details Mirrors ra_reflow_set_image_loader(). When bound, ra_reflow_layout_chapter()
 * resolves each `<link rel="stylesheet" href>` in the chapter via @p loader and
 * parses the returned CSS into the chapter sheet in document order (so a later
 * inline `<style>` / `style=` overrides it). NULL @p loader disables the feature
 * (chapters parse only their inline CSS, exactly as before).
 *
 * @param[in,out] engine Engine to bind.
 * @param[in]     loader Stylesheet byte loader, or NULL to disable.
 * @param[in]     ctx    Opaque context handed back to @p loader.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Binding recorded.
 * @retval k_ra_err_null_ptr        @p engine is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 *
 * @pre  @p engine is non-NULL and initialized.
 * @post On success the engine uses (@p loader, @p ctx) for `<link>` stylesheets.
 * @post Binding takes effect on the next ra_reflow_layout_chapter().
 *
 * @note Not thread-safe; bind before laying out.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_reflow_set_css_loader(ra_reflow_t* engine, ra_reflow_css_loader_fn loader, void* ctx);

/**
 * @brief Hit-test a point on a page against the laid-out `<a>` link rectangles.
 *
 * @details Walks `engine->link_rects[]` for @p page_idx and returns the href of
 * the first rectangle containing @p (x, y). Coordinates are page-local (the
 * same space ra_reflow_render_page() uses); subtract the render origin first if
 * the page was drawn at an offset. The href is returned as a slice into the
 * engine text pool -- read `&engine->...text...[*out_href_off]` for @p *out_href_len
 * bytes via the engine, or pass it straight to ra_reflow_href_split().
 *
 * @param[in]  engine       Initialized engine handle.
 * @param[in]  page_idx     Page to test.
 * @param[in]  x            Page-local x, pixels.
 * @param[in]  y            Page-local y, pixels.
 * @param[out] out_href_off Receives the href text-pool offset.
 * @param[out] out_href_len Receives the href length, bytes.
 *
 * @return ra_err_t
 * @retval k_ra_ok            A link rect contains the point; outputs set.
 * @retval k_ra_err_null_ptr  A required pointer is NULL.
 * @retval k_ra_err_not_found No link rect on @p page_idx contains the point.
 *
 * @pre  @p engine is initialized and laid out.
 * @pre  @p out_href_off / @p out_href_len are writable.
 * @post On success the outputs slice the engine text pool.
 * @post On failure the outputs are unchanged.
 *
 * @note Read-only; safe to call between render passes.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_hit_test_link(const ra_reflow_t* engine,
                                               uint32_t           page_idx,
                                               int32_t            x,
                                               int32_t            y,
                                               uint32_t*          out_href_off,
                                               uint32_t*          out_href_len);

/**
 * @brief Find the page of a same-chapter `id` anchor (for `#fragment` jumps).
 *
 * @details Linear-scans `engine->anchors[]` for an element whose captured `id`
 * equals @p id (exact byte compare). Used to resolve a `#frag` link to the page
 * holding the target element.
 *
 * @param[in]  engine   Initialized engine handle.
 * @param[in]  id       Fragment id bytes (no leading '#').
 * @param[in]  id_len   Length of @p id, bytes.
 * @param[out] out_page Receives the page index of the anchor.
 *
 * @return ra_err_t
 * @retval k_ra_ok            Anchor found; @p *out_page set.
 * @retval k_ra_err_null_ptr  A required pointer is NULL.
 * @retval k_ra_err_invalid_arg @p id_len is 0.
 * @retval k_ra_err_not_found No anchor matches @p id.
 *
 * @pre  @p engine is laid out; @p id / @p out_page are valid.
 * @pre  @p id_len > 0.
 * @post On success @p *out_page < engine page count.
 *
 * @note Read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_find_anchor(const ra_reflow_t* engine,
                                             const char*        id,
                                             uint32_t           id_len,
                                             uint32_t*          out_page);

/**
 * @brief Split + classify an `<a href>` into a path part and a `#fragment`.
 *
 * @details Pure string logic (no engine state): detects a URI scheme (external,
 * unsupported), a leading '#' (same-chapter fragment), or an embedded '#'
 * (chapter + fragment). The path part is `href[0 .. *out_path_len)`; the
 * fragment, if any, is `href[*out_frag_off .. *out_frag_off + *out_frag_len)`
 * (excluding the '#').
 *
 * @param[in]  href         Href bytes (not NUL-terminated).
 * @param[in]  len          Length of @p href, bytes.
 * @param[out] out_kind     Receives the classification.
 * @param[out] out_path_len Receives the path-part length (0 for fragment-only).
 * @param[out] out_frag_off Receives the fragment start offset (0 if none).
 * @param[out] out_frag_len Receives the fragment length (0 if none).
 *
 * @return ra_err_t
 * @retval k_ra_ok           Classified; all outputs set.
 * @retval k_ra_err_null_ptr A required pointer is NULL.
 *
 * @pre  @p href holds @p len bytes; all out pointers are writable.
 * @post `*out_kind` reflects the href shape; the spans index within @p href.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_reflow_href_split(const char*            href,
                                            uint32_t               len,
                                            ra_reflow_href_kind_t* out_kind,
                                            uint32_t*              out_path_len,
                                            uint32_t*              out_frag_off,
                                            uint32_t*              out_frag_len);

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

/**
 * @brief Bind an EPUB-embedded typeface as the engine's active face (#109).
 *
 * @details
 * Replaces the face bound at `ra_reflow_init()` with @p font_data so subsequent
 * layout + render use the book's own typeface (the common "the EPUB ships one
 * font" case). The blob is validated with `stbtt_InitFont` first; on any failure
 * the engine keeps its current face unchanged (graceful degradation -- never a
 * crash). The bytes are referenced, not copied (zero-heap), so they MUST outlive
 * the engine, exactly like the `ra_reflow_init()` font. If a chapter is already
 * laid out, the engine re-flows it against the new face (like
 * `ra_reflow_set_font_size()`); otherwise the next layout picks it up.
 *
 * Per-run family / bold / italic face *selection* across multiple embedded faces
 * is intentionally out of scope here and tracked on #109 (blocked on the
 * `@font-face` / `font-family` resolution prerequisite, #142).
 *
 * @param[in,out] engine    Initialised engine.
 * @param[in]     font_data TTF/OTF blob; must outlive the engine.
 * @param[in]     font_len  Length of @p font_data, bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Face bound (and re-flowed if a chapter was laid out).
 * @retval k_ra_err_null_ptr        @p engine or @p font_data is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 * @retval k_ra_err_invalid_size    `font_len < k_ra_reflow_min_font_bytes`.
 * @retval k_ra_err_not_supported   `stbtt_InitFont` rejected the blob (face unchanged).
 *
 * @pre `engine->in_use == 1`.
 * @pre @p font_data is non-NULL and outlives the engine.
 * @post On success `engine->font_data == font_data`; on failure the prior face
 *       is preserved byte-for-byte.
 *
 * @note Not thread-safe; single-threaded init/layout context.
 * @see ra_reflow_init(), ra_reflow_set_font_size()
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_reflow_bind_font(ra_reflow_t* engine, const uint8_t* font_data, size_t font_len);

/**
 * @brief Register one embedded `@font-face` typeface for per-run selection (#109).
 *
 * @details
 * Validates @p blob with `stbtt_InitFont` and, on success, appends it to the
 * engine's face registry keyed by @p css_face_idx (the index ::ra_css_match_face
 * returns for the parsed `<style>` `@font-face` table). After layout, a text run
 * whose cascaded `font-family` + emphasis match that table entry is rendered with
 * this face instead of the default bound at ::ra_reflow_init(); unmatched runs
 * fall back to the default. The app drives this after ::ra_epub_open by walking
 * the sheet's `@font-face` table, matching each `src` href to a manifest font.
 *
 * The blob is stored by pointer (zero-copy) and MUST outlive the engine. While a
 * face is registered (`engine->face_count > 0`) the pagination cache is bypassed
 * (`ra_reflow_cache_serialize` / `_load` return ::k_ra_err_invalid_state), so a
 * multi-face book is never serialized or mis-served under a different face set.
 *
 * @param[in,out] engine       Initialised engine.
 * @param[in]     css_face_idx `@font-face` table index this blob satisfies
 *                             (`0 .. sheet face_count - 1`).
 * @param[in]     blob         TTF/OTF bytes; must outlive the engine.
 * @param[in]     len          Length of @p blob, bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Face registered.
 * @retval k_ra_err_null_ptr        @p engine or @p blob is NULL.
 * @retval k_ra_err_not_initialized `engine->in_use == 0`.
 * @retval k_ra_err_invalid_size    `len < k_ra_reflow_min_font_bytes`.
 * @retval k_ra_err_no_mem          The registry is full (::k_ra_reflow_max_faces).
 * @retval k_ra_err_not_supported   `stbtt_InitFont` rejected the blob.
 *
 * @pre `engine->in_use == 1`; @p blob non-NULL and outlives the engine.
 * @pre `css_face_idx` is a valid `@font-face` table index.
 * @post On success `engine->face_count` grows by one; on failure it is unchanged.
 * @post Subsequent layouts may select this face per run; the cache is bypassed.
 *
 * @note Not thread-safe; single-threaded init/layout context.
 * @see ra_reflow_init(), ra_reflow_bind_font(), ra_css_match_face()
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_reflow_register_face(ra_reflow_t* engine, uint8_t css_face_idx, const uint8_t* blob, size_t len);

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
