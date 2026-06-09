/**
 * @file ra_reflow_tokenize_internal.h
 * @brief Test-access surface for ra_reflow_tokenize.c internal helpers.
 *
 * @details
 * Exposes the small, individually-MC/DC-able helpers of the no-heap XHTML
 * tokenizer so `tests/test_ra_reflow_tokenize.c` can drive both arms of
 * each decision directly (the tokenizer's main loop is otherwise reached
 * only through `priv_reflow_xml_walk`). Not part of the public API.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra_reflow.h"

/**
 * @brief True for the ASCII characters XHTML treats as whitespace.
 *
 * @details Matches space, tab, newline, carriage-return, form-feed and
 * vertical-tab -- the set collapsed by the tokenizer's text handling.
 *
 * @param[in] c Byte to classify.
 * @return true if `c` is space/tab/newline/carriage-return/form-feed/vtab.
 * @retval false `c` is any non-whitespace byte.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
bool ra_reflow_tok_is_xml_whitespace(char c);

/**
 * @brief Map a (possibly namespace-prefixed) tag name to its enum.
 *
 * @details Strips any `prefix:` namespace, lower-cases the local name, and
 * matches it against the v1 subset's tag table.
 *
 * @param[in] name NUL-terminated local-or-prefixed tag name.
 * @param[in] len  Length of `name` in bytes (excluding any NUL).
 * @return The matching ra_reflow_html_tag_t, or k_ra_reflow_tag_unknown.
 * @retval k_ra_reflow_tag_unknown Name is null or unrecognised.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function; case-insensitive on ASCII.
 * @since 0.1.0
 */
ra_reflow_html_tag_t ra_reflow_tok_classify(const char* name, size_t len);

/**
 * @brief Decode one XML entity reference beginning at `&`.
 *
 * @details Recognises the named entities amp/lt/gt/quot/apos and numeric
 * `&#dec;` / `&#xhex;` references. Unrecognised sequences are reported as
 * "not an entity" so the caller emits the literal `&`.
 *
 * @param[in]  src      Buffer positioned so `src[0] == '&'`.
 * @param[in]  avail    Bytes available from `src` (>= 1).
 * @param[out] out_cp   Decoded Unicode code point on success.
 * @param[out] out_used Bytes consumed from `src` on success.
 * @return true if a complete recognised entity was decoded.
 * @retval false Not a recognised/complete entity (caller emits '&').
 * @pre `src`, `out_cp`, `out_used` are non-null and `avail >= 1`.
 * @pre `src[0] == '&'`.
 * @post On false, out params are unspecified and no input is consumed.
 * @post On true, *out_used is in [3, avail].
 * @note Pure function.
 * @since 0.1.0
 */
bool ra_reflow_tok_decode_entity(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used);

/**
 * @brief UTF-8 encode a code point into `dst` (up to 4 bytes).
 *
 * @details Selects the 1/2/3/4-byte form by code-point range; values above
 * U+10FFFF are clamped to the maximum valid code point.
 *
 * @param[in]  cp  Unicode code point (clamped to the valid range).
 * @param[out] dst Destination of at least 4 bytes.
 * @return Number of bytes written (1..4).
 * @retval 1 ASCII code point.
 * @pre `dst` is non-null with room for 4 bytes.
 * @pre None.
 * @post Exactly the returned number of bytes in `dst` are written.
 * @post No other state is modified.
 * @note Pure function.
 * @since 0.1.0
 */
size_t ra_reflow_tok_utf8_encode(uint32_t cp, uint8_t* dst);
