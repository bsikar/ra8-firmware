/**
 * @file ra_reflow_internal.h
 * @brief Test-access surface for ra_reflow internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_reflow facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Return true iff @p tag is a tag that introduces / removes a
 *        block-level indent (currently @c <li> and @c <blockquote>).
 *
 * @details Promoted from the inline expressions in
 *          ``priv_open_block`` (line 479) and ``priv_close_block``
 *          (line 513) so tests can drive both arms of the
 *          ``tag == li || tag == blockquote`` decision under
 *          -fcoverage-mcdc on the production source.
 *
 * @param[in] tag Token tag value (raw @c uint8_t storage of
 *                @ref ra_reflow_html_tag_t to keep this header free of
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
 * Drives lines 479 / 513 ``tok->tag == k_ra_reflow_tag_li || tok->tag
 * == k_ra_reflow_tag_blockquote`` (2 conditions, OR; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
bool ra_reflow_internal_is_indent_tag(uint8_t tag);

#ifdef __cplusplus
}
#endif
