/**
 * @file mdl_extract.c
 * @brief Bounded image/anchor URL scanning and relative-URL resolution.
 * @details Resolves absolute and relative URL forms, then scans bounded image or
 *          anchor tags into caller-owned URL lists. Titled-anchor extraction
 *          is isolated in `mdl_extract_hits.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_extract.h"

#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Local scan/resolve limits. */
typedef enum : uint16_t {
  k_authority_max = 512, /**< Max bytes of "scheme://host" prefix. */
} mdl_scan_limits_t;

#include "mdl_extract_internal.h"

/** @brief Extract "scheme://host" from `base` into `auth` (NUL-terminated).
 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] base NUL-terminated base URL or path.
 * @param[in,out] auth Bounded authority string.
 * @param[in] cap Destination capacity including any terminator.
 * @return True when a complete authority fits in @p auth; otherwise false.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_authority_of(const char* base, char* auth, size_t cap)
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

/** @brief Resolve a scheme-relative ("//host/...") URL against `base`.
 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] base NUL-terminated base URL or path.
 * @param[in] raw Untrusted source text to validate or resolve.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @return True when the scheme-relative URL resolves completely into @p out.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
internal_resolve_scheme_rel(const char* base, const char* raw, char* out, size_t out_cap)
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

/** @brief Resolve a root-relative ("/path") URL against authority `auth`.
 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] auth Bounded authority string.
 * @param[in] raw Untrusted source text to validate or resolve.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @return True when the root-relative URL resolves completely into @p out.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
internal_resolve_root_rel(const char* auth, const char* raw, char* out, size_t out_cap)
{
  /* The snprintf return is the truncation test: n >= out_cap is exactly the
   * over-long case a separate strlen guard would reject, and using the return
   * (rather than discarding it) is also what keeps -Wformat-truncation quiet.
   */
  const int n = snprintf(out, out_cap, "%s%s", auth, raw);
  return (n >= 0) && ((size_t)n < out_cap);
}

/** @brief Resolve a path-relative URL against `base` (authority `auth`).
 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] base NUL-terminated base URL or path.
 * @param[in] auth Bounded authority string.
 * @param[in] raw Untrusted source text to validate or resolve.
 * @param[out] out Caller-owned result storage.
 * @param[in] out_cap Destination capacity including any terminator.
 * @return True when the path-relative URL resolves completely into @p out.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_resolve_path_rel(const char* base,
                                                   const char* auth,
                                                   const char* raw,
                                                   char*       out,
                                                   size_t      out_cap)
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
    /* As in internal_resolve_root_rel: the snprintf return is the truncation test, and
     * using it keeps -Wformat-truncation quiet where a discarded return does
     * not. */
    const int n = snprintf(out, out_cap, "%s/%s", auth, raw);
    return (n >= 0) && ((size_t)n < out_cap);
  }
  if ((slash + 1U + strlen(raw) + 1U) > out_cap) {
    return false;
  }
  memcpy(out, base, slash + 1U);
  memcpy(out + slash + 1U, raw, strlen(raw) + 1U);
  return true;
}

/** @brief Resolve `raw` (possibly relative) against `base` into `out`. */
bool mdl_extract_resolve_url(const char* base, const char* raw, char* out, size_t out_cap)
{
  if ((base == nullptr) || (raw == nullptr) || (out == nullptr) || (out_cap == 0U)) {
    return false;
  }
  while ((*raw == ' ') || (*raw == '\t') || (*raw == '\n') || (*raw == '\r')) {
    ++raw;
  }
  if ((raw[0] == '\0') || (raw[0] == '#') || (strncmp(raw, "data:", sizeof("data:") - 1U) == 0)) {
    return false;
  }
  if ((strncmp(raw, "http://", sizeof("http://") - 1U) == 0) ||
      (strncmp(raw, "https://", sizeof("https://") - 1U) == 0)) {
    return internal_copy_fits(out, out_cap, raw);
  }

  char auth[k_authority_max];
  if (!internal_authority_of(base, auth, sizeof(auth))) {
    return false;
  }
  if ((raw[0] == '/') && (raw[1] == '/')) {
    return internal_resolve_scheme_rel(base, raw, out, out_cap);
  }
  if (raw[0] == '/') {
    return internal_resolve_root_rel(auth, raw, out, out_cap);
  }
  return internal_resolve_path_rel(base, auth, raw, out, out_cap);
}

