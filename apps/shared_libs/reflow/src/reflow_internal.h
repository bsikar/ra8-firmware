/**
 * @file reflow_internal.h
 * @brief Test-access surface for reflow internal helpers (MC/DC).
 * @ingroup grp_ereader
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public reflow facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief Return true iff @p tag is a tag that introduces / removes a
 *        block-level indent (currently `<li>` and `<blockquote>`).
 *
 * @details Promoted from the inline expressions in
 *          ``internal_open_block`` (line 479) and ``internal_close_block``
 *          (line 513) so tests can drive both arms of the
 *          ``tag == li || tag == blockquote`` decision under
 *          -fcoverage-mcdc on the production source.
 *
 * @param[in] tag Token tag value (raw @c uint8_t storage of
 *                @ref reflow_html_tag_t to keep this header free of
 *                public-API includes).
 *
 * @return Boolean indent-tag predicate.
 * @retval true  Tag is @c li or @c blockquote.
 * @retval false Otherwise.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on @p tag.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives lines 479 / 513 ``tok->tag == k_reflow_tag_li || tok->tag
 * == k_reflow_tag_blockquote`` (2 conditions, OR; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_internal_is_indent_tag(uint8_t tag);

/**
 * @brief Decide whether a glyph emission would overflow the right
 *        margin AND the current line already has content.
 *
 * @details
 * Promoted from the inline compound decisions in
 * @c internal_emit_char (line 404), @c internal_layout_text (line 468) and
 * @c priv_apply_image (line 605) so the
 * ``cur->x + advance > right_limit && line_has_content != 0`` AND
 * decision can be driven directly under @c -fcoverage-mcdc.
 *
 * @param[in] cursor_x  Current pen x position in pixels.
 * @param[in] advance   Width about to be emitted in pixels.
 * @param[in] right_limit  Right edge in pixels (viewport_w - margin).
 * @param[in] line_has_content  Non-zero iff the current line already
 *                              has at least one glyph.
 *
 * @return Boolean break-needed predicate.
 * @retval true  Caller must call @c priv_newline before emitting.
 * @retval false Emitting in place is safe.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the four arguments.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - x+adv <= right, content=1   -> false (control: both false-side)
 *  - x+adv >  right, content=1   -> true  (varies left only)
 *  - x+adv >  right, content=0   -> false (varies right only)
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_internal_right_overflow_break(int32_t cursor_x,
                                                        int32_t advance,
                                                        int32_t right_limit,
                                                        uint8_t line_has_content);

/**
 * @brief Decide whether the cached XHTML buffer pointer/length pair
 *        is unusable for a re-flow (NULL pointer OR zero length).
 *
 * @details
 * Promoted from the inline OR decision in
 * @c reflow_set_font_size (line 953).
 *
 * @param[in] xhtml_buf  Cached buffer pointer (may be NULL).
 * @param[in] xhtml_len  Cached buffer length (may be zero).
 *
 * @return Boolean invalid-buffer predicate.
 * @retval true  Buffer is unusable; caller must return invalid_state.
 * @retval false Buffer is usable.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - buf!=NULL, len!=0 -> false (both false-side)
 *  - buf==NULL, len!=0 -> true  (varies buf)
 *  - buf!=NULL, len==0 -> true  (varies len)
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_internal_xhtml_invalid(const void* xhtml_buf, size_t xhtml_len);

/**
 * @brief Decide whether the layout pass produced zero pages but the
 *        token stream was non-empty (must synthesise a final page).
 *
 * @details
 * Promoted from the inline AND decision in
 * @c reflow_run_layout (line 750).
 *
 * @param[in] page_count   Number of pages flushed during the pass.
 * @param[in] token_count  Total parsed-token count.
 *
 * @return Boolean fixup-needed predicate.
 * @retval true  Caller must synthesise a single final page.
 * @retval false No fixup required.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - pages>0,  tokens>0  -> false
 *  - pages==0, tokens>0  -> true
 *  - pages==0, tokens==0 -> false
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_internal_final_page_needed(uint32_t page_count, uint32_t token_count);

/**
 * @struct priv_reflow_tofu_rect_t
 * @brief Geometry of the missing-glyph (tofu) box, relative to the glyph pen.
 *
 * @details Produced by ::priv_reflow_render_tofu_rect and consumed by the
 *          render pass, which adds the glyph's own baseline-left position and
 *          the page origin before drawing. Offsets are relative so the
 *          geometry stays a pure function of the font metrics.
 *
 * @since 0.1.0
 */
typedef struct {
  int32_t x_off; /**< Left edge, pixels right of the glyph pen x.     */
  int32_t y_off; /**< Top edge, pixels above the baseline (negative). */
  int32_t w;     /**< Box width in pixels (>= k_priv_tofu_min_px).    */
  int32_t h;     /**< Box height in pixels (>= k_priv_tofu_min_px).   */
} priv_reflow_tofu_rect_t;

