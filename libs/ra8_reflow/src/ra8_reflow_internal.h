/**
 * @file ra8_reflow_internal.h
 * @brief Test-access surface for ra8_reflow internal helpers (MC/DC).
 * @ingroup grp_ereader
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra8_reflow facade. See CLAUDE.md
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
 *          ``priv_open_block`` (line 479) and ``priv_close_block``
 *          (line 513) so tests can drive both arms of the
 *          ``tag == li || tag == blockquote`` decision under
 *          -fcoverage-mcdc on the production source.
 *
 * @param[in] tag Token tag value (raw @c uint8_t storage of
 *                @ref ra8_reflow_html_tag_t to keep this header free of
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
 * Drives lines 479 / 513 ``tok->tag == k_ra8_reflow_tag_li || tok->tag
 * == k_ra8_reflow_tag_blockquote`` (2 conditions, OR; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_internal_is_indent_tag(uint8_t tag);

/**
 * @brief Decide whether a glyph emission would overflow the right
 *        margin AND the current line already has content.
 *
 * @details
 * Promoted from the inline compound decisions in
 * @c priv_emit_char (line 404), @c priv_layout_text (line 468) and
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
RA8_PRIV bool ra8_reflow_internal_right_overflow_break(int32_t cursor_x,
                                                       int32_t advance,
                                                       int32_t right_limit,
                                                       uint8_t line_has_content);

/**
 * @brief Decide whether the cached XHTML buffer pointer/length pair
 *        is unusable for a re-flow (NULL pointer OR zero length).
 *
 * @details
 * Promoted from the inline OR decision in
 * @c ra8_reflow_set_font_size (line 953).
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
RA8_PRIV bool ra8_reflow_internal_xhtml_invalid(const void* xhtml_buf, size_t xhtml_len);

/**
 * @brief Decide whether the layout pass produced zero pages but the
 *        token stream was non-empty (must synthesise a final page).
 *
 * @details
 * Promoted from the inline AND decision in
 * @c ra8_reflow_run_layout (line 750).
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
RA8_PRIV bool ra8_reflow_internal_final_page_needed(uint32_t page_count, uint32_t token_count);

#ifdef __cplusplus
}
#endif
