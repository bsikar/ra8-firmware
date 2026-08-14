/**
 * @file mdl_search.c
 * @brief Pure search/discovery policy: percent-encoding, URL templating, and
 *        the zero-vs-broken result classifier.
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

/** @brief True for an RFC 3986 unreserved byte (copied verbatim). */
RA8_INTERNAL static bool is_unreserved(unsigned char c)
{
  const bool alpha = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'));
  const bool digit = (c >= '0') && (c <= '9');
  const bool mark  = (c == '-') || (c == '.') || (c == '_') || (c == '~');
  return alpha || digit || mark;
}

/** @brief Upper-case hex digit for a 0..15 nibble. */
RA8_INTERNAL static char hex_digit(unsigned nibble)
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
    if (is_unreserved(c)) {
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
      out[n + 1U] = hex_digit((unsigned)c >> (unsigned)k_hi_nibble_shift);
      out[n + 2U] = hex_digit((unsigned)c);
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