/** @brief True if `url` is already present in `list`.
 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] list Existing bounded URL list.
 * @param[in] url NUL-terminated URL input.
 * @return True when @p url already occurs in @p list.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_already_have(const mdl_url_list_t* list, const char* url)
{
  for (size_t i = 0U; i < list->count; ++i) {
    if (strcmp(list->urls[i], url) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Read `attr1` (or `attr2`) from one tag, resolve+filter, append.
 * @retval k_ra8_ok      Appended, filtered out, or attr absent (all non-fatal).
 * @retval k_ra8_err_no_mem  List already at ::k_mdl_max_urls.

 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] base_url NUL-terminated base URL.
 * @param[in] attr1 Preferred attribute name.
 * @param[in] attr2 Fallback attribute name, if any.
 * @param[in] keep Optional substring filter.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[out] out Caller-owned result storage.
 * @return Operation status.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit_tag_url(const char*     base_url,
                                                    const char*     attr1,
                                                    const char*     attr2,
                                                    const char*     keep,
                                                    const char*     tag,
                                                    size_t          tag_len,
                                                    mdl_url_list_t* out)
{
  char raw[k_mdl_url_max];
  bool got = internal_find_attr_value(tag, tag_len, attr1, raw, sizeof(raw));
  if (!got && (attr2 != nullptr)) {
    got = internal_find_attr_value(tag, tag_len, attr2, raw, sizeof(raw));
  }
  if (!got) {
    return k_ra8_ok;
  }
  char abs[k_mdl_url_max];
  if (!mdl_extract_resolve_url(base_url, raw, abs, sizeof(abs)) ||
      !internal_contains_ok(abs, keep) || internal_already_have(out, abs)) {
    return k_ra8_ok;
  }
  if (out->count >= (size_t)k_mdl_max_urls) {
    return k_ra8_err_no_mem;
  }
  (void)internal_copy_fits(out->urls[out->count], k_mdl_url_max, abs);
  out->count++;
  return k_ra8_ok;
}

/**
 * @brief Shared scanner for `<img>`/`<a>`: emit each tag's resolved URL.

 * @details Scans caller-supplied markup and URLs without allocation.
 *          Only fully resolved values that fit bounded result storage are appended.
 * @param[in] html Readable markup bytes.
 * @param[in] html_len Readable markup length.
 * @param[in] base_url NUL-terminated base URL.
 * @param[in] tagname Tag-name prefix to scan for.
 * @param[in] attr1 Preferred attribute name.
 * @param[in] attr2 Fallback attribute name, if any.
 * @param[in] keep Optional substring filter.
 * @param[out] out Caller-owned result storage.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_scan_tags(const char*     html,
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
    const char* tag = internal_find_ci(html + pos, html_len - pos, tagname);
    if (tag == nullptr) {
      break;
    }
    const size_t tag_off = (size_t)(tag - html);
    const size_t after   = tag_off + name_len;
    if ((after < html_len) && !internal_is_name_end(html[after])) {
      pos = tag_off + 1U; /* false match, e.g. "<article" while seeking "<a" */
      continue;
    }
    const char*  gt      = memchr(tag, '>', html_len - tag_off);
    const size_t tag_len = (gt == nullptr) ? (html_len - tag_off) : (size_t)(gt - tag);
    if (internal_emit_tag_url(base_url, attr1, attr2, keep, tag, tag_len, out) ==
        k_ra8_err_no_mem) {
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
  return internal_scan_tags(html, html_len, base_url, "<img", p1, p2, url_contains, out);
}

ra8_err_t mdl_extract_anchors(const char*     html,
                              size_t          html_len,
                              const char*     base_url,
                              const char*     href_contains,
                              mdl_url_list_t* out)
{
  return internal_scan_tags(html, html_len, base_url, "<a", "href", nullptr, href_contains, out);
}
