/**
 * @file xml_doctype.c
 * @brief Bounded external-only DOCTYPE lexer for the no-heap XML reader.
 * @ingroup grp_ereader
 *
 * @details Accepts and ignores lexically valid bare, SYSTEM, and PUBLIC
 * declarations before the root without resolving external resources; internal
 * subsets and unsupported declaration forms fail closed.
 *
 * [Ring 3 / LIB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xml_internal.h"

/**
 * @brief Test whether one byte is XML spacing inside a DOCTYPE.
 * @details Recognises the four XML S characters without locale state.
 * @param[in] byte Byte to classify.
 * @return True exactly for XML spacing.
 * @retval true @p byte is XML spacing.
 * @retval false @p byte is not XML spacing.
 * @pre @p byte is an unsigned source byte.
 * @pre Locale-dependent ctype state is not consulted.
 * @post No memory is modified.
 * @post The result depends only on @p byte.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_space(uint8_t byte)
{
  return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') || (byte == (uint8_t)'\n') ||
         (byte == (uint8_t)'\r');
}

/**
 * @brief Skip XML spacing and report whether at least one byte was consumed.
 * @details Advances only inside the supplied half-open byte range.
 * @param[in] source Immutable XML source.
 * @param[in] end One-past-last readable byte for this declaration.
 * @param[in,out] position Current scan offset.
 * @return True when at least one spacing byte was skipped.
 * @retval true Position advanced.
 * @retval false No XML spacing began at the position.
 * @pre @p source spans at least @p end bytes.
 * @pre `*position <= end` and @p position is writable.
 * @post Position never exceeds @p end.
 * @post Source bytes remain unchanged.
 * @note Pure with respect to source bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_skip_space(const uint8_t* source, size_t end, size_t* position)
{
  const size_t initial = *position;
  while ((*position < end) && internal_space(source[*position])) {
    ++*position;
  }
  return *position != initial;
}

/**
 * @brief Test the XML PubidChar subset accepted in public identifiers.
 * @details Accepts XML spacing, alphanumerics, and XML punctuation.
 * @param[in] byte Byte to classify.
 * @return True exactly for one supported PubidChar.
 * @retval true @p byte is permitted.
 * @retval false @p byte is forbidden.
 * @pre @p byte is an unsigned source byte.
 * @pre Public identifiers use XML's ASCII PubidChar set.
 * @post No memory is modified.
 * @post The result depends only on @p byte.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_pubid_byte(uint8_t byte)
{
  static const char punctuation[] = "-'()+,./:=?;!*#@$_%";
  return (byte == (uint8_t)' ') || (byte == (uint8_t)'\r') || (byte == (uint8_t)'\n') ||
         ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'Z')) ||
         ((byte >= (uint8_t)'a') && (byte <= (uint8_t)'z')) ||
         ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9')) ||
         ((byte != 0U) && (strchr(punctuation, (int)byte) != nullptr));
}

/**
 * @brief Parse one quoted system or public identifier literal.
 * @details Respects either quote and validates optional PubidChar bytes.
 * @param[in] source Immutable XML source.
 * @param[in] end One-past-last readable declaration byte.
 * @param[in,out] position Offset of the opening quote, then byte after closure.
 * @param[in] public_id Require XML PubidChar bytes when true.
 * @return Repository error code.
 * @retval k_ra8_ok Literal validated and consumed.
 * @retval k_ra8_err_validation_failed Quote, byte, or terminator was invalid.
 * @pre @p source spans at least @p end bytes.
 * @pre `*position <= end` and @p position is writable.
 * @post Success advances after the matching closing quote.
 * @post Failure leaves source unchanged; position may be partial.
 * @note No resource named by the literal is fetched.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_literal(const uint8_t* source, size_t end, size_t* position, bool public_id)
{
  if ((*position >= end) ||
      ((source[*position] != (uint8_t)'\'') && (source[*position] != (uint8_t)'"'))) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t quote = source[*position];
  *position += 1U;
  const size_t start = *position;
  while ((*position < end) && (source[*position] != quote)) {
    if (public_id && !internal_pubid_byte(source[*position])) {
      return k_ra8_err_validation_failed;
    }
    ++*position;
  }
  if (*position >= end) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t err = public_id ? k_ra8_ok : priv_xml_raw(source, start, *position);
  ++*position;
  return err;
}

/**
 * @brief Consume an exact ASCII keyword followed by required spacing.
 * @details Matches case-sensitively without reading beyond @p end.
 * @param[in] source Immutable XML source.
 * @param[in] end One-past-last readable declaration byte.
 * @param[in,out] position Current keyword offset.
 * @param[in] keyword NUL-terminated expected ASCII keyword.
 * @return Repository error code.
 * @retval k_ra8_ok Keyword and following XML spacing consumed.
 * @retval k_ra8_err_validation_failed Bytes or required spacing did not match.
 * @pre @p source spans at least @p end bytes.
 * @pre @p position and @p keyword are valid inputs.
 * @post Success advances beyond at least one spacing byte.
 * @post Failure leaves source unchanged; position may be partial.
 * @note Matching is intentionally case-sensitive.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_keyword(const uint8_t* source, size_t end, size_t* position, const char* keyword)
{
  const size_t length = strlen(keyword);
  // mcdc-deactivated: internal_keyword prefix recheck; the only caller, internal_external_id, has already proved `(*position + 6U) <= end` and matched the same six bytes before selecting the keyword, and both keywords are exactly six bytes long, so both conditions are constant-false on every reachable path.
  if (((*position + length) > end) || !priv_xml_bytes_equal(source, *position, keyword, length)) {
    return k_ra8_err_validation_failed;
  }
  *position += length;
  return internal_skip_space(source, end, position) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Parse a SYSTEM or PUBLIC external identifier.
 * @details Accepts SYSTEM literal or PUBLIC identifier plus system literal.
 * @param[in] source Immutable XML source.
 * @param[in] end One-past-last readable declaration byte.
 * @param[in,out] position Offset at the external identifier keyword.
 * @return Repository error code.
 * @retval k_ra8_ok One valid external identifier consumed.
 * @retval k_ra8_err_validation_failed Identifier grammar was malformed.
 * @pre @p source spans at least @p end bytes.
 * @pre `*position <= end` and @p position is writable.
 * @post Success leaves position after the final literal.
 * @post Failure leaves source unchanged; position may be partial.
 * @note External identifiers are checked but never resolved.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_external_id(const uint8_t* source, size_t end, size_t* position)
{
  if (((*position + 6U) <= end) && priv_xml_bytes_equal(source, *position, "SYSTEM", 6U)) {
    ra8_err_t err = internal_keyword(source, end, position, "SYSTEM");
    return (err == k_ra8_ok) ? internal_literal(source, end, position, false) : err;
  }
  if (((*position + 6U) <= end) && priv_xml_bytes_equal(source, *position, "PUBLIC", 6U)) {
    ra8_err_t err = internal_keyword(source, end, position, "PUBLIC");
    if (err == k_ra8_ok) {
      err = internal_literal(source, end, position, true);
    }
    if ((err == k_ra8_ok) && !internal_skip_space(source, end, position)) {
      err = k_ra8_err_validation_failed;
    }
    return (err == k_ra8_ok) ? internal_literal(source, end, position, false) : err;
  }
  return k_ra8_err_validation_failed;
}

ra8_err_t priv_xml_doctype(xml_reader_t* reader)
{
  static const char prefix[] = "<!DOCTYPE";
  const size_t      end      = reader->source_len;
  size_t            position = reader->position;
  if ((reader->root_count != 0U) || (reader->doctype_seen != 0U) ||
      ((position + sizeof(prefix) - 1U) > end) ||
      !priv_xml_bytes_equal(reader->source, position, prefix, sizeof(prefix) - 1U)) {
    return k_ra8_err_validation_failed;
  }
  position += sizeof(prefix) - 1U;
  if (!internal_skip_space(reader->source, end, &position)) {
    return k_ra8_err_validation_failed;
  }
  size_t name_end = 0U;
  if (priv_xml_qname(reader->source, end, position, &name_end) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  position             = name_end;
  const bool separated = internal_skip_space(reader->source, end, &position);
  if ((position < end) && (reader->source[position] != (uint8_t)'>')) {
    if (!separated || (internal_external_id(reader->source, end, &position) != k_ra8_ok)) {
      return k_ra8_err_validation_failed;
    }
    (void)internal_skip_space(reader->source, end, &position);
  }
  if ((position >= end) || (reader->source[position] != (uint8_t)'>')) {
    return k_ra8_err_validation_failed;
  }
  reader->doctype_seen = 1U;
  reader->position     = position + 1U;
  return k_ra8_ok;
}
