/**
 * @file ra8_reflow_tokenize_internal.h
 * @brief Test-access surface for ra8_reflow_tokenize.c internal helpers.
 * @ingroup grp_ereader
 *
 * @details
 * Exposes the small, individually-MC/DC-able helpers of the no-heap XHTML
 * tokenizer so `tests/test_ra8_reflow_tokenize.c` can drive both arms of
 * each decision directly (the tokenizer's main loop is otherwise reached
 * only through `priv_reflow_xml_walk`). Not part of the public API.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"

/**
 * @enum priv_tok_consts_t
 * @brief Tokenizer bounds and UTF-8 / numeric-entity encoding constants.
 *
 * @details Shared across the tokenizer translation units
 * (ra8_reflow_tokenize.c, ra8_reflow_tokenize_lex.c): the lexical primitives use
 * the UTF-8 / numeric-entity / tag-name constants while the markup engine uses
 * the nesting-depth bound and the run-style mask. Kept here so a single
 * definition is visible to every TU.
 *
 * @invariant `k_priv_uc_2byte < k_priv_uc_3byte < k_priv_uc_4byte < k_priv_uc_max`.
 * @see ra8_reflow_tok_utf8_encode
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_priv_max_depth     = 64U,       /**< Max element nesting (matches old shim). */
  k_priv_tag_name_cap  = 16U,       /**< Lower-cased tag-name buffer size.       */
  k_priv_entity_window = 12U,       /**< Max bytes scanned for one '&...;'.      */
  k_priv_entity_min    = 4U,        /**< Shortest valid reference ("&lt;").      */
  k_priv_uc_2byte      = 0x80U,     /**< Code points >= need >= 2 UTF-8 bytes.   */
  k_priv_uc_3byte      = 0x800U,    /**< Code points >= need >= 3 UTF-8 bytes.   */
  k_priv_uc_4byte      = 0x10000U,  /**< Code points >= need 4 UTF-8 bytes.      */
  k_priv_uc_max        = 0x10FFFFU, /**< Highest valid Unicode code point.       */
  k_priv_utf8_lead2    = 0xC0U,     /**< 2-byte sequence lead-byte prefix.       */
  k_priv_utf8_lead3    = 0xE0U,     /**< 3-byte sequence lead-byte prefix.       */
  k_priv_utf8_lead4    = 0xF0U,     /**< 4-byte sequence lead-byte prefix.       */
  k_priv_utf8_cont     = 0x80U,     /**< Continuation-byte prefix.               */
  k_priv_utf8_mask     = 0x3FU,     /**< Low 6 bits per continuation byte.       */
  k_priv_utf8_sh6      = 6U,        /**< Shift for one continuation byte.        */
  k_priv_utf8_sh12     = 12U,       /**< Shift for two continuation bytes.       */
  k_priv_utf8_sh18     = 18U,       /**< Shift for three continuation bytes.     */
  k_priv_base_dec      = 10U,       /**< Decimal numeric-entity base.            */
  k_priv_base_hex      = 16U,       /**< Hexadecimal numeric-entity base.        */
  k_priv_hex_offset    = 10U,       /**< Value of hex 'a'/'A' minus the letter.  */
  k_priv_style_mask    = ((uint32_t)k_ra8_reflow_style_bold | (uint32_t)k_ra8_reflow_style_italic |
                       (uint32_t)k_ra8_reflow_style_underline), /**< Run-style bits. */
} priv_tok_consts_t;

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
RA8_PRIV bool ra8_reflow_tok_is_xml_whitespace(char c);

/**
 * @brief Map a (possibly namespace-prefixed) tag name to its enum.
 *
 * @details Strips any `prefix:` namespace, lower-cases the local name, and
 * matches it against the v1 subset's tag table.
 *
 * @param[in] name NUL-terminated local-or-prefixed tag name.
 * @param[in] len  Length of `name` in bytes (excluding any NUL).
 * @return The matching ra8_reflow_html_tag_t, or k_ra8_reflow_tag_unknown.
 * @retval k_ra8_reflow_tag_unknown Name is null or unrecognised.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function; case-insensitive on ASCII.
 * @since 0.1.0
 */
RA8_PRIV ra8_reflow_html_tag_t ra8_reflow_tok_classify(const char* name, size_t len);

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
RA8_PRIV bool
ra8_reflow_tok_decode_entity(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used);

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
RA8_PRIV size_t ra8_reflow_tok_utf8_encode(uint32_t cp, uint8_t* dst);

