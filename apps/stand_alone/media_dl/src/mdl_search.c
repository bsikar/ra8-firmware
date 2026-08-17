/**
 * @file mdl_search.c
 * @brief Pure search/discovery policy: percent-encoding, URL templating, and
 *        the zero-vs-broken result classifier.
 *
 * @details Provides bounded query encoding and template expansion plus the
 * result-list policy that distinguishes an honest empty result from changed
 * markup. Filtering compacts caller-owned fixed storage in place and preserves
 * discovery order.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_search.h"

#include <string.h>

#include "ra8_attributes.h"

/** @brief Query-encoding constants. */
typedef enum : uint8_t {
  k_hi_nibble_shift = 4U,    /**< Bits to shift for the high hex nibble. */
  k_nibble_mask     = 0x0FU, /**< Low-nibble mask.                       */
  k_triplet_len     = 3U,    /**< Bytes a `%HH` escape occupies.         */
} mdl_search_const_t;

/** @brief The `{q}` placeholder token, kept in one place. */
static const char s_placeholder[] = "{q}";

const char* mdl_search_placeholder(void)
{
  return s_placeholder;
}

/**
 * @brief True for an RFC 3986 unreserved byte (copied verbatim).
 * @details Recognises ASCII alphanumeric bytes and the four unreserved marks.
 * @param[in] c Byte to classify.
 * @return Whether percent-encoding may be omitted.
 * @retval true The byte is RFC 3986 unreserved.
 * @retval false The byte requires percent-encoding.
 * @pre @p c is one complete input byte.
 * @pre Classification is intentionally ASCII-only.
 * @post No state is modified.
 * @post Input is unchanged.
 * @note Thread-safe: pure comparison.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_unreserved(unsigned char c)
{
  const bool alpha = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'));
  const bool digit = (c >= '0') && (c <= '9');
  const bool mark  = (c == '-') || (c == '.') || (c == '_') || (c == '~');
  return alpha || digit || mark;
}

/**
 * @brief Upper-case hex digit for a 0..15 nibble.
 * @details Masks to the low nibble before indexing the constant digit table.
 * @param[in] nibble Value whose low four bits are encoded.
 * @return Upper-case hexadecimal digit.
 * @retval other One byte from `0`-`9` or `A`-`F`.
 * @pre Only the low nibble is significant.
 * @pre Upper-case percent escapes are required.
 * @post No state is modified.
 * @post The result is an ASCII hex digit.
 * @note Thread-safe: reads constant storage.
 * @since 0.1.0
 */
RA8_INTERNAL static char internal_hex_digit(unsigned nibble)
{
  static const char digits[] = "0123456789ABCDEF";
  return digits[nibble & (unsigned)k_nibble_mask];
}

bool mdl_query_encode(const char* term, char* out, size_t cap)
{
  if ((term == nullptr) || (out == nullptr) || (cap == 0U)) {
    if ((out != nullptr) && (cap > 0U)) {
      out[0] = '\0';
    }
    return false;
  }
  size_t n = 0U;
  for (size_t i = 0U; term[i] != '\0'; ++i) {
    const unsigned char c = (unsigned char)term[i];
    if (internal_is_unreserved(c)) {
      if ((n + 1U) >= cap) {
        out[0] = '\0';
        return false;
      }
      out[n] = (char)c;
      ++n;
    } else {
      if ((n + (size_t)k_triplet_len) >= cap) {
        out[0] = '\0';
        return false;
      }
      out[n]      = '%';
      out[n + 1U] = internal_hex_digit((unsigned)c >> (unsigned)k_hi_nibble_shift);
      out[n + 2U] = internal_hex_digit((unsigned)c);
      n += (size_t)k_triplet_len;
    }
  }
  out[n] = '\0';
  return true;
}

bool mdl_search_build_url(const char* tmpl, const char* encoded_term, char* out, size_t cap)
{
  if ((tmpl == nullptr) || (encoded_term == nullptr) || (out == nullptr) || (cap == 0U)) {
    if ((out != nullptr) && (cap > 0U)) {
      out[0] = '\0';
    }
    return false;
  }
  const size_t plen = strlen(s_placeholder);
  const size_t tlen = strlen(encoded_term);
  size_t       n    = 0U;
  bool         hit  = false;
  size_t       i    = 0U;
  while (tmpl[i] != '\0') {
    if (strncmp(tmpl + i, s_placeholder, plen) == 0) {
      if ((n + tlen) >= cap) {
        out[0] = '\0';
        return false;
      }
      memcpy(out + n, encoded_term, tlen);
      n += tlen;
      i += plen;
      hit = true;
    } else {
      if ((n + 1U) >= cap) {
        out[0] = '\0';
        return false;
      }
      out[n] = tmpl[i];
      ++n;
      ++i;
    }
  }
  if (!hit) {
    out[0] = '\0';
    return false; /* a template with no {q} could never carry the term */
  }
  out[n] = '\0';
  return true;
}

mdl_search_outcome_t mdl_search_classify(const mdl_hit_list_t* hits)
{
  if (hits == nullptr) {
    return k_mdl_search_markup_changed;
  }
  if (hits->count > 0U) {
    return k_mdl_search_have_results;
  }
  if (hits->anchors_seen == 0U) {
    return k_mdl_search_markup_changed;
  }
  return k_mdl_search_zero_results;
}

size_t mdl_search_filter_series_hits(mdl_hit_list_t* hits, const char* chapter_marker)
{
  if ((hits == nullptr) || (chapter_marker == nullptr)) {
    return 0U;
  }
  const size_t before = hits->count;
  size_t       kept   = 0U;
  for (size_t i = 0U; i < before; ++i) {
    const mdl_hit_t* candidate = &hits->hits[i];
    bool             reject    = false;
    if ((chapter_marker[0] != '\0') && (strstr(candidate->url, chapter_marker) != nullptr)) {
      reject = true;
    }
    for (size_t j = 0U; (!reject) && (j < kept); ++j) {
      if (strcmp(candidate->url, hits->hits[j].url) == 0) {
        reject = true;
      }
    }
    if (!reject) {
      if (kept != i) {
        hits->hits[kept] = *candidate;
      }
      ++kept;
    }
  }
  hits->count = (uint16_t)kept;
  return before - kept;
}
