/**
 * @file xml_decode.c
 * @brief Bounded entity and UTF-8 decoding for the no-heap XML pull reader.
 * @ingroup grp_ereader
 *
 * @details Owns the lexical half of the reader: canonical UTF-8 scalar
 * validation and encoding, the five predefined entities plus decimal and
 * hexadecimal character references, bounded span decoding into caller-owned
 * storage, and the decoded comparisons the event walker layers on top. Nothing
 * here reads reader state; the pull reader itself -- markup scanning, element
 * frames, and event emission -- lives in `xml.c`.
 *
 * [Ring 3 / LIB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "xml.h"
#include "xml_internal.h"

/** @brief Incremental decoded-byte cursor used for allocation-free comparison. */
typedef struct {
  size_t  position;    /**< Next source byte.                    */
  size_t  end;         /**< One-past-last source byte.           */
  uint8_t pending[4];  /**< UTF-8 bytes from the current entity. */
  uint8_t pending_at;  /**< Next pending byte.                   */
  uint8_t pending_len; /**< Pending-byte count.                  */
} priv_decode_cursor_t;

/**
 * @brief Test whether a scalar is an XML 1.0 character.
 * @details Rejects forbidden controls, surrogates, and out-of-range scalars.
 * @param[in] cp Unicode scalar candidate.
 * @return True exactly for an XML 1.0 character.
 * @retval true @p cp is permitted.
 * @retval false @p cp is forbidden.
 * @pre @p cp is represented without narrowing.
 * @pre No normalization or character replacement is requested.
 * @post No memory is modified.
 * @post The result depends only on @p cp.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_xml_char(uint32_t cp)
{
  return (cp == k_priv_xml_tab) || (cp == k_priv_xml_line_feed) ||
         (cp == k_priv_xml_carriage_return) ||
         ((cp >= k_priv_xml_printable_min) && (cp <= k_priv_xml_bmp_first_max)) ||
         ((cp >= k_priv_xml_bmp_second_min) && (cp <= k_priv_xml_bmp_second_max)) ||
         ((cp >= k_priv_xml_supplementary_min) && (cp <= k_priv_xml_scalar_max));
}

/**
 * @brief Classify one UTF-8 lead byte into its scalar width and payload.
 * @details Distinguishes ASCII, 2/3/4-byte lead bytes, and rejects a bare
 * continuation byte used as a lead.
 * @param[in] lead One candidate lead byte.
 * @param[out] out_cp Payload bits decoded so far (lead byte only).
 * @param[out] out_used Total encoded byte width (1..4).
 * @param[out] out_minimum Minimum scalar value the payload must reach.
 * @return Classification status.
 * @retval k_ra8_ok The lead byte is a valid ASCII or multi-byte lead.
 * @retval k_ra8_err_validation_failed The byte is a bare continuation byte.
 * @pre None; @p lead is treated as untrusted input.
 * @pre @p out_cp, @p out_used, and @p out_minimum are writable and do not
 * alias one another.
 * @post On success @p out_used is in [1, 4] and @p out_cp holds the decoded
 * lead-byte payload bits.
 * @post Failure writes none of the three outputs, so a caller that keeps
 * scanning sees its own values rather than a partial classification.
 * @note Thread-safe: pure function over its arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_utf8_lead(uint8_t lead, uint32_t* out_cp, size_t* out_used, uint32_t* out_minimum)
{
  uint32_t cp      = lead;
  size_t   used    = 1U;
  uint32_t minimum = 0U;
  if ((lead >= k_priv_utf8_two_lead_min) && (lead <= k_priv_utf8_two_lead_max)) {
    cp      = (uint32_t)(lead & k_priv_utf8_two_payload_mask);
    used    = 2U;
    minimum = k_priv_utf8_continuation_tag;
  } else if ((lead >= k_priv_utf8_three_lead_min) && (lead <= k_priv_utf8_three_lead_max)) {
    cp      = (uint32_t)(lead & k_priv_utf8_three_payload_mask);
    used    = 3U;
    minimum = k_priv_utf8_three_scalar_min;
  } else if ((lead >= k_priv_utf8_four_lead_min) && (lead <= k_priv_utf8_four_lead_max)) {
    cp      = (uint32_t)(lead & k_priv_utf8_four_payload_mask);
    used    = 4U;
    minimum = k_priv_xml_supplementary_min;
  } else if (lead >= k_priv_utf8_continuation_tag) {
    return k_ra8_err_validation_failed;
  } else {
    /* ASCII lead: the seeded cp/used/minimum above already describe it. */
  }
  *out_cp      = cp;
  *out_used    = used;
  *out_minimum = minimum;
  return k_ra8_ok;
}

