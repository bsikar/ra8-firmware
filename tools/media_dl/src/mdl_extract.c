/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_extract.c
 * @brief v1 `<img>`/`<a>` tag scanner + relative-URL resolver.
 */
#include "mdl_extract.h"

#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Local scan/resolve limits. */
typedef enum : uint16_t {
  k_authority_max = 512, /**< Max bytes of "scheme://host" prefix. */
} mdl_scan_limits_t;

/** @brief ASCII lower-case (locale-independent). */
RA8_INTERNAL static char lc(char c)
{
  /* The conditional operator promotes both arms to int, so the cast belongs
   * on the whole expression; both arms are already char-valued. */
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/** @brief True if `c` ends an HTML attribute token on its left side. */
RA8_INTERNAL static bool is_attr_boundary(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '"') || (c == '\'') ||
         (c == '<');
}

/** @brief True if `c` terminates a tag name (so "<a" != "<article"). */
RA8_INTERNAL static bool is_name_end(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '>') || (c == '/');
}

/** @brief Case-insensitive search for `needle` in [hay, hay+hay_len). */
RA8_INTERNAL static const char* find_ci(const char* hay, size_t hay_len, const char* needle)
{
  const size_t nlen = strlen(needle);
  if ((nlen == 0U) || (hay_len < nlen)) {
    return nullptr;
  }
  for (size_t i = 0U; i + nlen <= hay_len; ++i) {
    size_t j = 0U;
    while ((j < nlen) && (lc(hay[i + j]) == lc(needle[j]))) {
      ++j;
    }
    if (j == nlen) {
      return hay + i;
    }
  }
  return nullptr;
}

/** @brief Advance past spaces/tabs in [k, tag_len). */
RA8_INTERNAL static size_t skip_ws(const char* tag, size_t tag_len, size_t k)
{
  while ((k < tag_len) && ((tag[k] == ' ') || (tag[k] == '\t'))) {
    ++k;
  }
  return k;
}

/**
 * @brief From position `k` just after an attr name, read `="value"`.
 * @return true and fills `out` (NUL-terminated) if a quoted value was parsed.
 */
