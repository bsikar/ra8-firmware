/**
 * @file ra8_reflow_css_internal.h
 * @brief Cross-TU surface for the split content-CSS cascade (#111).
 * @ingroup grp_ereader
 *
 * @details
 * The content-CSS implementation is split across three translation units that
 * share a small set of pure, leaf-level helpers:
 *   - ra8_reflow_css.c          -- parser entry, selector + `@font-face` parsing.
 *   - ra8_reflow_css_decl.c     -- declaration-body parsing (colour / font-size /
 *                                 emphasis / align / `prop:value;` block).
 *   - ra8_reflow_css_cascade.c  -- selector matching, the cascade, and the public
 *                                 cascade / face-lookup API.
 * Only the symbols referenced by more than one of those TUs are declared here;
 * every TU-private helper stays `static` in its defining file. There is no MMIO
 * and no heap: all state lives in the caller-owned ::ra8_css_sheet_t.
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

#include "ra8_attributes.h"
#include "ra8_reflow_css.h"

/**
 * @brief True for CSS whitespace (space / tab / newline / CR / FF).
 *
 * @details Classifies a single byte as a CSS white-space character per the CSS
 * specification: space (0x20), horizontal tab (0x09), newline (0x0A), carriage
 * return (0x0D), and form feed (0x0C). Used throughout the parser to skip or
 * trim tokens.
 *
 * @param[in] c Byte to classify (any value).
 *
 * @return bool Classification result.
 * @retval true  @p c is a CSS whitespace byte.
 * @retval false @p c is not a CSS whitespace byte.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller must be in a context where character classification is meaningful.
 * @post Return value is determined solely by @p c; no state is mutated.
 * @post The function has no side effects.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_css_is_ws(char c);

/**
 * @brief ASCII-fold one byte to lower case.
 *
 * @details Converts an ASCII upper-case letter ('A'-'Z') to its lower-case
 * equivalent by adding the fixed offset between 'A' and 'a'. All other byte
 * values are returned unchanged. This is used for case-insensitive CSS keyword
 * and identifier comparisons throughout the parser.
 *
 * @param[in] c Byte to fold; may be any value.
 *
 * @return char The lower-cased byte.
 * @retval c + ('a' - 'A') When @p c is in the range 'A'..'Z'.
 * @retval c               For all other byte values.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller must be performing a case-insensitive comparison or scan.
 * @post Return value differs from @p c only when @p c is an ASCII upper-case letter.
 * @post No state is mutated; the function is pure.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_PRIV char ra8_reflow_css_lower(char c);

/**
 * @brief Case-insensitive compare of span @p s[0..len) against NUL literal @p lit.
 *
 * @details Compares exactly @p len bytes of @p s against the NUL-terminated
 * string @p lit using ASCII case folding via ra8_reflow_css_lower(). Returns true
 * only when the span and the literal have equal length and all bytes fold equal.
 * Either null pointer causes an immediate false return without dereferencing.
 *
 * @param[in] s   Pointer to the byte span (need not be NUL-terminated).
 * @param[in] len Number of bytes in @p s to compare.
 * @param[in] lit NUL-terminated reference string; must not be NULL for a true result.
 *
 * @return bool Comparison result.
 * @retval true  @p s[0..len) equals @p lit case-insensitively and lengths match.
 * @retval false Lengths differ, any byte differs, or a null pointer was passed.
 *
 * @pre @p len is the exact byte count of the span to compare (may be 0).
 * @pre @p lit is a valid NUL-terminated string pointer or NULL.
 * @post No state is mutated; the function is pure.
 * @post @p s and @p lit are not modified.
 *
 * @note Thread-safe; no shared mutable state.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_reflow_css_ci_eq(const char* s, size_t len, const char* lit);

/**
 * @brief Trim leading + trailing whitespace from @p s[0..*len); returns start.
 *
 * @details Advances past leading ra8_reflow_css_is_ws() bytes and shrinks past
 * trailing ones, writing the trimmed length back through @p len and returning a
 * pointer to the first retained byte. Used to normalise property names, values,
 * and selector spans before classification.
 *
 * @param[in]     s   Byte span to trim (need not be NUL-terminated).
 * @param[in,out] len On entry, the span length; on return, the trimmed length.
 *
 * @return const char* Pointer to the first non-whitespace byte of @p s.
 * @retval &s[a] Pointer to the trimmed start; @p *len holds the trimmed length.
 *
 * @pre @p s points to at least @p *len readable bytes.
 * @pre @p len is non-NULL.
 * @post @p *len is in [0, *len_entry]; @p s contents are not modified.
 * @post The returned pointer lies within [@p s, @p s + *len_entry].
 *
 * @note Thread-safe; operates only on caller-owned memory.
 * @since 0.1.0
 */
RA8_PRIV const char* ra8_reflow_css_trim(const char* s, size_t* len);

/**
 * @brief Parse a `prop:value; ...` declaration body (no braces) into @p out.
 *
 * @details Iterates over semicolon-delimited declarations in @p s[0..len).
 * For each declaration, it locates the colon separator, trims whitespace from
 * both property name and value, and applies the recognised property to @p out.
 * Declarations with an empty property or value after trimming are skipped
 * silently. The loop is bounded by @p len; each iteration advances past the next
 * semicolon.
 *
 * @param[in]     s   Byte span of the declaration block (no surrounding braces).
 * @param[in]     len Number of bytes in @p s.
 * @param[in,out] out Style record to accumulate parsed declarations into.
 *
 * @return Nothing.
 *
 * @pre @p s points to at least @p len readable bytes (may be NULL only when len == 0).
 * @pre @p out is non-NULL and zero-initialised or partially filled by the caller.
 * @post @p out reflects all successfully parsed declarations from @p s.
 * @post @p s is not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
RA8_PRIV void ra8_reflow_css_parse_decls(const char* s, size_t len, ra8_css_style_t* out);

/**
 * @brief Dispatch one `selector|@rule { block }`: at-rule vs. style rule.
 *
 * @details Trims @p sel to obtain the effective keyword. When the first byte is
 * '@', routes to the `@font-face` handler. Otherwise, parses the declaration
 * block and distributes the resulting style to every comma-separated selector in
 * @p sel. This is the per-block worker called by the top-level stylesheet
 * scanner ra8_css_parse() (which lives in a different translation unit).
 *
 * @param[in,out] sheet     Sheet receiving parsed rules or font faces.
 * @param[in]     sel       Selector or at-keyword text.
 * @param[in]     sel_len   Number of bytes in @p sel.
 * @param[in]     block     Declaration block body (no surrounding braces).
 * @param[in]     block_len Number of bytes in @p block.
 *
 * @return Nothing.
 *
 * @pre @p sheet is non-NULL.
 * @pre @p sel points to at least @p sel_len readable bytes.
 * @post @p sheet has been updated with all rules or faces derived from this block.
 * @post @p sel and @p block are not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_PRIV void ra8_reflow_css_parse_one_block(ra8_css_sheet_t* sheet,
                                             const char*      sel,
                                             size_t           sel_len,
                                             const char*      block,
                                             size_t           block_len);
