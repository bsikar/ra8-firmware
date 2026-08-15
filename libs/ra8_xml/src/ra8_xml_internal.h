/**
 * @file ra8_xml_internal.h
 * @brief Private lexical seams shared by the bounded XML reader.
 * @ingroup grp_ereader
 *
 * @details Exposes only the QName, raw UTF-8, and external-only DOCTYPE
 * validators shared across the reader's implementation translation units.
 *
 * [Ring 3 / LIB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_xml.h"

/**
 * @brief Scan the supported QName subset over a bounded byte range.
 * @details Accepts one or two ASCII NCName components separated by one colon.
 * @param[in] source Immutable XML source.
 * @param[in] end One-past-last readable lexical byte.
 * @param[in] start Candidate QName start offset.
 * @param[out] out_end One-past-last accepted QName byte.
 * @return Repository error code.
 * @retval k_ra8_ok At least one valid QName component was consumed.
 * @retval k_ra8_err_validation_failed Start or namespace spelling was invalid.
 * @pre @p source spans at least @p end readable bytes.
 * @pre @p out_end is writable and does not overlap source.
 * @post Success sets an offset in `[start + 1, end]`.
 * @post Failure leaves source unchanged and output unspecified.
 * @note This is the documented ASCII QName subset, not full XML NameStartChar.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_xml_qname(const uint8_t* source,
                                      size_t         end,
                                      size_t         start,
                                      size_t*        out_end);

/**
 * @brief Validate canonical UTF-8 XML 1.0 characters over a byte range.
 * @details Rejects overlong encoding, invalid continuations, controls, and surrogates.
 * @param[in] source Immutable XML source.
 * @param[in] start First byte to validate.
 * @param[in] end One-past-last byte to validate.
 * @return Repository error code.
 * @retval k_ra8_ok Every byte belongs to a permitted canonical scalar.
 * @retval k_ra8_err_validation_failed Encoding or XML character was invalid.
 * @pre @p source spans at least @p end readable bytes.
 * @pre `start <= end` describes a half-open range.
 * @post Source bytes remain unchanged.
 * @post Success validates the complete range, not a prefix.
 * @note Allocation-free and thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_xml_raw(const uint8_t* source, size_t start, size_t end);

/**
 * @brief Validate and skip one supported pre-root external DOCTYPE.
 * @details Accepts bare, SYSTEM, or PUBLIC external-only forms and fetches nothing.
 * @param[in,out] reader Active reader positioned at `<!DOCTYPE`.
 * @return Repository error code.
 * @retval k_ra8_ok Supported declaration consumed.
 * @retval k_ra8_err_validation_failed Placement, subset, or grammar was invalid.
 * @pre Reader source/position describe a complete bounded document.
 * @pre No root or prior DOCTYPE has been consumed.
 * @post Success advances after `>` and marks one DOCTYPE seen.
 * @post Failure leaves immutable source bytes unchanged.
 * @note Internal subsets and entity declarations fail closed.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_xml_doctype(ra8_xml_reader_t* reader);