/* ===========================================================================
 * Cross-TU helpers shared between ra8_reflow_tokenize.c and its companions
 * (ra8_reflow_tokenize_lex.c / ra8_reflow_tokenize_attr.c). Promoted from
 * file-local `static` so the markup engine can call the lexical + attribute
 * primitives. Production callers stay on the public API; only the defining
 * companion TU + the markup engine consume these.
 * ===========================================================================
 */

/**
 * @brief Test whether a tag is a block-flow container in the v1 subset.
 *
 * @details Block tags emit block-start / block-end tokens; all others do not
 * introduce a block box. Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in] tag Classified tag.
 * @return true for p / h1-h6 / ul / ol / li / blockquote / table family.
 * @retval false Any inline, void, or unknown tag.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_is_block(ra8_reflow_html_tag_t tag);

/**
 * @brief Return the style bit an inline tag contributes to its subtree.
 *
 * @details b/strong add bold, i/em add italic, a adds underline; every other
 * tag contributes nothing. Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in] tag Classified tag.
 * @return The ra8_reflow_style bit to OR in, or 0.
 * @retval 0 The tag carries no inline style.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV uint8_t ra8_reflow_tok_style_for(ra8_reflow_html_tag_t tag);

/**
 * @brief Append one token to the engine's token pool.
 *
 * @details Fills the next ra8_reflow_token_t and advances token_count, or fails
 * if the fixed pool is full. Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in,out] engine Engine whose token pool is appended to.
 * @param[in]     kind   Token kind.
 * @param[in]     tag    Associated tag (or k_ra8_reflow_tag_unknown).
 * @param[in]     style  Inline-style bits for the token.
 * @param[in]     off    Text-pool offset (text tokens only).
 * @param[in]     len    Text-pool length (text tokens only).
 * @return true on success, false on pool overflow.
 * @retval false The token pool is full.
 * @pre `engine` is non-null.
 * @pre `engine->token_count <= k_ra8_reflow_max_tokens`.
 * @post On success token_count is incremented by one.
 * @post On failure the pool is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_emit(ra8_reflow_t*           engine,
                                  ra8_reflow_token_kind_t kind,
                                  ra8_reflow_html_tag_t   tag,
                                  uint8_t                 style,
                                  uint32_t                off,
                                  uint32_t                len);

/**
 * @brief Write one byte through the whitespace-collapse state machine.
 *
 * @details Runs of ASCII whitespace collapse to a single space; a run that
 * begins with `*last_ws` true (start of node) drops its leading whitespace.
 * Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in,out] engine  Engine whose text pool is appended to.
 * @param[in]     ch      Byte to write.
 * @param[in,out] last_ws Collapse state (true if the prior byte was space).
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine` and `last_ws` are non-null.
 * @pre `engine->text_pool_used <= k_ra8_reflow_text_pool_bytes`.
 * @post On a written byte text_pool_used is incremented.
 * @post Collapsed (skipped) whitespace leaves the pool unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_feed(ra8_reflow_t* engine, char ch, bool* last_ws);

/**
 * @brief Entity-decode and whitespace-collapse a text run into the pool.
 *
 * @details Walks the run one source position at a time, decoding a recognised
 * entity to UTF-8 (else passing '&' through) and collapsing whitespace.
 * Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in,out] engine  Engine whose text pool is appended to.
 * @param[in]     buf     Source buffer.
 * @param[in]     start   Start offset of the run (inclusive).
 * @param[in]     end     End offset of the run (exclusive).
 * @param[out]    out_off Text-pool offset of the stored run.
 * @param[out]    out_len Number of bytes stored.
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine`, `buf`, `out_off`, `out_len` are non-null.
 * @pre `start <= end`.
 * @post On success *out_off / *out_len describe the stored slice.
 * @post On failure the pool may be partially written and is abandoned.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_stash_run(ra8_reflow_t*  engine,
                                       const uint8_t* buf,
                                       size_t         start,
                                       size_t         end,
                                       uint32_t*      out_off,
                                       uint32_t*      out_len);

/**
 * @brief Test whether `buf` at `i` begins with the literal `lit`.
 *
 * @details Bounds-checks that `lit` fits within `[i, len)` before comparing.
 * Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to test from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to match.
 * @return true if the literal fits and matches at `i`.
 * @retval false Out of range or mismatch.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_starts_with(const uint8_t* buf, size_t i, size_t len, const char* lit);

/**
 * @brief Return the index just past the first occurrence of `lit`.
 *
 * @details Linear bounded scan; used to skip comments / PIs / declarations.
 * Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to search from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to find.
 * @return Index just past `lit`, or `len` if not found.
 * @retval len The literal does not occur in `[i, len)`.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV size_t ra8_reflow_tok_skip_past(const uint8_t* buf, size_t i, size_t len, const char* lit);

/**
 * @brief Return the index of the first occurrence of `lit` (its start), or len.
 *
 * @details Like ::ra8_reflow_tok_skip_past but returns the literal's START
 * offset. Defined in ra8_reflow_tokenize_lex.c.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to search from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to find.
 * @return Start index of `lit`, or `len` if absent.
 * @retval len The literal does not occur in `[i, len)`.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post Return value is in `[i, len]`.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV size_t ra8_reflow_tok_find_lit(const uint8_t* buf, size_t i, size_t len, const char* lit);

/**
 * @brief Locate a named attribute's quoted value span within a tag (no copy).
 *
 * @details Returns the value's offset + length within @p tag so callers can
 * copy it to the pool or scan it in place. Defined in
 * ra8_reflow_tokenize_attr.c.
 *
 * @param[in]  tag      Raw tag span starting at '<'.
 * @param[in]  tag_len  Length of @p tag, bytes.
 * @param[in]  name     Lower-case attribute name to find.
 * @param[in]  name_len Length of @p name, bytes.
 * @param[out] out_voff Receives the value offset within @p tag.
 * @param[out] out_vlen Receives the value length, bytes.
 * @return true iff a quoted @p name value was found.
 * @retval true  `*out_voff` / `*out_vlen` describe the value within @p tag.
 * @retval false Attribute absent or malformed.
 * @pre `tag`, `name`, `out_voff`, `out_vlen` are non-null.
 * @pre `tag_len >= name_len`.
 * @post No state mutated.
 * @post On false, `*out_voff` and `*out_vlen` are unspecified.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_find_attr(const uint8_t* tag,
                                       size_t         tag_len,
                                       const char*    name,
                                       size_t         name_len,
                                       size_t*        out_voff,
                                       size_t*        out_vlen);

/**
 * @brief Copy a named attribute's quoted value from a tag span into the pool.
 *
 * @details Scans the raw `<...>` span for attribute @p name and copies its
 * quoted value verbatim into the engine text pool. On no match / malformed
 * attribute / empty value / full pool the outputs are zeroed. Defined in
 * ra8_reflow_tokenize_attr.c.
 *
 * @param[in,out] engine   Engine whose text pool receives the value bytes.
 * @param[in]     tag      Raw tag span starting at '<'.
 * @param[in]     tag_len  Length of @p tag, bytes.
 * @param[in]     name     Lower-case attribute name to find.
 * @param[in]     name_len Length of @p name, bytes.
 * @param[out]    out_off  Receives the text-pool offset of the value (0 if none).
 * @param[out]    out_len  Receives the value byte length (0 if none).
 * @return Nothing.
 * @pre `engine`, `tag`, `name`, `out_off`, `out_len` are non-null.
 * @pre `engine->text_pool_used <= k_ra8_reflow_text_pool_bytes`.
 * @post `*out_len > 0` iff a quoted @p name value was stored in the pool.
 * @post The pool grows by `*out_len` bytes when a value is stored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_reflow_tok_capture_attr(ra8_reflow_t*  engine,
                                          const uint8_t* tag,
                                          size_t         tag_len,
                                          const char*    name,
                                          size_t         name_len,
                                          uint32_t*      out_off,
                                          uint32_t*      out_len);

/**
 * @brief Build a CSS element identity from a just-opened tag's attributes.
 *
 * @details Locates the `id` and `class` attribute value spans in place (no
 * copy) so the cascade can match `#id` / `.class` selectors; the returned
 * pointers alias @p tag. Defined in ra8_reflow_tokenize_attr.c.
 *
 * @param[in] kind Classified tag.
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The element identity for ra8_css_cascade().
 * @retval ra8_css_element_t Populated struct; absent attributes have NULL pointer and zero length.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `kind` is a valid `ra8_reflow_html_tag_t` value.
 * @post No state mutated; returned pointers alias @p tag.
 * @post `el.tag == (uint8_t)kind`.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
RA8_PRIV ra8_css_element_t ra8_reflow_tok_css_element(ra8_reflow_html_tag_t kind,
                                                      const uint8_t*        tag,
                                                      size_t                span);

/**
 * @brief Parse a just-opened tag's inline `style="..."` into a CSS declaration.
 *
 * @details Locates the `style` attribute and delegates to
 * `ra8_css_parse_inline()`. The returned struct has `set == 0` when no inline
 * declarations override the cascade. Defined in ra8_reflow_tokenize_attr.c.
 *
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The inline CSS declaration for this element.
 * @retval ra8_css_style_t Struct with `set == 0` if no `style` attribute is present.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `span > 0`.
 * @post No state mutated.
 * @post Returned struct `set` field is 0 when no `style` attribute was found.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
RA8_PRIV ra8_css_style_t ra8_reflow_tok_css_inline(const uint8_t* tag, size_t span);

/**
 * @brief Intern an `<a>` href slice into the link-target table.
 *
 * @details Appends a {href_off, href_len} entry and returns its 1-based id. An
 * empty href or a full table returns 0. Defined in ra8_reflow_tokenize_attr.c.
 *
 * @param[in,out] engine   Engine whose link-target table grows.
 * @param[in]     href_off Href text-pool offset.
 * @param[in]     href_len Href byte length.
 * @return 1-based link id, or 0 if not interned.
 * @retval 0 Empty href or the table is full.
 * @retval 1..k_ra8_reflow_max_links The newly assigned 1-based link id.
 * @pre `engine` is non-null.
 * @pre `engine->link_target_count <= k_ra8_reflow_max_links`.
 * @post On a non-zero return the table grew by one entry.
 * @post `engine->link_target_count` is unchanged on a zero return.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV uint8_t ra8_reflow_tok_intern_link(ra8_reflow_t* engine,
                                            uint32_t      href_off,
                                            uint32_t      href_len);

/**
 * @brief True if a `rel` attribute value contains the `stylesheet` token.
 *
 * @details Case-sensitive substring scan, bounded by @p len. Defined in
 * ra8_reflow_tokenize_attr.c.
 *
 * @param[in] rel Attribute-value bytes (not NUL-terminated).
 * @param[in] len Length of @p rel, bytes.
 * @return true if `stylesheet` occurs in @p rel.
 * @retval true  The substring "stylesheet" was found in @p rel.
 * @retval false The substring is absent or @p len is too short.
 * @pre @p rel addresses @p len readable bytes.
 * @pre @p rel is non-null.
 * @post No state mutated.
 * @post Return value depends solely on the inputs.
 * @note Pure.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_tok_rel_is_stylesheet(const uint8_t* rel, size_t len);

/**
 * @brief Map a cascaded run's (font-family + emphasis) to an embedded face slot.
 *
 * @details Returns 0 (the engine's default bound face) when no embedded face
 * matches, else `1 + the registry index`. Defined in
 * ra8_reflow_tokenize_attr.c.
 *
 * @param[in] engine Engine whose face registry + parsed sheet are consulted.
 * @param[in] comp   The element's cascaded style (family + emphasis bits).
 * @return 0 for the default face, else `1 + engine->faces[] index`.
 * @retval 0 No registered face matches the cascaded family + emphasis combination.
 * @pre @p engine and @p comp are non-null.
 * @pre `engine->face_count <= k_ra8_reflow_max_faces`.
 * @post No state is modified.
 * @post Return value is in [0, engine->face_count].
 * @note Pure; reads engine state only.
 * @since 0.1.0
 */