/**
 * @brief Decode one canonical UTF-8 scalar.
 * @details Enforces shortest form, valid continuation bytes, and XML characters.
 * @param[in] source Immutable byte source.
 * @param[in] end One-past-last readable byte.
 * @param[in] position Candidate scalar start.
 * @param[out] out_cp Decoded scalar.
 * @param[out] out_used Consumed byte count.
 * @return Repository error code.
 * @retval k_ra8_ok One valid scalar decoded.
 * @retval k_ra8_err_validation_failed Encoding or character was invalid.
 * @pre @p source spans at least @p end readable bytes.
 * @pre Outputs are writable and do not overlap source.
 * @post Success sets both outputs without changing source.
 * @post Failure leaves output values unspecified.
 * @note Allocation-free and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_utf8_next(const uint8_t* source,
                                                 size_t         end,
                                                 size_t         position,
                                                 uint32_t*      out_cp,
                                                 size_t*        out_used)
{
  if (position >= end) {
    return k_ra8_err_validation_failed;
  }
  uint32_t        cp       = 0U;
  size_t          used     = 0U;
  uint32_t        minimum  = 0U;
  const ra8_err_t lead_err = internal_utf8_lead(source[position], &cp, &used, &minimum);
  if (lead_err != k_ra8_ok) {
    return lead_err;
  }
  if ((position + used) > end) {
    return k_ra8_err_validation_failed;
  }
  for (size_t i = 1U; i < used; ++i) {
    const uint8_t byte = source[position + i];
    if ((byte & k_priv_utf8_continuation_mask) != k_priv_utf8_continuation_tag) {
      return k_ra8_err_validation_failed;
    }
    cp = (cp << 6U) | (uint32_t)(byte & k_priv_utf8_scalar_mask);
  }
  if ((cp < minimum) || !internal_xml_char(cp)) {
    return k_ra8_err_validation_failed;
  }
  *out_cp   = cp;
  *out_used = used;
  return k_ra8_ok;
}

bool priv_xml_span_valid(size_t source_len, xml_span_t span)
{
  return ((size_t)span.offset <= source_len) &&
         ((size_t)span.length <= (source_len - (size_t)span.offset));
}

/**
 * @brief Encode one valid Unicode scalar as UTF-8.
 * @details Writes the canonical one-to-four-byte representation.
 * @param[in] cp Valid Unicode scalar.
 * @param[out] out Four-byte destination.
 * @return Number of bytes written.
 * @retval 1 ASCII scalar encoded.
 * @retval 2 Two-byte scalar encoded.
 * @retval 3 Three-byte scalar encoded.
 * @retval 4 Four-byte scalar encoded.
 * @pre @p cp is a Unicode scalar accepted by ::internal_xml_char.
 * @pre @p out spans four writable bytes.
 * @post Exactly the returned prefix is initialized.
 * @post No memory outside @p out is modified.
 * @note Caller validates scalars before invoking this encoder.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_utf8(uint32_t cp, uint8_t out[4])
{
  if (cp < k_priv_utf8_continuation_tag) {
    out[0] = (uint8_t)cp;
    return 1U;
  }
  if (cp < k_priv_utf8_three_scalar_min) {
    out[0] = (uint8_t)(k_priv_utf8_two_lead_tag | (cp >> 6U));
    out[1] = (uint8_t)(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
    return 2U;
  }
  if (cp < k_priv_xml_supplementary_min) {
    out[0] = (uint8_t)(k_priv_utf8_three_lead_tag | (cp >> k_priv_utf8_shift_second));
    out[1] = (uint8_t)(k_priv_utf8_continuation_tag | ((cp >> 6U) & k_priv_utf8_scalar_mask));
    out[2] = (uint8_t)(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
    return 3U;
  }
  out[0] = (uint8_t)(k_priv_utf8_four_lead_tag | (cp >> k_priv_utf8_shift_third));
  out[1] = (uint8_t)(k_priv_utf8_continuation_tag |
                     ((cp >> k_priv_utf8_shift_second) & k_priv_utf8_scalar_mask));
  out[2] = (uint8_t)(k_priv_utf8_continuation_tag | ((cp >> 6U) & k_priv_utf8_scalar_mask));
  out[3] = (uint8_t)(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
  return 4U;
}

/**
 * @brief Convert one numeric-reference digit.
 * @details Supports decimal and hexadecimal reference bodies.
 * @param[in] c Candidate ASCII digit.
 * @param[in] base Numeric base, ten or sixteen.
 * @return Digit value or UINT32_MAX when invalid.
 * @retval UINT32_MAX @p c is invalid for @p base.
 * @pre @p base is 10 or 16.
 * @pre @p c is an unsigned source byte.
 * @post No memory is modified.
 * @post The result depends only on the arguments.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_digit(uint8_t c, uint32_t base)
{
  const uint32_t value = (uint32_t)c;
  if ((c >= (uint8_t)'0') && (c <= (uint8_t)'9')) {
    return value - (uint32_t)(uint8_t)'0';
  }
  if ((base == 16U) && (c >= (uint8_t)'a') && (c <= (uint8_t)'f')) {
    return (value - (uint32_t)(uint8_t)'a') + k_priv_xml_decimal_base;
  }
  if ((base == 16U) && (c >= (uint8_t)'A') && (c <= (uint8_t)'F')) {
    return (value - (uint32_t)(uint8_t)'A') + k_priv_xml_decimal_base;
  }
  return UINT32_MAX;
}

/**
 * @brief Compare bounded source bytes against a literal of known length.
 * @details Walks both operands one byte at a time so the comparison never
 * hands an essentially-character operand to `memcmp()`, whose ordering is
 * implementation-defined for plain `char` (MISRA-C:2012 Rules 21.14, 21.16).
 * Only equality is ever asked of this function, so the loop is the whole
 * contract.
 * @param[in] source Immutable source bytes.
 * @param[in] offset First source byte to compare.
 * @param[in] literal Literal whose first @p length bytes are compared.
 * @param[in] length Byte count to compare.
 * @return True exactly when the two byte runs are equal.
 * @retval true Every compared byte matched.
 * @retval false At least one byte differed.
 * @pre @p source spans at least `offset + length` readable bytes.
 * @pre @p literal spans at least @p length readable bytes.
 * @post No memory is modified.
 * @post The result depends only on the arguments.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool
priv_xml_bytes_equal(const uint8_t* source, size_t offset, const char* literal, size_t length)
{
  for (size_t i = 0U; i < length; ++i) {
    if (source[offset + i] != (uint8_t)literal[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Decode one entity beginning at a bounded position.
 * @details Accepts five predefined or decimal/hex numeric references only.
 * @param[in] source Immutable source bytes.
 * @param[in] end One-past-last readable entity byte.
 * @param[in] position Offset of the leading ampersand.
 * @param[out] out_cp Decoded XML character.
 * @param[out] out_used Encoded bytes consumed.
 * @return Repository error code.
 * @retval k_ra8_ok Entity decoded.
 * @retval k_ra8_err_validation_failed Entity or character was invalid.
 * @pre @p source spans at least @p end readable bytes.
 * @pre Outputs are writable and non-overlapping with source.
 * @post Success sets both outputs and leaves source unchanged.
 * @post Failure leaves outputs unspecified.
 * @note Named entity expansion beyond the five predefined names is unsupported.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_entity(const uint8_t* source,
                                              size_t         end,
                                              size_t         position,
                                              uint32_t*      out_cp,
                                              size_t*        out_used)
{
  static const char* const names[] = {"amp;", "lt;", "gt;", "quot;", "apos;"};
  static const uint32_t    cps[]   = {'&', '<', '>', '"', '\''};
  if ((position + 2U) >= end) {
    return k_ra8_err_validation_failed;
  }
  for (size_t i = 0U; i < (sizeof(names) / sizeof(names[0])); ++i) {
    const size_t length = strlen(names[i]);
    if (((position + 1U + length) <= end) &&
        priv_xml_bytes_equal(source, position + 1U, names[i], length)) {
      *out_cp   = cps[i];
      *out_used = length + 1U;
      return k_ra8_ok;
    }
  }
  if (source[position + 1U] != (uint8_t)'#') {
    return k_ra8_err_validation_failed;
  }
  size_t   cursor = position + 2U;
  uint32_t base   = k_priv_xml_decimal_base;
  // mcdc-deactivated: internal_entity radix probe; the `(position + 2U) >= end` precheck at the top of this function has already returned for every shorter span, so `cursor == position + 2U` is strictly less than `end` on every reachable path and the bound condition is constant-true.
  if ((cursor < end) && ((source[cursor] == (uint8_t)'x') || (source[cursor] == (uint8_t)'X'))) {
    base = 16U;
    ++cursor;
  }
  const size_t first_digit = cursor;
  uint32_t     cp          = 0U;
  while ((cursor < end) && (source[cursor] != (uint8_t)';')) {
    const uint32_t digit = internal_digit(source[cursor], base);
    if ((digit >= base) || (cp > ((k_priv_xml_scalar_max - digit) / base))) {
      return k_ra8_err_validation_failed;
    }
    cp = (cp * base) + digit;
    ++cursor;
  }
  if ((cursor == first_digit) || (cursor >= end) || !internal_xml_char(cp)) {
    return k_ra8_err_validation_failed;
  }
  *out_cp   = cp;
  *out_used = (cursor - position) + 1U;
  return k_ra8_ok;
}

/**
 * @brief Decode one character or entity and append it to the destination.
 * @details Decodes one XML character reference/entity, or one raw UTF-8
 * character, at @p cursor, then copies its UTF-8 bytes into @p destination,
 * clipping (or rejecting, per @p truncate) once capacity is reached.
 * @param[in] source Immutable validated XML source.
 * @param[in] end Exclusive end offset of the decodable span.
 * @param[in] cursor Current source offset to decode from.
 * @param[in,out] destination Caller-owned destination buffer, or NULL to
 * measure only.
 * @param[in] capacity Writable capacity of @p destination in bytes.
 * @param[in] truncate Whether to clip rather than reject on overflow.
 * @param[in,out] output Running output byte count.
 * @param[in,out] clipped Whether the destination has already been clipped.
 * @param[out] out_used Source bytes consumed by this character or entity.
 * @return Decode status.
 * @retval k_ra8_ok One character or entity was decoded and appended.
 * @retval k_ra8_err_no_mem Capacity was exhausted and @p truncate is false.
 * @retval k_ra8_err_validation_failed The entity or UTF-8 sequence is malformed.
 * @pre @p cursor is within the decodable span, strictly less than @p end.
 * @pre @p output and @p clipped carry the running state of the enclosing
 * span walk and do not alias @p source.
 * @post Success advances @p output (unless clipped) and reports @p out_used.
 * @post A clip NUL-terminates @p destination and latches @p clipped, so every
 * later character is measured but never stored.
 * @note Not thread-safe: mutates caller-owned scratch state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode_one(const uint8_t* source,
                                                  size_t         end,
                                                  size_t         cursor,
                                                  char*          destination,
                                                  size_t         capacity,
                                                  bool           truncate,
                                                  size_t*        output,
                                                  bool*          clipped,
                                                  size_t*        out_used)
{
  uint8_t bytes[4] = {};
  size_t  count    = 0U;
  size_t  used     = 0U;
  if (source[cursor] == (uint8_t)'&') {
    uint32_t  cp  = 0U;
    ra8_err_t err = internal_entity(source, end, cursor, &cp, &used);
    if (err != k_ra8_ok) {
      return err;
    }
    count = internal_utf8(cp, bytes);
  } else {
    uint32_t        cp  = 0U;
    const ra8_err_t err = internal_utf8_next(source, end, cursor, &cp, &used);
    if (err != k_ra8_ok) {
      return err;
    }
    count = used;
    (void)memcpy(bytes, &source[cursor], count);
  }
  if ((destination != nullptr) && !*clipped && ((*output + count) >= capacity)) {
    if (!truncate) {
      return k_ra8_err_no_mem;
    }
    *clipped             = true;
    destination[*output] = '\0';
  }
  if ((destination != nullptr) && !*clipped) {
    (void)memcpy(&destination[*output], bytes, count);
  }
  if ((destination == nullptr) || !*clipped) {
    *output += count;
  }
  *out_used = used;
  return k_ra8_ok;
}

/**
 * @brief Decode, prefix-decode, or measure one already-bounded span.
 * @details Validates the complete span even when prefix output clips.
 * @param[in] source Immutable source containing @p span.
 * @param[in] span Valid source-relative encoded span.
 * @param[out] destination Optional decoded destination.
 * @param[in] capacity Writable destination capacity including NUL.
 * @param[in] truncate Permit a maximal complete prefix when capacity is short.
 * @param[out] out_length Decoded or emitted byte count.
 * @return Repository error code.
 * @retval k_ra8_ok Complete validation/decode succeeded.
 * @retval k_ra8_err_no_mem Non-truncating destination capacity was insufficient.
 * @retval k_ra8_err_validation_failed UTF-8 or entity was invalid.
 * @pre Caller proved @p span lies in the source extent.
 * @pre Non-NULL destination spans @p capacity writable bytes.
 * @post Success reports an exact count and NUL-terminates non-NULL destination.
 * @post Failure leaves source unchanged; destination may contain a prefix.
 * @note Output and source must not overlap.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode(const uint8_t* source,
                                              xml_span_t     span,
                                              char*          destination,
                                              size_t         capacity,
                                              bool           truncate,
                                              size_t*        out_length)
{
  size_t       output  = 0U;
  bool         clipped = false;
  const size_t end     = (size_t)span.offset + (size_t)span.length;
  if ((destination != nullptr) && (capacity == 0U)) {
    return k_ra8_err_no_mem;
  }
  for (size_t cursor = span.offset; cursor < end;) {
    size_t          used = 0U;
    const ra8_err_t err  = internal_decode_one(source,
                                               end,
                                               cursor,
                                               destination,
                                               capacity,
                                               truncate,
                                               &output,
                                               &clipped,
                                               &used);
    if (err != k_ra8_ok) {
      return err;
    }
    cursor += used;
  }
  if (destination != nullptr) {
    destination[output] = '\0';
  }
  *out_length = output;
  return k_ra8_ok;
}

/**
 * @brief Return one entity-decoded byte from a comparison cursor.
 * @details Buffers remaining bytes when one entity decodes to multibyte UTF-8.
 * @param[in] source Immutable source bytes.
 * @param[in,out] cursor Valid decoded-span cursor.
 * @param[out] out Next decoded byte.
 * @return Repository error code.
 * @retval k_ra8_ok One byte returned and cursor advanced.
 * @retval k_ra8_err_validation_failed Cursor ended or entity was invalid.
 * @pre Cursor bounds lie within the readable source.
 * @pre @p out is writable and does not overlap cursor/source.
 * @post Success advances exactly one decoded byte.
 * @post Failure leaves source unchanged; cursor progress may be partial.
 * @note Used only after complete decoded-size validation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_decoded_byte(const uint8_t* source, priv_decode_cursor_t* cursor, uint8_t* out)
{
  if (cursor->pending_at < cursor->pending_len) {
    *out = cursor->pending[cursor->pending_at];
    cursor->pending_at++;
    return k_ra8_ok;
  }
  if (cursor->position >= cursor->end) {
    return k_ra8_err_validation_failed;
  }
  if (source[cursor->position] != (uint8_t)'&') {
    *out = source[cursor->position];
    cursor->position++;
    return k_ra8_ok;
  }
  uint32_t        cp   = 0U;
  size_t          used = 0U;
  const ra8_err_t err  = internal_entity(source, cursor->end, cursor->position, &cp, &used);
  if (err != k_ra8_ok) {
    return err;
  }
  cursor->position += used;
  cursor->pending_len = (uint8_t)internal_utf8(cp, cursor->pending);
  cursor->pending_at  = 1U;
  *out                = cursor->pending[0];
  return k_ra8_ok;
}

ra8_err_t xml_decode(const uint8_t* source,
                     size_t         source_len,
                     xml_span_t     span,
                     char*          destination,
                     size_t         capacity,
                     size_t*        out_length)
{
  if ((source == nullptr) || (destination == nullptr) || (out_length == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!priv_xml_span_valid(source_len, span)) {
    return k_ra8_err_validation_failed;
  }
  return internal_decode(source, span, destination, capacity, false, out_length);
}

ra8_err_t xml_decode_prefix(const uint8_t* source,
                            size_t         source_len,
                            xml_span_t     span,
                            char*          destination,
                            size_t         capacity,
                            size_t*        out_length)
{
  if ((source == nullptr) || (destination == nullptr) || (out_length == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!priv_xml_span_valid(source_len, span)) {
    return k_ra8_err_validation_failed;
  }
  return internal_decode(source, span, destination, capacity, true, out_length);
}

ra8_err_t
xml_decoded_size(const uint8_t* source, size_t source_len, xml_span_t span, size_t* out_length)
{
  if ((source == nullptr) || (out_length == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!priv_xml_span_valid(source_len, span)) {
    return k_ra8_err_validation_failed;
  }
  return internal_decode(source, span, nullptr, 0U, false, out_length);
}

bool xml_span_equal(const uint8_t* source, size_t source_len, xml_span_t span, const char* literal)
{
  if ((source == nullptr) || (literal == nullptr) || !priv_xml_span_valid(source_len, span)) {
    return false;
  }
  const size_t length = strlen(literal);
  return ((size_t)span.length == length) &&
         priv_xml_bytes_equal(source, (size_t)span.offset, literal, length);
}

bool xml_span_local_equal(const uint8_t* source,
                          size_t         source_len,
                          xml_span_t     span,
                          const char*    literal)
{
  if ((source == nullptr) || !priv_xml_span_valid(source_len, span)) {
    return false;
  }
  uint32_t offset = span.offset;
  uint32_t length = span.length;
  for (uint32_t i = 0U; i < span.length; ++i) {
    if (source[span.offset + i] == (uint8_t)':') {
      offset = span.offset + i + 1U;
      length = span.length - i - 1U;
    }
  }
  return xml_span_equal(source, source_len, (xml_span_t){offset, length}, literal);
}

bool xml_decoded_equal(const uint8_t* source, size_t source_len, xml_span_t left, xml_span_t right)
{
  if (source == nullptr) {
    return false;
  }
  size_t left_size  = 0U;
  size_t right_size = 0U;
  if ((xml_decoded_size(source, source_len, left, &left_size) != k_ra8_ok) ||
      (xml_decoded_size(source, source_len, right, &right_size) != k_ra8_ok) ||
      (left_size != right_size)) {
    return false;
  }
  priv_decode_cursor_t li = {.position = left.offset, .end = (size_t)left.offset + left.length};
  priv_decode_cursor_t ri = {.position = right.offset, .end = (size_t)right.offset + right.length};
  for (size_t i = 0U; i < left_size; ++i) {
    uint8_t lb = 0U;
    uint8_t rb = 0U;
    // mcdc-deactivated: xml_decoded_equal byte-walk status guards; xml_decoded_size already decoded both spans end to end and reported exactly left_size/right_size bytes, so for every i < left_size both internal_decoded_byte() calls are re-walking bytes proven decodable -- only the byte-inequality condition can flip.
    if ((internal_decoded_byte(source, &li, &lb) != k_ra8_ok) ||
        (internal_decoded_byte(source, &ri, &rb) != k_ra8_ok) || (lb != rb)) {
      return false;
    }
  }
  return true;
}

ra8_err_t priv_xml_raw(const uint8_t* source, size_t start, size_t end)
{
  size_t cursor = start;
  while (cursor < end) {
    uint32_t        cp   = 0U;
    size_t          used = 0U;
    const ra8_err_t err  = internal_utf8_next(source, end, cursor, &cp, &used);
    if (err != k_ra8_ok) {
      return err;
    }
    cursor += used;
  }
  return k_ra8_ok;
}
