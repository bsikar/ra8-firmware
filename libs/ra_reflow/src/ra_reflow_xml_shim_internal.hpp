/**
 * @file ra_reflow_xml_shim_internal.hpp
 * @brief Test-access surface for ra_reflow_xml_shim.cpp internal helpers.
 *
 * @details
 * Not part of the public API. The C++ test target
 * @c test_ra_reflow_xml_shim MAY include this header to drive
 * compound boolean decisions sitting inside the TU-private helpers
 * of @c ra_reflow_xml_shim.cpp under @c -fcoverage-mcdc.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ra_reflow_xml_shim_internal {

/**
 * @brief Lower-case-and-truncate copy of @p src into @p dst.
 *
 * @details
 * Promoted from the inline expressions at lines 70 and 72 of
 * @c ra_reflow_xml_shim.cpp so MC/DC tests can drive both arms of:
 *  - ``while (tail[i] != '\0' && i + 1U < sizeof(lower))`` (line 70)
 *  - ``if (c >= 'A' && c <= 'Z')`` (line 72)
 *
 * @param[in]  src      Source NUL-terminated string.
 * @param[out] dst      Destination buffer (NUL-terminated on return).
 * @param[in]  dst_cap  Capacity of @p dst including the trailing NUL.
 *
 * @return Number of characters written to @p dst (excluding NUL).
 *
 * @pre @p dst is non-null and @p dst_cap is at least 1.
 * @pre @p src is either non-null or callers may pass null safely
 *      (helper treats null as empty).
 * @post @p dst is NUL-terminated.
 * @post Returned length is < @p dst_cap.
 *
 * @note Pure function; thread-safe.
 *
 * @par MC/DC:
 * Two compound decisions tested via direct vectors:
 *  - Loop bound (2-cond AND): N+1 = 3 vectors.
 *  - Uppercase test (2-cond AND): N+1 = 3 vectors.
 *
 * @since 0.1.0
 */
std::size_t lowercase_truncated_copy(const char* src, char* dst, std::size_t dst_cap);

/**
 * @brief Test whether @p c is one of the six XML whitespace runes.
 *
 * @details
 * Promoted from line 232 of @c ra_reflow_xml_shim.cpp:
 *   ``c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
 *     || c == '\v'``
 * (6-condition OR). Keeps the host-side MC/DC vector set focused on
 * one helper instead of fighting the parser end-to-end.
 *
 * @param[in] c Character byte.
 *
 * @return true iff @p c is one of the six whitespace runes.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on @p c.
 *
 * @note Pure function; thread-safe.
 *
 * @par MC/DC:
 * 6-condition OR: N+1 = 7 vectors. Each whitespace rune toggles
 * exactly one C_i true and the negative vector (e.g. 'a') is the
 * common all-false control row.
 *
 * @since 0.1.0
 */
bool is_xml_whitespace(char c);

/**
 * @brief Test whether the @c priv_reflow_xml_walk public-API guard
 *        should reject the call.
 *
 * @details
 * Promoted from line 334 of @c ra_reflow_xml_shim.cpp:
 *   ``engine == nullptr || xhtml_buf == nullptr`` (2-condition OR).
 *
 * @param[in] engine_is_null     True iff caller passed a null engine.
 * @param[in] xhtml_buf_is_null  True iff caller passed a null buffer.
 *
 * @return true iff the call must be rejected with k_ra_err_null_ptr.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the two arguments.
 *
 * @note Pure function; thread-safe.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - V1: engine!=null, buf!=null -> false (control)
 *  - V2: engine==null, buf!=null -> true  (varies engine)
 *  - V3: engine!=null, buf==null -> true  (varies buf)
 *
 * @since 0.1.0
 */
bool should_reject_null_args(bool engine_is_null, bool xhtml_buf_is_null);

} /* namespace ra_reflow_xml_shim_internal */
