/**
 * @file ra_book_internal.h
 * @brief ra_book DOM-walk helpers shared across the library's translation units.
 *
 * @details
 * Module-private seam between ra_book_xhtml.c (resident XHTML serialiser + text
 * extractor) and ra_book_paged.c (the #163 paged text extractor). The plain-text
 * leaf helpers (whitespace collapse, paragraph break, block-element test) and the
 * shared walk-bound constants are declared here so the paged walk can reuse them
 * verbatim -- guaranteeing paged output is byte-identical to the resident walk --
 * without duplicating the logic. Not part of the public ra_book API; consumers
 * use ra_book.h / ra_book_paged.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since Version 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra_book_xhtml_bound_t
 * @brief Bounds for the iterative, recursion-free DOM walks (resident + paged).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra_book_xhtml_stack  = 512U, /**< Max pending open/close walk entries.        */
  k_ra_book_xhtml_iter_x = 4U,   /**< Iteration-guard multiplier over node_count. */
} ra_book_xhtml_bound_t;

/**
 * @brief Test whether an element name is a block-level HTML element.
 *
 * @details Linear search through a fixed table of block-level element names used
 *          to decide where to insert paragraph-break newlines during plain-text
 *          extraction (@c p, @c h1..h6, @c li, @c ul, @c ol, @c div, @c br,
 *          @c hr, @c section, @c tr, @c pre, @c header, @c blockquote,
 *          @c article, @c aside, @c footer, @c figure, @c figcaption). The
 *          comparison is case-sensitive (DOM names are lower-cased at compile).
 *
 * @param[in] name NUL-terminated element name to test (e.g. @c "p").
 *
 * @return bool Query result.
 * @retval true  @p name is a block-level element; emit a break before it.
 * @retval false @p name is an inline element; no break.
 *
 * @pre  @p name is a valid, NUL-terminated C string.
 * @pre  @p name contains only lower-case ASCII letters (DOM invariant).
 * @post The block-element table is not modified.
 * @post The result is a pure function of @p name (no side effects).
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 * @since Version 0.1.0
 */
bool ra_book_is_block(const char* name);

/**
 * @brief Append a whitespace-collapsed text run to the output buffer.
 *
 * @details Folds any run of space / tab / CR / LF into at most a single ASCII
 *          space; non-whitespace is forwarded via the raw append. @p at_break
 *          carries inter-call state: when @c true, leading whitespace in the
 *          fragment is dropped (so indentation between inline elements does not
 *          leak into prose). It is set @c true after a collapsed space and
 *          @c false after any non-whitespace byte -- so calling this repeatedly
 *          over chunks of one logical run yields the same result as one call.
 *
 * @param[out]    out      Destination character buffer.
 * @param[in]     cap      Total capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced for each byte written.
 * @param[in]     str      NUL-terminated text fragment to collapse and append.
 * @param[in,out] at_break Whitespace-collapse carry flag (in: prior state; out:
 *                         trailing state after this fragment).
 *
 * @return bool Append result.
 * @retval true  Fragment appended (or it was entirely suppressed whitespace).
 * @retval false Output buffer overflowed; partial output may have been written.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p str is a valid, NUL-terminated C string.
 * @post On success every character in @p str was processed; @p *pos advanced.
 * @post @p *at_break reflects the trailing whitespace state, success or not.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 * @since Version 0.1.0
 */
bool ra_book_emit_text(char* out, size_t cap, size_t* pos, const char* str, bool* at_break);

/**
 * @brief Append a paragraph break, collapsing consecutive block-level breaks.
 *
 * @details Trims any trailing spaces already written (decrementing @p *pos while
 *          the last byte is a space), sets @p *at_break to suppress leading
 *          whitespace next, then appends a single @c '\n' unless the last byte is
 *          already @c '\n' (so nested/adjacent block elements collapse to one
 *          blank line).
 *
 * @param[out]    out      Destination character buffer.
 * @param[in]     cap      Total capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; may be trimmed, then advanced by 1.
 * @param[in,out] at_break Set @c true on return; entry value ignored.
 *
 * @return bool Append result.
 * @retval true  Break emitted (or collapsed into an existing newline).
 * @retval false Output buffer overflowed while writing the newline byte.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p pos is non-null and @p *pos <= @p cap on entry.
 * @post @p *at_break is @c true on return.
 * @post Trailing spaces before @p *pos are removed; at most one @c '\n' added.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 * @since Version 0.1.0
 */
bool ra_book_emit_break(char* out, size_t cap, size_t* pos, bool* at_break);
