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

/** @brief Start offset of the path, excluding any URL scheme and authority. */
RA8_INTERNAL static size_t path_start(const char* url, size_t end)
{
  const char* scheme = strstr(url, "://");
  if ((scheme == nullptr) || ((size_t)(scheme - url) >= end)) {
    return 0U; /* Relative URL: its entire prefix is a path. */
  }
  const size_t authority = (size_t)(scheme - url) + strlen("://");
  const char*  slash     = memchr(url + authority, '/', end - authority);
  return (slash == nullptr) ? end : (size_t)(slash - url);
}

/** @brief Locate the last chapter marker wholly inside `[begin,end)`. */
RA8_INTERNAL static const char*
last_path_marker(const char* url, size_t begin, size_t end, const char* marker)
{
  const size_t marker_len = strlen(marker);
  const char*  found      = nullptr;
  if (marker_len > (end - begin)) {
    return nullptr;
  }
  for (size_t i = begin; i <= (end - marker_len); ++i) {
    if (memcmp(url + i, marker, marker_len) == 0) {
      found = url + i;
    }
  }
  return found;
}

double mdl_urlname_chapter_value(const char* url)
{
  if (url == nullptr) {
    return 0.0;
  }
  const size_t end          = path_end(url);
  const size_t begin        = path_start(url, end);
  const char*  start        = last_path_marker(url, begin, end, "chapter-");
  size_t       skip         = strlen("chapter-");
  const char*  short_marker = last_path_marker(url, begin, end, "/ch-");
  if ((short_marker != nullptr) && ((start == nullptr) || (short_marker > start))) {
    start = short_marker;
    skip  = strlen("/ch-");
  }
  if (start != nullptr) {
    start += skip;
    char   num[k_urlname_num_bytes];
    size_t n = 0U;
    while ((*start >= '0') && (*start <= '9') && (n + 1U < sizeof(num))) {
      num[n++] = *start++;
    }
    if (((*start == '-') || (*start == '.')) && (start[1] >= '0') && (start[1] <= '9') &&
        (n + 2U < sizeof(num))) {
      num[n++] = '.';
      ++start;
      while ((*start >= '0') && (*start <= '9') && (n + 1U < sizeof(num))) {
        num[n++] = *start++;
      }
    }
    num[n] = '\0';
    return (n == 0U) ? 0.0 : strtod(num, nullptr);
  }

  /* Compatibility fallback for descriptors whose chapter slugs lack a marker. */
  size_t s = 0U;
  size_t e = 0U;
  for (size_t i = begin; i < end; ++i) {
    if ((url[i] >= '0') && (url[i] <= '9')) {
      size_t j = i;
      while ((j < end) && (url[j] >= '0') && (url[j] <= '9')) {
        ++j;
      }
      s = i;
      e = j;
      i = j;
    }
  }
  if (e == 0U) {
    return 0.0;
  }
  char   num[k_urlname_num_bytes];
  size_t n = 0U;
  for (size_t i = s; (i < e) && (n + 1U < sizeof(num)); ++i) {
    num[n++] = url[i];
  }
  num[n] = '\0';
  return strtod(num, nullptr);
}

