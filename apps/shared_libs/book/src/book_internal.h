/**
 * @file book_internal.h
 * @brief book DOM-walk helpers shared across the library's translation units.
 * @ingroup grp_ereader
 *
 * @details
 * Module-private seam between book_xhtml.c (resident XHTML serialiser + text
 * extractor) and book_paged.c (the #163 paged text extractor). The plain-text
 * leaf helpers (whitespace collapse, paragraph break, block-element test) and the
 * shared walk-bound constants are declared here so the paged walk can reuse them
 * verbatim -- guaranteeing paged output is byte-identical to the resident walk --
 * without duplicating the logic. Also the seam between book.c (resident
 * container open) and book_chunked.c (demand-paged chunk reader): both parse
 * the "RBKC" container header through one helper so the format has a single
 * in-firmware definition. Not part of the public book API; consumers use
 * book.h / book_paged.h / book_chunked.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum book_container_field_off_t
 * @brief Byte offsets of the fixed "RBKC" container-header fields.
 * @details Mirrors the layout documented on @ref book_container_t; shared by
 *          the resident open (book.c) and the demand-paged chunk reader
 *          (book_chunked.c) so both parse one definition of the format.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_book_cont_off_chunk_bytes = 4U,  /**< uint32 LE: inflated bytes per chunk. */
  k_book_cont_off_total       = 8U,  /**< uint64 LE: flat-blob inflated total. */
  k_book_cont_off_count       = 16U, /**< uint32 LE: number of chunks.         */
  k_book_cont_off_reserved    = 20U, /**< uint32 LE: reserved, must be 0.      */
} book_container_field_off_t;

/**
 * @brief Parse + validate the fixed 24-byte "RBKC" container header.
 *
 * @details Checks the magic, requires a non-zero chunk size and inflated total,
 *          requires the reserved word to be zero, and requires the stored chunk
 *          count to equal `ceil(inflated_total / chunk_bytes)`. Field decoding
 *          is memcpy-based so the header buffer needs no alignment. Bounds
 *          against the file length are each caller's job (the resident open and
 *          the chunk reader own different views of the file).
 *
 * @param[in]  hdr             First @ref k_book_container_header_len bytes of
 *                             the container file.
 * @param[out] out_chunk_bytes Receives the inflated bytes-per-chunk.
 * @param[out] out_total       Receives the flat-blob inflated total.
 * @param[out] out_count       Receives the chunk count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Header well-formed; outputs populated.
 * @retval k_ra8_err_null_ptr    A pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg Bad magic, zero chunk size or total, non-zero
 *                              reserved word, or a chunk count that disagrees
 *                              with `ceil(total / chunk_bytes)`.
 *
 * @pre @p hdr holds at least @ref k_book_container_header_len readable bytes.
 * @pre All three output pointers are distinct, writable locations.
 * @post On k_ra8_ok every output is populated and internally consistent.
 * @post On any error no output is modified.
 *
 * @note Thread-safe: reads only @p hdr, writes only the outputs.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_container_header_fields(const uint8_t* hdr,
                                                     uint32_t*      out_chunk_bytes,
                                                     uint64_t*      out_total,
                                                     uint32_t*      out_count);

/**
 * @brief Decode one uint64 LE chunk-table entry from unaligned container bytes.
 *
 * @details memcpy-based so the table may sit at any alignment (baked MRAM
 *          arrays are byte-aligned). Entry `idx` starts at
 *          `table + idx * k_book_container_entry_len`.
 *
 * @param[in] table First byte of the chunk table.
 * @param[in] idx   Entry index (`<= chunk_count`, caller-bounded).
 *
 * @return The decoded payload-relative offset.
 * @retval 0 For entry 0 of every well-formed table (offsets are
 *           payload-relative and the first stream starts at 0).
 *
 * @pre @p table holds at least `(idx + 1) * k_book_container_entry_len` bytes.
 * @pre @p idx was bounds-checked against the validated chunk count.
 * @post No state is modified.
 * @post The result is a pure function of the table bytes.
 *
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_PRIV uint64_t priv_book_container_table_entry(const uint8_t* table, uint32_t idx);

/**
 * @brief Extend a finalized CRC-32/ISO-HDLC with another byte span.
 *
 * @details Uses the same reflected polynomial and complement convention as
 *          zlib's @c crc32(): pass 0 for the first span, then feed the returned
 *          value into each subsequent call. This lets resident and streaming
 *          validators share one wire-integrity definition without retaining
 *          the whole RABOOK body.
 *
 * @param[in] crc  CRC returned for all preceding spans, or 0 for the first.
 * @param[in] data Next readable byte span (non-NULL when @p len is non-zero).
 * @param[in] len  Number of bytes in @p data.
 *
 * @return CRC-32/ISO-HDLC over the concatenation of prior and current spans.
 * @retval UINT32_C(0) The concatenated byte stream's finalized CRC is zero.
 * @retval UINT32_MAX The concatenated byte stream's finalized CRC has every bit set.
 *
 * @pre @p data addresses at least @p len readable bytes when @p len is non-zero.
 * @pre @p crc is zero or a finalized value returned by an earlier call.
 * @post No caller memory or global state is modified.
 * @post Reusing the result with the next span preserves concatenation order.
 * @note Thread-safe: reads only immutable input and a constant lookup table.
 * @since Version 0.1.0
 */
RA8_PRIV uint32_t priv_book_crc32_extend(uint32_t crc, const uint8_t* data, size_t len);

/**
 * @enum book_xhtml_bound_t
 * @brief Bounds for the iterative, recursion-free DOM walks (resident + paged).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_book_xhtml_stack  = 512U, /**< Max pending open/close walk entries.        */
  k_book_xhtml_iter_x = 4U,   /**< Iteration-guard multiplier over node_count. */
} book_xhtml_bound_t;

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
RA8_PRIV bool priv_book_is_block(const char* name);

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
RA8_PRIV bool
priv_book_emit_text(char* out, size_t cap, size_t* pos, const char* str, bool* at_break);

/**
 * @brief Append a paragraph break, collapsing consecutive block-level breaks.
 *
 * @details Trims any trailing spaces already written (decrementing @p *pos while
 *          the last byte is a space), sets @p *at_break to suppress leading
 *          whitespace next, then appends a single `'\n'` unless the last byte is
 *          already `'\n'` (so nested/adjacent block elements collapse to one
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
 * @post Trailing spaces before @p *pos are removed; at most one `'\n'` added.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 * @since Version 0.1.0
 */
RA8_PRIV bool priv_book_emit_break(char* out, size_t cap, size_t* pos, bool* at_break);