RA8_INTERNAL static bool
read_eq_value(const char* tag, size_t tag_len, size_t k, char* out, size_t out_cap)
{
  k = skip_ws(tag, tag_len, k);
  if (!((k < tag_len) && (tag[k] == '='))) {
    return false;
  }
  k = skip_ws(tag, tag_len, k + 1U);
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
 */
RA8_INTERNAL static bool
find_attr_value(const char* tag, size_t tag_len, const char* attr, char* out, size_t out_cap)
{
  const size_t alen = strlen(attr);
  size_t       pos  = 0U;
  while (pos + alen <= tag_len) {
    const char* hit = find_ci(tag + pos, tag_len - pos, attr);
    if (hit == nullptr) {
      return false;
    }
    const size_t idx     = (size_t)(hit - tag);
    const bool   left_ok = (idx == 0U) || is_attr_boundary(tag[idx - 1U]);
    if (left_ok && read_eq_value(tag, tag_len, idx + alen, out, out_cap)) {
      return true;
    }
    pos = idx + alen;
  }
  return false;
}

/** @brief Copy `src` into `out` if it fits (incl. NUL). */
RA8_INTERNAL static bool copy_fits(char* out, size_t out_cap, const char* src)
{
  const size_t n = strlen(src);
  if (n + 1U > out_cap) {
    return false;
  }
  memcpy(out, src, n + 1U);
  return true;
}

/** @brief Extract "scheme://host" from `base` into `auth` (NUL-terminated). */
RA8_INTERNAL static bool authority_of(const char* base, char* auth, size_t cap)
{
  const char* sep = strstr(base, "://");
  if (sep == nullptr) {
    return false;
  }
  const char*  host = sep + (sizeof("://") - 1U);
  const char*  end  = strchr(host, '/');
  const size_t len  = (end == nullptr) ? strlen(base) : (size_t)(end - base);
  if (len + 1U > cap) {
    return false;
  }
  memcpy(auth, base, len);
  auth[len] = '\0';
  return true;
}

/** @brief Resolve a scheme-relative ("//host/...") URL against `base`. */
RA8_INTERNAL static bool
resolve_scheme_rel(const char* base, const char* raw, char* out, size_t out_cap)
{
  const char*  colon = strstr(base, "://");
  const size_t slen  = (size_t)(colon - base);
  if ((slen + 1U + strlen(raw) + 1U) > out_cap) {
    return false;
  }
  memcpy(out, base, slen);
  out[slen] = ':';
  memcpy(out + slen + 1U, raw, strlen(raw) + 1U);
  return true;
}

/** @brief Resolve a root-relative ("/path") URL against authority `auth`. */
RA8_INTERNAL static bool
resolve_root_rel(const char* auth, const char* raw, char* out, size_t out_cap)
{
  if ((strlen(auth) + strlen(raw) + 1U) > out_cap) {
    return false;
  }
  (void)snprintf(out, out_cap, "%s%s", auth, raw);
  return true;
}

/** @brief Resolve a path-relative URL against `base` (authority `auth`). */
RA8_INTERNAL static bool
resolve_path_rel(const char* base, const char* auth, const char* raw, char* out, size_t out_cap)
{
  const char*  q     = strpbrk(base, "?#");
  const size_t blen  = (q == nullptr) ? strlen(base) : (size_t)(q - base);
  size_t       slash = 0U;
  for (size_t i = 0U; i < blen; ++i) {
    if (base[i] == '/') {
      slash = i;
    }
  }
  if (slash < strlen(auth)) {
    if ((strlen(auth) + 1U + strlen(raw) + 1U) > out_cap) {
      return false;
    }
    (void)snprintf(out, out_cap, "%s/%s", auth, raw);
    return true;
  }
  if ((slash + 1U + strlen(raw) + 1U) > out_cap) {
    return false;
  }
  memcpy(out, base, slash + 1U);
  memcpy(out + slash + 1U, raw, strlen(raw) + 1U);
  return true;
}

/** @brief Resolve `raw` (possibly relative) against `base` into `out`. */
RA8_INTERNAL static bool resolve_url(const char* base, const char* raw, char* out, size_t out_cap)
{
  while ((*raw == ' ') || (*raw == '\t') || (*raw == '\n') || (*raw == '\r')) {
    ++raw;
  }
  if ((raw[0] == '\0') || (raw[0] == '#') || (strncmp(raw, "data:", sizeof("data:") - 1U) == 0)) {
    return false;
  }
  if ((strncmp(raw, "http://", sizeof("http://") - 1U) == 0) ||
      (strncmp(raw, "https://", sizeof("https://") - 1U) == 0)) {
    return copy_fits(out, out_cap, raw);
  }

  char auth[k_authority_max];
  if (!authority_of(base, auth, sizeof(auth))) {
    return false;
  }
  if ((raw[0] == '/') && (raw[1] == '/')) {
    return resolve_scheme_rel(base, raw, out, out_cap);
  }
  if (raw[0] == '/') {
    return resolve_root_rel(auth, raw, out, out_cap);
  }
  return resolve_path_rel(base, auth, raw, out, out_cap);
}

/** @brief True if `url` is already present in `list`. */
RA8_INTERNAL static bool already_have(const mdl_url_list_t* list, const char* url)
{
  for (size_t i = 0U; i < list->count; ++i) {
    if (strcmp(list->urls[i], url) == 0) {
      return true;
    }
  }
  return false;
}

/** @brief True if `needle` is empty/NULL or a substring of `url`. */
RA8_INTERNAL static bool contains_ok(const char* url, const char* needle)
{
  if ((needle == nullptr) || (needle[0] == '\0')) {
    return true;
  }
  return strstr(url, needle) != nullptr;
}

/**
 * @brief Read `attr1` (or `attr2`) from one tag, resolve+filter, append.
 * @retval k_ra8_ok      Appended, filtered out, or attr absent (all non-fatal).
 * @retval k_ra8_err_no_mem  List already at ::k_mdl_max_urls.
 */
RA8_INTERNAL static ra8_err_t emit_tag_url(const char*     base_url,
                                           const char*     attr1,
                                           const char*     attr2,
                                           const char*     keep,
                                           const char*     tag,
                                           size_t          tag_len,
                                           mdl_url_list_t* out)
{
  char raw[k_mdl_url_max];
  bool got = find_attr_value(tag, tag_len, attr1, raw, sizeof(raw));
  if (!got && (attr2 != nullptr)) {
    got = find_attr_value(tag, tag_len, attr2, raw, sizeof(raw));
  }
  if (!got) {
    return k_ra8_ok;
  }
  char abs[k_mdl_url_max];
  if (!resolve_url(base_url, raw, abs, sizeof(abs)) || !contains_ok(abs, keep) ||
      already_have(out, abs)) {
    return k_ra8_ok;
  }
  if (out->count >= (size_t)k_mdl_max_urls) {
    return k_ra8_err_no_mem;
  }
  (void)copy_fits(out->urls[out->count], k_mdl_url_max, abs);
  out->count++;
  return k_ra8_ok;
}

/**
 * @brief Shared scanner for `<img>`/`<a>`: emit each tag's resolved URL.
 */
RA8_INTERNAL static ra8_err_t scan_tags(const char*     html,
                                        size_t          html_len,
                                        const char*     base_url,
                                        const char*     tagname,
                                        const char*     attr1,
                                        const char*     attr2,
                                        const char*     keep,
                                        mdl_url_list_t* out)
{
  if ((html == nullptr) || (base_url == nullptr) || (attr1 == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  out->count            = 0U;
  const size_t name_len = strlen(tagname);

  size_t pos = 0U;
  while (pos < html_len) {
    const char* tag = find_ci(html + pos, html_len - pos, tagname);
    if (tag == nullptr) {
      break;
    }
    const size_t tag_off = (size_t)(tag - html);
    const size_t after   = tag_off + name_len;
    if ((after < html_len) && !is_name_end(html[after])) {
      pos = tag_off + 1U; /* false match, e.g. "<article" while seeking "<a" */
      continue;
    }
    const char*  gt      = memchr(tag, '>', html_len - tag_off);
    const size_t tag_len = (gt == nullptr) ? (html_len - tag_off) : (size_t)(gt - tag);
    if (emit_tag_url(base_url, attr1, attr2, keep, tag, tag_len, out) == k_ra8_err_no_mem) {
      return k_ra8_err_no_mem;
    }
    pos = tag_off + ((gt == nullptr) ? tag_len : (tag_len + 1U));
  }
  return k_ra8_ok;
}

ra8_err_t mdl_extract_images(const char*     html,
                             size_t          html_len,
                             const char*     base_url,
                             const char*     prefer_attr,
                             const char*     url_contains,
                             mdl_url_list_t* out)
{
  const char* p1 = (prefer_attr == nullptr) ? "data-src" : prefer_attr;
  const char* p2 = (strcmp(p1, "data-src") == 0) ? "src" : "data-src";
  return scan_tags(html, html_len, base_url, "<img", p1, p2, url_contains, out);
}

ra8_err_t mdl_extract_anchors(const char*     html,
                              size_t          html_len,
                              const char*     base_url,
                              const char*     href_contains,
                              mdl_url_list_t* out)
{
  return scan_tags(html, html_len, base_url, "<a", "href", nullptr, href_contains, out);
}
