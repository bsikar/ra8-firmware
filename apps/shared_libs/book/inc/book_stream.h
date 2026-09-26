/**
 * @file book_stream.h
 * @brief Strict, zero-allocation validation of a streamed RABOOK1 flat blob.
 * @ingroup grp_ereader
 *
 * @details The resident @ref book_validate API preserves the original v1
 * compatibility contract. This interface is the fail-closed ingestion gate for
 * newly downloaded or externally supplied books: it validates the canonical
 * wire layout and every reference through a positioned-read callback while
 * hashing the complete body through a bounded caller-owned transfer buffer.
 *
 * The callback is the shared ::ra8_vsource_read_fn seam from @c ra8_mem, not a
 * private typedef: @ref book_chunked_read already carries that exact shape, so
 * a chunked reader can be handed to @ref ra8_vsource_add_paged and to this
 * validator without an adapter (#770).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <stdint.h>

#include "book.h"
#include "ra8_err.h"
#include "ra8_vsource.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * @param[in] read Exact positioned-read callback over the inflated flat blob;
 *                 the shared ::ra8_vsource_read_fn seam.
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
 * @pre @p read obeys the ::ra8_vsource_read_fn exact-read contract: it fills
 *      all @p len bytes or returns an error, never a short count.
 * @pre The source is immutable for the duration of validation.
 * @pre @p scratch does not alias mutable source state used by @p read.
 * @post On success @p out_header describes the fully validated source.
 * @post On failure @p out_header is zeroed and must not be consumed.
 * @note No dynamic allocation or recursion is used.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t book_validate_stream_strict(ra8_vsource_read_fn read,
                                                    void*               read_ctx,
                                                    uint64_t            source_size,
                                                    uint8_t*            scratch,
                                                    uint32_t            scratch_cap,
                                                    book_header_t*      out_header);

#ifdef __cplusplus
}
#endif
