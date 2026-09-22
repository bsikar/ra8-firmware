/**
 * @file mdl_extract_internal.h
 * @brief Shared bounded markup lexer helpers for media extraction units.
 * @details Centralizes allocation-free ASCII matching, attribute parsing, and
 *          bounded string copies used by the image and titled-anchor scanners.
 *          Every definition is internal inline code in the including unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <stddef.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief ASCII lower-case (locale-independent).
 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] c Input ASCII byte.
 * @return Lower-case ASCII mapping of @p c, or @p c unchanged outside `A-Z`.
 * @retval 0 The input character was NUL.
 * @retval other Lower-case mapping or unchanged input character.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline char internal_lc(char c)
{
  /* The conditional operator promotes both arms to int, so the cast belongs
   * on the whole expression; both arms are already char-valued. */
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/** @brief True if `c` ends an HTML attribute token on its left side.
 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] c Input ASCII byte.
 * @return True when @p c may delimit the left edge of an attribute.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool internal_is_attr_boundary(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '"') || (c == '\'') ||
         (c == '<');
}

/** @brief True if `c` terminates a tag name (so "<a" != "<article").
 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] c Input ASCII byte.
 * @return True when @p c terminates an HTML tag name.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool internal_is_name_end(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '>') || (c == '/');
}

/** @brief Case-insensitive search for `needle` in [hay, hay+hay_len). */
RA8_INTERNAL static inline const char*
internal_find_ci(const char* hay, size_t hay_len, const char* needle)
{
  const size_t nlen = strlen(needle);
  if ((nlen == 0U) || (hay_len < nlen)) {
    return nullptr;
  }
  for (size_t i = 0U; i + nlen <= hay_len; ++i) {
    size_t j = 0U;
    while ((j < nlen) && (internal_lc(hay[i + j]) == internal_lc(needle[j]))) {
      ++j;
    }
    if (j == nlen) {
      return hay + i;
    }
  }
  return nullptr;
}

/** @brief Advance past spaces/tabs in [k, tag_len).
 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[in,out] k Current bounded scan offset.
 * @return First offset at or after @p k that is not a space or tab.
 * @retval tag_len Only whitespace remains through the end of the span.
 * @retval other First non-whitespace offset in the span.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline size_t internal_skip_ws(const char* tag, size_t tag_len, size_t k)
{
  while ((k < tag_len) && ((tag[k] == ' ') || (tag[k] == '\t'))) {
    ++k;
  }
  return k;
}

/**
 * @brief From position `k` just after an attr name, read `="value"`.
 * @return true and fills `out` (NUL-terminated) if a quoted value was parsed.

 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[in,out] k Current bounded scan offset.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool
internal_read_eq_value(const char* tag, size_t tag_len, size_t k, char* out, size_t out_cap)
{
  k = internal_skip_ws(tag, tag_len, k);
  if (!((k < tag_len) && (tag[k] == '='))) {
    return false;
  }
  k = internal_skip_ws(tag, tag_len, k + 1U);
  if (!((k < tag_len) && ((tag[k] == '"') || (tag[k] == '\'')))) {
    return false;
  }
  const char quote = tag[k];
  ++k;
  size_t n = 0U;
  while ((k < tag_len) && (tag[k] != quote) && (n + 1U < out_cap)) {
    out[n] = tag[k];
    ++n;
    ++k;
  }
  if ((k < tag_len) && (tag[k] == quote)) {
    out[n] = '\0';
    return true;
  }
  return false;
}

/**
 * @brief Read the value of `attr` inside one tag [tag, tag+tag_len).
 * @return true and fills `out` (NUL-terminated) if a quoted value was found.

 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[in] attr Attribute name to locate.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool internal_find_attr_value(const char* tag,
                                                         size_t      tag_len,
                                                         const char* attr,
                                                         char*       out,
                                                         size_t      out_cap)
{
  const size_t alen = strlen(attr);
  size_t       pos  = 0U;
  while (pos + alen <= tag_len) {
    const char* hit = internal_find_ci(tag + pos, tag_len - pos, attr);
    if (hit == nullptr) {
      return false;
    }
    const size_t idx     = (size_t)(hit - tag);
    const bool   left_ok = (idx == 0U) || internal_is_attr_boundary(tag[idx - 1U]);
    if (left_ok && internal_read_eq_value(tag, tag_len, idx + alen, out, out_cap)) {
      return true;
    }
    pos = idx + alen;
  }
  return false;
}

/** @brief True if `needle` is empty/NULL or a substring of `url`.
 * @details Applies the optional substring filter without modifying either input.
 *          Both NUL-terminated inputs remain borrowed for the call.
 * @param[in] url NUL-terminated URL input.
 * @param[in] needle Optional substring filter.
 * @return True when @p needle is absent, empty, or occurs in @p url.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool internal_contains_ok(const char* url, const char* needle)
{
  if ((needle == nullptr) || (needle[0] == '\0')) {
    return true;
  }
  return strstr(url, needle) != nullptr;
}

/** @brief Copy `src` into `out` if it fits (incl. NUL).
 * @details Applies locale-independent HTML token rules to caller-owned text.
 *          Offsets and copies remain inside supplied spans and capacities.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @param[in] src Readable source bytes.
 * @return True when @p src and its terminator fit completely in @p out.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline bool internal_copy_fits(char* out, size_t out_cap, const char* src)
{
  const size_t n = strlen(src);
  if (n + 1U > out_cap) {
    return false;
  }
  memcpy(out, src, n + 1U);
  return true;
}
