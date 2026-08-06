/**
 * @file mdl_sanitize.c
 * @brief Implementation of the untrusted-name sanitisers.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#include "mdl_sanitize.h"

#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Small fixed sizes used while sanitising a name. */
typedef enum : uint8_t {
  k_reserved_base_max = 8, /**< Buffer for a Windows reserved-name base. */
  k_reserved_len      = 4, /**< Length of a COMx / LPTx reserved name.   */
  k_reserved_digit_at = 3, /**< Index of the digit in COMx / LPTx.       */
} mdl_sanitize_size_t;

/** @brief Fallback name substituted when a segment sanitises to nothing. */
static const char* const s_fallback_name = "item";

/** @brief Exactly-reserved Windows device base names (case-folded). */
static const char* const s_reserved_exact[] = {"con", "prn", "aux", "nul"};
/** @brief Reserved Windows device prefixes taking a 1-9 suffix. */
static const char* const s_reserved_numbered[] = {"com", "lpt"};

/** @brief ASCII lower-case of one character (locale-independent). */
RA8_INTERNAL static char lower_ascii(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/** @brief True if `c` may appear verbatim in a sanitised segment. */
RA8_INTERNAL static bool is_allowed_char(char c)
{
  return ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
         (c == '.') || (c == '-') || (c == '_');
}

/** @brief True if `name` is empty, ".", or ".." (no useful segment). */
RA8_INTERNAL static bool is_dot_segment(const char* name)
{
  return (name[0] == '\0') || (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0);
}

/** @brief Case-folded base name (up to the first ".") of `name` into `base`. */
RA8_INTERNAL static void base_of(const char* name, char* base, size_t cap)
{
  size_t i = 0U;
  while ((name[i] != '\0') && (name[i] != '.') && ((i + 1U) < cap)) {
    base[i] = lower_ascii(name[i]);
    ++i;
  }
  base[i] = '\0';
}

/** @brief True if `name`'s base is a Windows reserved device name. */
RA8_INTERNAL static bool is_reserved_base(const char* name)
{
  char base[k_reserved_base_max];
  base_of(name, base, sizeof(base));
  for (size_t i = 0U; i < (sizeof(s_reserved_exact) / sizeof(s_reserved_exact[0])); ++i) {
    if (strcmp(base, s_reserved_exact[i]) == 0) {
      return true;
    }
  }
  if ((strlen(base) == (size_t)k_reserved_len) && (base[k_reserved_digit_at] >= '1') &&
      (base[k_reserved_digit_at] <= '9')) {
    for (size_t i = 0U; i < (sizeof(s_reserved_numbered) / sizeof(s_reserved_numbered[0])); ++i) {
      if (strncmp(base, s_reserved_numbered[i], strlen(s_reserved_numbered[i])) == 0) {
        return true;
      }
    }
  }
  return false;
}

/** @brief Copy `raw` into `out` replacing unsafe bytes; report bad/truncated. */
RA8_INTERNAL static bool copy_sanitised(const char* raw, char* out, size_t cap, size_t* out_len)
{
  bool   clean = true;
  size_t n     = 0U;
  if (raw != nullptr) {
    size_t i = 0U;
    while ((raw[i] != '\0') && ((n + 1U) < cap)) {
      if (is_allowed_char(raw[i])) {
        out[n] = raw[i];
      } else {
        out[n] = '_';
        clean  = false;
      }
      ++n;
      ++i;
    }
    if (raw[i] != '\0') {
      clean = false; /* input did not fit: truncated */
    }
  }
  out[n]   = '\0';
  *out_len = n;
  return clean;
}

/** @brief Prepend `_` to `out` in place, staying within `cap`. */
RA8_INTERNAL static void prepend_underscore(char* out, size_t cap, size_t len)
{
  size_t keep = len;
  if ((keep + 2U) > cap) {
    keep = cap - 2U; /* drop the tail that no longer fits after the prefix */
  }
  memmove(out + 1, out, keep);
  out[0]        = '_';
  out[keep + 1] = '\0';
}

bool mdl_sanitize_segment(const char* raw, char* out, size_t cap)
{
  if ((out == nullptr) || (cap < 2U)) {
    return false;
  }
  size_t len   = 0U;
  bool   clean = copy_sanitised(raw, out, cap, &len);
  if (is_dot_segment(out)) {
    (void)snprintf(out, cap, "%s", s_fallback_name);
    return false;
  }
  if (is_reserved_base(out)) {
    prepend_underscore(out, cap, len);
    return false;
  }
  return clean;
}

bool mdl_path_contained(const char* parent, const char* candidate)
{
  if ((parent == nullptr) || (candidate == nullptr)) {
    return false;
  }
  size_t plen = strlen(parent);
  while ((plen > 0U) && (parent[plen - 1U] == '/')) {
    --plen;
  }
  if (plen == 0U) {
    return false;
  }
  if (strncmp(candidate, parent, plen) != 0) {
    return false;
  }
  const char sep = candidate[plen];
  return (sep == '/') || (sep == '\0');
}

/** @brief True if `seg` embeds a path separator (would span directories). */
RA8_INTERNAL static bool has_separator(const char* seg)
{
  return strchr(seg, '/') != nullptr;
}

bool mdl_path_join(const char* parent, const char* seg, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return false;
  }
  out[0] = '\0';
  if ((parent == nullptr) || (seg == nullptr)) {
    return false;
  }
  if (is_dot_segment(seg) || has_separator(seg)) {
    return false; /* empty, `.`, `..`, or a `/`-bearing/absolute segment */
  }
  const size_t plen = strlen(parent);
  const size_t slen = strlen(seg);
  const size_t need = plen + 1U + slen + 1U; /* parent + '/' + seg + NUL */
  if (need > cap) {
    return false; /* refuse a truncated (thus different) path */
  }
  memcpy(out, parent, plen);
  out[plen] = '/';
  memcpy(out + plen + 1U, seg, slen);
  out[plen + 1U + slen] = '\0';
  return true;
}

/** @brief XML entity for a metacharacter, or NULL when `c` needs no escape. */
RA8_INTERNAL static const char* xml_entity(char c)
{
  switch (c) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '"':
      return "&quot;";
    case '\'':
      return "&apos;";
    default:
      return nullptr;
  }
}

bool mdl_xml_escape(const char* src, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return false;
  }
  out[0] = '\0';
  if (src == nullptr) {
    return false;
  }
  size_t n = 0U;
  for (size_t i = 0U; src[i] != '\0'; ++i) {
    const char*  ent  = xml_entity(src[i]);
    const size_t need = (ent != nullptr) ? strlen(ent) : 1U;
    if ((n + need + 1U) > cap) {
      out[0] = '\0';
      return false;
    }
    if (ent != nullptr) {
      memcpy(out + n, ent, need);
    } else {
      out[n] = src[i];
    }
    n += need;
  }
  out[n] = '\0';
  return true;
}
