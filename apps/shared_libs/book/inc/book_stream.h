/**
 * @file book_stream.h
 * @brief Strict, zero-allocation validation of a streamed RABOOK1 flat blob.
 * @ingroup grp_ereader
 *
 * @details The resident @ref book_validate API preserves the original v1
 * compatibility contract. This interface is the fail-closed ingestion gate for
 * newly downloaded or externally supplied books: it validates the canonical
 * wire layout and every reference through a random-read callback while hashing
 * the complete body through a bounded caller-owned transfer buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <stdint.h>

#include "book.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef book_stream_read_fn
 * @brief Exact random-read callback over an inflated RABOOK1 flat blob.
 * @param[in] ctx Opaque source context supplied to the validator.
 * @param[in] offset Byte offset from the beginning of the flat blob.
 * @param[out] dst Destination for exactly @p len bytes.
 * @param[in] len Exact byte count requested.
 * @return k_ra8_ok on a complete read, otherwise a source error.
 * @pre The callback either fills all @p len bytes or returns an error.
 * @since Version 0.1.0
 */
typedef ra8_err_t (*book_stream_read_fn)(void* ctx, uint64_t offset, uint8_t* dst, uint32_t len);

/**
 * @brief Strictly validate one callback-backed RABOOK1 flat blob.
 *
 * @details Requires the source length and header total to agree exactly, then
 *          enforces the canonical contiguous table/pool layout, known version
 *          and flags, string-boundary references, table indices, forward-only
 *          DOM links, exact attribute ownership, known image representations,
 *          and a gap-free image pool. Finally it reads every body byte through
 *          @p scratch and verifies the stored CRC-32. All wire integers are
 *          decoded little-endian, so validation does not depend on host
 *          alignment or byte order.
 *
 * @param[in] read Exact random-read callback over the inflated flat blob.
 * @param[in] read_ctx Opaque context passed to @p read.
 * @param[in] source_size Exact readable source length in bytes.
 * @param[out] scratch Caller-owned transfer and node-ownership workspace.
 * @param[in] scratch_cap Capacity of @p scratch; must be at least one byte and
 *                        at least @c ceil(node_count/8) bytes.
 * @param[out] out_header Receives the decoded host-order header on success.
 *
 * @return Validation status.
 * @retval k_ra8_ok The complete flat blob is canonical and intact.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size A length, layout, or extent is inconsistent.
 * @retval k_ra8_err_invalid_arg A semantic field or reference is invalid.
 * @retval k_ra8_err_range_check_failed The full body CRC does not match.
 * @retval k_ra8_err_* A callback error, returned verbatim.
 *
 * @pre The source is immutable for the duration of validation.
 * @pre @p scratch does not alias mutable source state used by @p read.
 * @post On success @p out_header describes the fully validated source.
 * @post On failure @p out_header is zeroed and must not be consumed.
 * @note No dynamic allocation or recursion is used.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t book_validate_stream_strict(book_stream_read_fn read,
                                                    void*               read_ctx,
                                                    uint64_t            source_size,
                                                    uint8_t*            scratch,
                                                    uint32_t            scratch_cap,
                                                    book_header_t*      out_header);

#ifdef __cplusplus
}
#endif
