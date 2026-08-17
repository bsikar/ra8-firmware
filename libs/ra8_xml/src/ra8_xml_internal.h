/**
 * @file ra8_xml_internal.h
 * @brief Private lexical seams shared by the bounded XML reader.
 * @ingroup grp_ereader
 *
 * @details Exposes the scalar geometry plus the span, QName, raw UTF-8, and
 * external-only DOCTYPE validators shared across the reader's implementation
 * translation units.
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

/** @brief XML 1.0 scalar bounds and canonical UTF-8 bit geometry. */
typedef enum : uint32_t {
  k_priv_xml_tab                 = 0x09U,     /**< XML tab character.                  */
  k_priv_xml_line_feed           = 0x0AU,     /**< XML line-feed character.            */
  k_priv_xml_carriage_return     = 0x0DU,     /**< XML carriage-return character.      */
  k_priv_xml_printable_min       = 0x20U,     /**< First ordinary XML character.       */
  k_priv_xml_bmp_first_max       = 0xD7FFU,   /**< Last scalar before surrogates.      */
  k_priv_xml_bmp_second_min      = 0xE000U,   /**< First scalar after surrogates.      */
  k_priv_xml_bmp_second_max      = 0xFFFDU,   /**< Last permitted BMP scalar.          */
  k_priv_xml_supplementary_min   = 0x10000U,  /**< First supplementary scalar.         */
  k_priv_xml_scalar_max          = 0x10FFFFU, /**< Last Unicode scalar.                */
  k_priv_utf8_two_lead_min       = 0xC2U,     /**< First canonical two-byte lead.      */
  k_priv_utf8_two_lead_max       = 0xDFU,     /**< Last two-byte lead.                 */
  k_priv_utf8_two_payload_mask   = 0x1FU,     /**< Payload bits in a two-byte lead.    */
  k_priv_utf8_three_lead_min     = 0xE0U,     /**< First three-byte lead.              */
  k_priv_utf8_three_lead_max     = 0xEFU,     /**< Last three-byte lead.               */
  k_priv_utf8_three_payload_mask = 0x0FU,     /**< Payload bits in a three-byte lead.  */
  k_priv_utf8_three_scalar_min   = 0x800U,    /**< First scalar needing three bytes.   */
  k_priv_utf8_four_lead_min      = 0xF0U,     /**< First four-byte lead.               */
  k_priv_utf8_four_lead_max      = 0xF4U,     /**< Last canonical four-byte lead.      */
  k_priv_utf8_four_payload_mask  = 0x07U,     /**< Payload bits in a four-byte lead.   */
  k_priv_utf8_continuation_mask  = 0xC0U,     /**< Continuation tag mask.              */
  k_priv_utf8_continuation_tag   = 0x80U,     /**< Continuation tag and ASCII ceiling. */
  k_priv_utf8_scalar_mask        = 0x3FU,     /**< Scalar bits per continuation byte.  */
  k_priv_utf8_two_lead_tag       = 0xC0U,     /**< Encoded two-byte lead tag.          */
  k_priv_utf8_three_lead_tag     = 0xE0U,     /**< Encoded three-byte lead tag.        */
  k_priv_utf8_four_lead_tag      = 0xF0U,     /**< Encoded four-byte lead tag.         */
  k_priv_utf8_shift_second       = 12U,       /**< Shift for the second payload group. */
  k_priv_utf8_shift_third        = 18U,       /**< Shift for the third payload group.  */
  k_priv_xml_decimal_base        = 10U,       /**< Numeric-entity decimal radix.       */
  k_priv_xml_encoding_bytes      = 5U,        /**< Bytes in the UTF-8 encoding label.  */
  k_priv_xml_cdata_open_bytes    = 9U,        /**< Bytes in the CDATA opener.          */
  k_priv_xml_doctype_open_bytes  = 9U,        /**< Bytes in the DOCTYPE opener.        */
  k_priv_utf8_bom_first          = 0xEFU,     /**< First UTF-8 BOM byte.               */
  k_priv_utf8_bom_second         = 0xBBU,     /**< Second UTF-8 BOM byte.              */
  k_priv_utf8_bom_third          = 0xBFU,     /**< Third UTF-8 BOM byte.               */
} priv_xml_encoding_t;

/**
 * @brief Check that a source-relative span is in range.
 * @details Uses subtraction after checking the offset to avoid overflow.
 * @param[in] source_len Exact source byte extent.
 * @param[in] span Candidate source-relative span.
 * @return True only when the complete span lies in the extent.
 * @retval true Offset and length are bounded.
 * @retval false The span is forged, stale, or out of range.
 * @pre @p source_len is the true readable extent.
 * @pre @p span uses the same source-relative coordinate system.
 * @post No memory is modified.
 * @post The result is overflow-safe.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool priv_ra8_xml_span_valid(size_t source_len, ra8_xml_span_t span);

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