/**
 * @brief Return true iff @p cp is a code point that must draw nothing.
 *
 * @details
 * Space and the zero-width format characters legitimately have no ink, so a
 * face that maps them to glyph 0 must stay blank rather than gain a tofu box.
 * Every other code point the face cannot draw is a missing glyph and is drawn
 * as the box (see ::priv_reflow_render_needs_tofu).
 *
 * @param[in] cp Code point from the layout pass.
 *
 * @return Boolean blank-code-point predicate.
 * @retval true  @p cp is whitespace or a zero-width format character.
 * @retval false @p cp is expected to carry ink.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on @p cp.
 *
 * @note Test-access only. Pure function.
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_render_is_blank_cp(int32_t cp);

/**
 * @brief Decide whether a code point must be drawn as a missing-glyph box.
 *
 * @details
 * Promoted from the guard in @c internal_blit_glyph so the
 * ``glyph_index == 0 && !blank(cp)`` AND decision can be driven directly
 * under @c -fcoverage-mcdc. @p glyph_index is what
 * @c stbtt_FindGlyphIndex reported for @p cp in the resolved face; zero is
 * stb's "this face has no glyph for that code point" answer.
 *
 * @param[in] glyph_index Glyph index the face reported for @p cp.
 * @param[in] cp          Code point from the layout pass.
 *
 * @return Boolean tofu-needed predicate.
 * @retval true  The face cannot draw @p cp and @p cp should carry ink.
 * @retval false The face has a glyph, or @p cp is legitimately blank.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - index!=0, cp inked  -> false
 *  - index==0, cp inked  -> true
 *  - index==0, cp blank  -> false
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_render_needs_tofu(int32_t glyph_index, int32_t cp);

/**
 * @brief Compute the missing-glyph box geometry from the face metrics.
 *
 * @details
 * The box sits on the baseline (``y_off == -h``) and is inset inside the
 * code point's own advance so consecutive tofu boxes stay separated. A face
 * that reports no usable advance or ascent (a degenerate or unscaled metric)
 * falls back to fractions of @p font_px, so the box is always drawable and
 * the render pass never has to special-case a broken face.
 *
 * @param[in]  advance_px Scaled advance width for the code point, pixels.
 * @param[in]  ascent_px  Scaled face ascent, pixels.
 * @param[in]  font_px    Glyph size in pixels (the fallback basis).
 * @param[out] out        Receives the box geometry.
 *
 * @return Boolean success flag.
 * @retval true  @p out holds a drawable box.
 * @retval false @p out was NULL, or @p font_px is not positive.
 *
 * @pre @p out addresses writable storage, or is NULL.
 * @pre None.
 * @post On true, ``out->w`` and ``out->h`` are at least the minimum box size.
 * @post On false, @p out is untouched.
 *
 * @note Test-access only. Pure function.
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_reflow_render_tofu_rect(int32_t                  advance_px,
                                           int32_t                  ascent_px,
                                           int32_t                  font_px,
                                           priv_reflow_tofu_rect_t* out);

/**
 * @brief Injected per-face coverage probe used to resolve a fallback face.
 *
 * @details
 * The render pass answers this from ``stbtt_FindGlyphIndex`` over its
 * per-render ``stbtt_fontinfo`` array; a test answers it from a table, so
 * ::priv_reflow_render_pick_face can be driven without a font file. Index
 * 0 is the engine's bound default face and 1.. are the registered
 * ``@font-face`` blobs, the same numbering the glyph style field carries.
 *
 * @param[in] ctx       Opaque face set supplied by the caller.
 * @param[in] face_idx  Face to probe, ``0 .. face_count - 1``.
 * @param[in] cp        Code point to look for.
 *
 * @return Boolean coverage answer.
 * @retval true  That face can draw @p cp.
 * @retval false That face has no glyph for @p cp.
 *
 * @since 0.1.0
 */
typedef bool (*priv_reflow_face_has_glyph_fn)(const void* ctx, uint8_t face_idx, int32_t cp);

/**
 * @brief Resolve which face draws @p cp, falling back by coverage (#687).
 *
 * @details
 * A run's face comes from CSS ``@font-face`` selection, which knows family,
 * weight and style but nothing about coverage, so a book whose body face
 * lacks a code point loses that character even when another registered face
 * carries it. This picks, in order: the run's own face, then the engine's
 * bound default face, then the registered faces by index. When no face
 * covers @p cp the run's own face is returned unchanged, so the
 * missing-glyph box is drawn at the metrics of the text around it.
 *
 * A blank code point is never hunted for: it must stay blank in its own
 * face (see ::priv_reflow_render_is_blank_cp). A face set of one is
 * returned immediately as well, so a book with no embedded faces pays no
 * coverage probe at all.
 *
 * @param[in] has_glyph  Per-face coverage probe; NULL disables fallback.
 * @param[in] ctx        Opaque face set handed to @p has_glyph.
 * @param[in] face_count Number of usable faces (default plus registered).
 * @param[in] primary    Face the layout pass selected for this run.
 * @param[in] cp         Code point from the layout pass.
 *
 * @return Face index to render @p cp with.
 * @retval primary The run's own face covers @p cp, nothing else does, the
 *                 code point is blank, or the arguments disable fallback.
 *
 * @pre @p ctx stays valid for the duration of the call.
 * @pre @p face_count counts the faces @p has_glyph will accept.
 * @post No state mutated; only @p has_glyph is called, at most once per face.
 * @post The returned index is always less than @p face_count when fallback
 *       was possible, and @p primary otherwise.
 *
 * @note Test-access only. Pure apart from @p has_glyph.
 *
 * @par MC/DC:
 * Guard is a 4-condition OR (``has_glyph == NULL``, ``face_count <= 1``,
 * ``primary >= face_count``, blank code point); the scan carries a
 * 2-condition AND (``k != primary && has_glyph(k)``), N+1 = 3 vectors.
 *
 * @since 0.1.0
 */
RA8_PRIV uint8_t priv_reflow_render_pick_face(priv_reflow_face_has_glyph_fn has_glyph,
                                              const void*                   ctx,
                                              uint8_t                       face_count,
                                              uint8_t                       primary,
                                              int32_t                       cp);

#ifdef __cplusplus
}
#endif