long mdl_urlname_chapter_number(const char* url)
{
  return (long)mdl_urlname_chapter_value(url);
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

bool mdl_urlname_sniff_image_type(const void* buf,
                                  size_t      buf_len,
                                  const char* content_type,
                                  char*       out_ext,
                                  size_t      ext_cap,
                                  char*       out_mime,
                                  size_t      mime_cap)
{
  const char* ext   = nullptr;
  const char* mime  = nullptr;
  bool        found = false;

  /* 1. Inspect Magic Bytes of fetched buffer */
  if ((buf != nullptr) && (buf_len >= 3U)) {
    const uint8_t* b = (const uint8_t*)buf;
    /* JPEG: FF D8 FF */
    if ((b[0] == 0xFFU) && (b[1] == 0xD8U) && (b[2] == 0xFFU)) {
      ext   = "jpg";
      mime  = "image/jpeg";
      found = true;
    }
  }

  if (!found && (buf != nullptr) && (buf_len >= 4U)) {
    const uint8_t* b = (const uint8_t*)buf;
    /* PNG: 89 50 4E 47 */
    if ((b[0] == 0x89U) && (b[1] == 0x50U) && (b[2] == 0x4EU) && (b[3] == 0x47U)) {
      ext   = "png";
      mime  = "image/png";
      found = true;
    }
  }

  if (!found && (buf != nullptr) && (buf_len >= 12U)) {
    const uint8_t* b = (const uint8_t*)buf;
    /* WebP: RIFF....WEBP */
    if ((b[0] == 'R') && (b[1] == 'I') && (b[2] == 'F') && (b[3] == 'F') && (b[8] == 'W') &&
        (b[9] == 'E') && (b[10] == 'B') && (b[11] == 'P')) {
      ext   = "webp";
      mime  = "image/webp";
      found = true;
    }
  }

  if (!found && (buf != nullptr) && (buf_len >= 6U)) {
    const uint8_t* b = (const uint8_t*)buf;
    /* GIF: GIF87a / GIF89a */
    if ((b[0] == 'G') && (b[1] == 'I') && (b[2] == 'F') && (b[3] == '8') &&
        ((b[4] == '7') || (b[4] == '9')) && (b[5] == 'a')) {
      ext   = "gif";
      mime  = "image/gif";
      found = true;
    }
  }

  /* 2. Fallback to HTTP Content-Type header */
  if (!found && (content_type != nullptr) && (content_type[0] != '\0')) {
    char   ct_lower[128];
    size_t i = 0U;
    for (; (content_type[i] != '\0') && (i + 1U < sizeof(ct_lower)); ++i) {
      ct_lower[i] = to_lower_ascii(content_type[i]);
    }
    ct_lower[i] = '\0';

    if ((strstr(ct_lower, "image/jpeg") != nullptr) || (strstr(ct_lower, "image/jpg") != nullptr)) {
      ext   = "jpg";
      mime  = "image/jpeg";
      found = true;
    } else if (strstr(ct_lower, "image/png") != nullptr) {
      ext   = "png";
      mime  = "image/png";
      found = true;
    } else if (strstr(ct_lower, "image/webp") != nullptr) {
      ext   = "webp";
      mime  = "image/webp";
      found = true;
    } else if (strstr(ct_lower, "image/gif") != nullptr) {
      ext   = "gif";
      mime  = "image/gif";
      found = true;
    }
  }

  if (found) {
    if ((out_ext != nullptr) && (ext_cap > 0U)) {
      (void)snprintf(out_ext, ext_cap, "%s", ext);
    }
    if ((out_mime != nullptr) && (mime_cap > 0U)) {
      (void)snprintf(out_mime, mime_cap, "%s", mime);
    }
    return true;
  }

  return false;
}

bool mdl_urlname_sniff_file(const char* file_path,
                            const char* content_type,
                            char*       out_ext,
                            size_t      ext_cap,
                            char*       out_mime,
                            size_t      mime_cap)
{
  if (file_path == nullptr) {
    return mdl_urlname_sniff_image_type(nullptr,
                                        0U,
                                        content_type,
                                        out_ext,
                                        ext_cap,
                                        out_mime,
                                        mime_cap);
  }
  FILE* f = fopen(file_path, "rb");
  if (f == nullptr) {
    return mdl_urlname_sniff_image_type(nullptr,
                                        0U,
                                        content_type,
                                        out_ext,
                                        ext_cap,
                                        out_mime,
                                        mime_cap);
  }
  uint8_t      header[16];
  const size_t nread = fread(header, 1U, sizeof(header), f);
  (void)fclose(f);
  return mdl_urlname_sniff_image_type(header,
                                      nread,
                                      content_type,
                                      out_ext,
                                      ext_cap,
                                      out_mime,
                                      mime_cap);
}