RA8_PRIV uint8_t ra8_reflow_tok_resolve_face_slot(const ra8_reflow_t*    engine,
                                                  const ra8_css_style_t* comp);

/**
 * @brief Tokenize the XHTML buffer and populate `engine->tokens[]`.
 *
 * @details
 * The tokenizer entry point. A single forward pass over the XHTML source with
 * no heap and no DOM. Defined in `ra8_reflow_tokenize.c` and consumed by the
 * layout driver in `ra8_reflow_parse.c`; the per-decision helpers above are
 * the individually-MC/DC-able primitives it drives.
 *
 * @param[in,out] engine    Engine whose token / text pools are populated.
 * @param[in]     xhtml_buf XHTML source bytes.
 * @param[in]     xhtml_len Length of `xhtml_buf`, in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok         Tokenization succeeded.
 * @retval k_ra8_err_null_ptr   @p engine or @p xhtml_buf is null.
 * @retval k_ra8_err_invalid_size @p xhtml_len is zero.
 *
 * @pre @p engine and @p xhtml_buf are non-null.
 * @pre @p xhtml_len is non-zero.
 * @post On success `engine->tokens[]` is populated.
 * @post On failure the engine token / text pools are left unmodified.
 * @note Not thread-safe; single-threaded, non-recursive.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_reflow_xml_walk(ra8_reflow_t*  engine,
                                        const uint8_t* xhtml_buf,
                                        size_t         xhtml_len);
