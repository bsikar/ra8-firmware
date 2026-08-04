/**
 * @file mdl_urlname.c
 * @brief Pure URL-to-name helpers (last segment, chapter number, extension).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_urlname.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_sanitize.h"
#include "ra8_attributes.h"

/** @brief On-stack buffers and parse radix for the URL-name helpers. */
typedef enum : uint16_t {
  k_urlname_raw_bytes = 256, /**< Pre-sanitise last-segment scratch bytes. */
  k_urlname_num_bytes = 16,  /**< Digit-run capture buffer bytes.          */
  k_urlname_dec_base  = 10,  /**< strtol() radix for the chapter number.   */
  k_urlname_case_gap  = 32,  /**< 'a' - 'A': ASCII upper-to-lower offset.  */
} mdl_urlname_const_t;

/** @brief End offset of a URL's path, past any `?query`/`#fragment`. */
RA8_INTERNAL static size_t path_end(const char* url)
{
  const char* q = strpbrk(url, "?#");
  return (q == nullptr) ? strlen(url) : (size_t)(q - url);
}

void mdl_urlname_last_segment(const char* url, char* out, size_t cap)
{
  if ((url == nullptr) || (out == nullptr) || (cap < 2U)) {
    if ((out != nullptr) && (cap > 0U)) {
      out[0] = '\0';
    }
    return;
  }
  size_t end = path_end(url);
  while ((end > 0U) && (url[end - 1U] == '/')) {
    --end; /* trim trailing slashes */
  }
  size_t start = end;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start;
  }
  char   raw[k_urlname_raw_bytes];
  size_t n = 0U;
  for (size_t i = start; (i < end) && (n + 1U < sizeof(raw)); ++i) {
    raw[n] = url[i];
    ++n;
  }
  raw[n] = '\0';
  (void)mdl_sanitize_segment(raw, out, cap);
}

long mdl_urlname_chapter_number(const char* url)
{
  if (url == nullptr) {
    return 0L;
  }
  const size_t len   = strlen(url);
  size_t       s     = 0U;
  size_t       e     = 0U;
  bool         found = false;
  for (size_t i = 0U; i < len; ++i) {
    if ((url[i] >= '0') && (url[i] <= '9')) {
      size_t j = i;
      while ((j < len) && (url[j] >= '0') && (url[j] <= '9')) {
        ++j;
      }
      s     = i;
      e     = j;
      found = true;
      i     = j; /* skip the run just captured */
    }
  }
  if (!found) {
    return 0L;
  }
  char   num[k_urlname_num_bytes];
  size_t n = 0U;
  for (size_t i = s; (i < e) && (n + 1U < sizeof(num)); ++i) {
    num[n++] = url[i];
  }
  num[n] = '\0';
  return strtol(num, nullptr, (int)k_urlname_dec_base);
}

/** @brief Lower-case an ASCII byte. */
RA8_INTERNAL static char to_lower_ascii(char c)
{
  const int lowered = ((c >= 'A') && (c <= 'Z')) ? (c + (int)k_urlname_case_gap) : (int)c;
  return (char)lowered;
}

/** @brief True when `ext` is one of the accepted raster image extensions. */
RA8_INTERNAL static bool is_known_ext(const char* ext)
{
  static const char* const k_known[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
  for (size_t i = 0U; i < (sizeof(k_known) / sizeof(k_known[0])); ++i) {
    if (strcmp(ext, k_known[i]) == 0) {
      return true;
    }
  }
  return false;
}

void mdl_urlname_ext(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return;
  }
  (void)snprintf(out, cap, "%s", "jpg");
  if (url == nullptr) {
    return;
  }
  const size_t pend  = path_end(url);
  size_t       start = pend;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start; /* isolate the last path segment */
  }
  size_t dot = pend;
  for (size_t i = start; i < pend; ++i) {
    if (url[i] == '.') {
      dot = i;
    }
  }
  if (dot >= pend) {
    return; /* no '.' in the segment -> keep the default */
  }
  char   ext[8];
  size_t n = 0U;
  for (size_t i = dot + 1U; (i < pend) && (n + 1U < sizeof(ext)); ++i) {
    ext[n++] = to_lower_ascii(url[i]);
  }
  ext[n] = '\0';
  if (is_known_ext(ext)) {
    (void)snprintf(out, cap, "%s", ext);
  }
}
