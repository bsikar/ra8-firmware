/**
 * @file mdl_extract_hits.c
 * @brief Bounded titled-anchor extraction and deduplication.
 * @details Scans anchor tags, resolves URLs through the shared public resolver,
 *          cleans bounded display titles, and publishes only complete unique
 *          hits into caller-owned storage without allocation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include <string.h>

#include "mdl_extract.h"
#include "mdl_extract_internal.h"

/** @brief One recognised HTML entity mapped to its single replacement byte. */
typedef struct {
  const char* name; /**< Entity body after '&', including the ';'. */
  char        ch;   /**< Decoded byte ('\0' means "emit a space"). */
} mdl_entity_t;

/** @brief ASCII whitespace test (locale-independent).
 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] c Input ASCII byte.
 * @return True when @p c is supported ASCII whitespace.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_is_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f') || (c == '\v');
}

/**
 * @brief Decode an entity at `&`, returning its byte and advancing `*i`.
 * @return true when a known entity was consumed; `*space` is set when the
 *         entity is whitespace-like (`&nbsp;`) so the caller collapses it.

 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] s Readable text or byte sequence.
 * @param[in] len Readable byte length.
 * @param[in,out] i Current bounded scan offset.
 * @param[out] out_ch Receives a decoded character.
 * @param[out] space Receives the whitespace classification.
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
internal_decode_entity(const char* s, size_t len, size_t* i, char* out_ch, bool* space)
{
  static const mdl_entity_t table[] = {
    {"amp;", '&'},
    {"lt;", '<'},
    {"gt;", '>'},
    {"quot;", '"'},
    {"#39;", '\''},
    {"apos;", '\''},
    {"nbsp;", '\0'},
  };
  const size_t start = *i + 1U; /* byte after '&' */
  for (size_t e = 0U; e < (sizeof(table) / sizeof(table[0])); ++e) {
    const size_t nlen = strlen(table[e].name);
    if (((start + nlen) <= len) && (memcmp(s + start, table[e].name, nlen) == 0)) {
      *i      = start + nlen - 1U; /* leave *i on the ';'; the loop's ++ steps past */
      *space  = (table[e].ch == '\0');
      *out_ch = table[e].ch;
      return true;
    }
  }
  return false;
}

/**
 * @brief Append one character to bounded output, tracking overflow.
 * @details Writes @p ch at `out[*n]` when capacity remains (reserving room
 *          for the trailing NUL), advancing `*n`; otherwise leaves @p out
 *          unchanged and marks @p fits false.
 * @param[out] out Destination buffer.
 * @param[in,out] n Current write offset, advanced on success.
 * @param[in] cap Destination capacity including the trailing NUL.
 * @param[in,out] fits Cleared when a character could not be appended.
 * @param[in] ch Character to append.
 * @return Nothing.
 * @pre @p out addresses @p cap writable bytes.
 * @pre @p n and @p fits are non-NULL and already initialized.
 * @post `*n` always leaves room for a trailing NUL at `out[*n]`.
 * @post @p fits is only ever cleared, never set back to true.
 * @note Pure bookkeeping; never itself writes the trailing NUL.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_append_out_char(char* out, size_t* n, size_t cap, bool* fits, char ch)
{
  if ((*n + 1U) < cap) {
    out[*n] = ch;
    ++(*n);
  } else {
    *fits = false;
  }
}

/**
 * @brief Copy the anchor inner text from `text` into `out`, cleaned for
 * display.
 * @details Strips nested `<...>` tags, collapses runs of whitespace to one
 *          space, decodes the common HTML entities, and trims the ends.
 * @param[in]  text Source HTML fragment; need not be NUL-terminated.
 * @param[in]  len  Number of readable bytes at @p text.
 * @param[out] out  Destination for cleaned display text.
 * @param[in]  cap  Destination capacity including NUL.
 * @return Whether the complete cleaned text fit.
 * @retval true  @p out contains the complete cleaned text.
 * @retval false @p out contains a NUL-terminated truncated prefix.
 * @pre @p text and @p out are non-NULL.
 * @pre @p cap is greater than zero and describes writable @p out storage.
 * @post @p out is always NUL-terminated.
 * @post No source bytes are modified.
 * @note Thread-safe: uses only caller-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_clean_inner_text(const char* text, size_t len, char* out, size_t cap)
{
  size_t n       = 0U;
  bool   pending = false; /* a space is owed before the next visible char */
  bool   fits    = true;
  for (size_t i = 0U; i < len; ++i) {
    const char c = text[i];
    if (c == '<') {
      while ((i < len) && (text[i] != '>')) {
        ++i; /* skip a nested tag such as <span> */
      }
      continue;
    }
    char emit  = c;
    bool space = internal_is_ws(c);
    if (c == '&') {
      char decoded = '\0';
      if (internal_decode_entity(text, len, &i, &decoded, &space)) {
        emit = decoded;
      }
    }
    if (space) {
      pending = (n > 0U); /* collapse; never a leading space */
      continue;
    }
    if (pending) {
      internal_append_out_char(out, &n, cap, &fits, ' ');
    }
    pending = false;
    internal_append_out_char(out, &n, cap, &fits, emit);
  }
  out[n] = '\0';
  return fits;
}

/** @brief Fill `out` with the URL's last non-empty path segment (slug
 * fallback).
 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] url NUL-terminated URL input.
 * @param[out] out Caller-owned result storage.
 * @param[in] cap Destination capacity including any terminator.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_url_slug(const char* url, char* out, size_t cap)
{
  const char*  q   = strpbrk(url, "?#");
  const size_t end = (q == nullptr) ? strlen(url) : (size_t)(q - url);
  size_t       beg = 0U;
  size_t       fin = end;
  while ((fin > 0U) && (url[fin - 1U] == '/')) {
    --fin; /* drop trailing slashes */
  }
  beg = fin;
  while ((beg > 0U) && (url[beg - 1U] != '/')) {
    --beg; /* walk back to the segment start */
  }
  size_t n = 0U;
  for (size_t i = beg; (i < fin) && ((n + 1U) < cap); ++i) {
    out[n] = url[i];
    ++n;
  }
  out[n] = '\0';
}

/**
 * @brief Read the title for the anchor whose `>` sits at `gt_off`.
 * @return true when a real title (attribute or inner text) was found; false
 *         when the caller must fall back to the URL slug.

 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] html Readable markup bytes.
 * @param[in] html_len Readable markup length.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[in] gt_off Offset of the tag-closing greater-than byte.
 * @param[out] out Caller-owned result storage.
 * @param[in] cap Destination capacity including any terminator.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_anchor_title(const char* html,
                                               size_t      html_len,
                                               const char* tag,
                                               size_t      tag_len,
                                               size_t      gt_off,
                                               char*       out,
                                               size_t      cap)
{
  if (internal_find_attr_value(tag, tag_len, "title", out, cap) && (out[0] != '\0')) {
    return true;
  }
  if (gt_off >= html_len) {
    return false;
  }
  const size_t text_off = gt_off + 1U;
  const char*  close    = internal_find_ci(html + text_off, html_len - text_off, "</a");
  const size_t text_len =
    (close == nullptr) ? (html_len - text_off) : (size_t)(close - (html + text_off));
  return internal_clean_inner_text(html + text_off, text_len, out, cap) && (out[0] != '\0');
}

/** @brief Index of an existing hit with URL `url`, or `count` when absent.
 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] out Existing bounded hit list.
 * @param[in] url NUL-terminated URL input.
 * @return Index of @p url, or `out->count` when it is absent.
 * @retval out->count No existing hit has the requested URL.
 * @retval other Zero-based index of the matching hit.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_hit_index_of(const mdl_hit_list_t* out, const char* url)
{
  for (size_t i = 0U; i < out->count; ++i) {
    if (strcmp(out->hits[i].url, url) == 0) {
      return i;
    }
  }
  return out->count;
}

/** @brief Merge one resolved (url,title) hit; upgrade a slug with a real title.

 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[out] out Caller-owned result storage.
 * @param[in,out] real Per-hit real-title flags.
 * @param[in] url NUL-terminated URL input.
 * @param[in] title NUL-terminated display title.
 * @param[in] title_real Whether the title came from markup rather than a slug.
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
RA8_INTERNAL static ra8_err_t internal_merge_hit(mdl_hit_list_t* out,
                                                 bool*           real,
                                                 const char*     url,
                                                 const char*     title,
                                                 bool            title_real)
{
  const size_t at = internal_hit_index_of(out, url);
  if (at < out->count) {
    if (title_real && !real[at]) {
      (void)internal_copy_fits(out->hits[at].title, k_mdl_hit_title_max, title);
      real[at] = true;
    }
    return k_ra8_ok;
  }
  if (out->count >= (size_t)k_mdl_max_hits) {
    return k_ra8_err_no_mem;
  }
  (void)internal_copy_fits(out->hits[out->count].url, k_mdl_url_max, url);
  (void)internal_copy_fits(out->hits[out->count].title, k_mdl_hit_title_max, title);
  real[out->count] = title_real;
  out->count++;
  return k_ra8_ok;
}

/** @brief Resolve one anchor's href, count it, and emit a filtered hit.
 * @details Scans bounded anchor bytes and merges duplicate resolved URLs in place.
 *          Only complete titles and URLs fitting caller result storage publish.
 * @param[in] html Readable markup bytes.
 * @param[in] html_len Readable markup length.
 * @param[in] base_url NUL-terminated base URL.
 * @param[in] keep Optional substring filter.
 * @param[in] tag Readable tag byte span.
 * @param[in] tag_len Readable tag length.
 * @param[in] gt_off Offset of the tag-closing greater-than byte.
 * @param[out] out Caller-owned result storage.
 * @param[in,out] real Per-hit real-title flags.
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
RA8_INTERNAL static ra8_err_t internal_emit_hit(const char*     html,
                                                size_t          html_len,
                                                const char*     base_url,
                                                const char*     keep,
                                                const char*     tag,
                                                size_t          tag_len,
                                                size_t          gt_off,
                                                mdl_hit_list_t* out,
                                                bool*           real)
{
  char raw[k_mdl_url_max];
  if (!internal_find_attr_value(tag, tag_len, "href", raw, sizeof(raw))) {
    return k_ra8_ok; /* an <a> with no href is not a link */
  }
  char abs[k_mdl_url_max];
  if (!mdl_extract_resolve_url(base_url, raw, abs, sizeof(abs))) {
    return k_ra8_ok; /* unresolvable (e.g. javascript:, fragment): not counted
                      */
  }
  out->anchors_seen++;
  if (!internal_contains_ok(abs, keep)) {
    return k_ra8_ok; /* a real link, but not a result: counted, not stored */
  }
  char       title[k_mdl_hit_title_max];
  const bool title_real =
    internal_anchor_title(html, html_len, tag, tag_len, gt_off, title, sizeof(title));
  if (!title_real) {
    internal_url_slug(abs, title, sizeof(title));
  }
  return internal_merge_hit(out, real, abs, title, title_real);
}

ra8_err_t mdl_extract_hits(const char*     html,
                           size_t          html_len,
                           const char*     base_url,
                           const char*     url_contains,
                           mdl_hit_list_t* out)
{
  if ((html == nullptr) || (base_url == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  out->count        = 0U;
  out->anchors_seen = 0U;
  bool real[k_mdl_max_hits];
  memset(real, 0, sizeof(real));

  size_t pos = 0U;
  while (pos < html_len) {
    const char* tag = internal_find_ci(html + pos, html_len - pos, "<a");
    if (tag == nullptr) {
      break;
    }
    const size_t tag_off = (size_t)(tag - html);
    const size_t after   = tag_off + (sizeof("<a") - 1U);
    if ((after < html_len) && !internal_is_name_end(html[after])) {
      pos = tag_off + 1U; /* "<article" while seeking "<a" */
      continue;
    }
    const char*  gt      = memchr(tag, '>', html_len - tag_off);
    const size_t gt_off  = (gt == nullptr) ? html_len : (size_t)(gt - html);
    const size_t tag_len = (gt == nullptr) ? (html_len - tag_off) : (size_t)(gt - tag);
    if (internal_emit_hit(html,
                          html_len,
                          base_url,
                          url_contains,
                          tag,
                          tag_len,
                          gt_off,
                          out,
                          real) == k_ra8_err_no_mem) {
      return k_ra8_err_no_mem;
    }
    pos = tag_off + ((gt == nullptr) ? tag_len : (tag_len + 1U));
  }
  return k_ra8_ok;
}

/**
 * @brief Find one whitespace-delimited class token.
 * @details Uses exact token boundaries so substrings of longer class names do
 *          not match.
 * @param[in] classes NUL-terminated HTML class attribute.
 * @param[in] value   Non-empty class token to find.
 * @return Whether @p value occurs as one complete token.
 * @retval true  A boundary-delimited token matched.
 * @retval false No complete token matched.
 * @pre @p classes and @p value are non-NULL and NUL-terminated.
 * @pre @p value is non-empty.
 * @post Both inputs are unchanged.
 * @post No locale or global state is accessed.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_class_has_token(const char* classes, const char* value)
{
  const size_t value_len = strlen(value);
  const char*  at        = classes;
  while ((at = strstr(at, value)) != nullptr) {
    const bool left_ok  = (at == classes) || internal_is_ws(at[-1]);
    const char after    = at[value_len];
    const bool right_ok = (after == '\0') || internal_is_ws(after);
    if (left_ok && right_ok) {
      return true;
    }
    at += value_len;
  }
  return false;
}

/**
 * @brief Copy one non-empty literal selector value.
 * @details Rejects empty and over-capacity values without reporting success.
 * @param[in]  value NUL-terminated literal value.
 * @param[out] out   Destination character buffer.
 * @param[in]  cap   Destination capacity including NUL.
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok               The complete value was copied.
 * @retval k_ra8_err_not_found    The value was empty.
 * @retval k_ra8_err_invalid_size The complete value did not fit.
 * @pre @p value and @p out are non-NULL.
 * @pre @p cap is greater than zero and describes writable @p out storage.
 * @post On success, @p out is non-empty and NUL-terminated.
 * @post @p value is unchanged.
 * @note Thread-safe: uses only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_selector_copy(const char* value, char* out, size_t cap)
{
  if (value[0] == '\0') {
    return k_ra8_err_not_found;
  }
  if (!internal_copy_fits(out, cap, value)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Extract one matching HTML meta-content value.
 * @details Scans bounded markup for a `property` or `name` equal to @p key,
 *          then cleans its `content` value without truncation.
 * @param[in]  html Bounded HTML bytes.
 * @param[in]  len  Number of readable HTML bytes.
 * @param[in]  key  Meta property/name to match.
 * @param[out] out  Destination for cleaned content.
 * @param[in]  cap  Destination capacity including NUL.
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok               A non-empty value was extracted.
 * @retval k_ra8_err_not_found    No matching non-empty meta value exists.
 * @retval k_ra8_err_invalid_size The cleaned value did not fit.
 * @pre @p html, @p key, and @p out are non-NULL.
 * @pre @p cap is greater than zero and @p len bounds readable @p html bytes.
 * @post On success, @p out is non-empty and NUL-terminated.
 * @post No input bytes are modified.
 * @note Thread-safe: uses only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_extract_meta(const char* html, size_t len, const char* key, char* out, size_t cap)
{
  size_t pos = 0U;
  while (pos < len) {
    const char* tag = internal_find_ci(html + pos, len - pos, "<meta");
    if (tag == nullptr) {
      return k_ra8_err_not_found;
    }
    const size_t off = (size_t)(tag - html);
    const char*  gt  = memchr(tag, '>', len - off);
    if (gt == nullptr) {
      return k_ra8_err_not_found;
    }
    const size_t tag_len = (size_t)(gt - tag);
    char         name[k_mdl_hit_title_max];
    char         content[k_mdl_url_max];
    const bool   named = internal_find_attr_value(tag, tag_len, "property", name, sizeof(name)) ||
                         internal_find_attr_value(tag, tag_len, "name", name, sizeof(name));
    if (named && (strcmp(name, key) == 0) &&
        internal_find_attr_value(tag, tag_len, "content", content, sizeof(content))) {
      if (!internal_clean_inner_text(content, strlen(content), out, cap)) {
        return k_ra8_err_invalid_size;
      }
      return (out[0] == '\0') ? k_ra8_err_not_found : k_ra8_ok;
    }
    pos = off + tag_len + 1U;
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Extract visible text from the first matching class token.
 * @details Scans bounded start tags, matches one exact class token, and cleans
 *          the following element text without truncation.
 * @param[in]  html  Bounded HTML bytes.
 * @param[in]  len   Number of readable HTML bytes.
 * @param[in]  token Non-empty class token to match.
 * @param[out] out   Destination for cleaned visible text.
 * @param[in]  cap   Destination capacity including NUL.
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok               A non-empty value was extracted.
 * @retval k_ra8_err_not_found    No matching non-empty element exists.
 * @retval k_ra8_err_invalid_size The cleaned value did not fit.
 * @pre @p html, @p token, and @p out are non-NULL.
 * @pre @p token is non-empty and @p len bounds readable @p html bytes.
 * @post On success, @p out is non-empty and NUL-terminated.
 * @post No input bytes are modified.
 * @note Thread-safe: uses only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_extract_class(const char* html, size_t len, const char* token, char* out, size_t cap)
{
  size_t pos = 0U;
  while (pos < len) {
    const char* tag = memchr(html + pos, '<', len - pos);
    if (tag == nullptr) {
      return k_ra8_err_not_found;
    }
    const size_t off = (size_t)(tag - html);
    const char*  gt  = memchr(tag, '>', len - off);
    if (gt == nullptr) {
      return k_ra8_err_not_found;
    }
    const size_t tag_len = (size_t)(gt - tag);
    char         classes[k_mdl_hit_title_max];
    if (internal_find_attr_value(tag, tag_len, "class", classes, sizeof(classes)) &&
        internal_class_has_token(classes, token)) {
      const size_t inner_off = (size_t)(gt - html) + 1U;
      const char*  close     = internal_find_ci(html + inner_off, len - inner_off, "</");
      const size_t inner_len =
        (close == nullptr) ? (len - inner_off) : (size_t)(close - (html + inner_off));
      if (!internal_clean_inner_text(html + inner_off, inner_len, out, cap)) {
        return k_ra8_err_invalid_size;
      }
      return (out[0] == '\0') ? k_ra8_err_not_found : k_ra8_ok;
    }
    pos = off + tag_len + 1U;
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Extract the first anchor text following a visible label.
 * @details Finds @p label case-insensitively, locates the next anchor, and
 *          cleans its bounded inner text without truncation.
 * @param[in]  html  Bounded HTML bytes.
 * @param[in]  len   Number of readable HTML bytes.
 * @param[in]  label Non-empty visible label to find.
 * @param[out] out   Destination for cleaned anchor text.
 * @param[in]  cap   Destination capacity including NUL.
 * @return An ::ra8_err_t extraction result.
 * @retval k_ra8_ok               A non-empty anchor value was extracted.
 * @retval k_ra8_err_not_found    The label/anchor/value was absent.
 * @retval k_ra8_err_invalid_size The cleaned value did not fit.
 * @pre @p html, @p label, and @p out are non-NULL.
 * @pre @p label is NUL-terminated and @p len bounds readable @p html bytes.
 * @post On success, @p out is non-empty and NUL-terminated.
 * @post No input bytes are modified.
 * @note Thread-safe: uses only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_extract_label(const char* html, size_t len, const char* label, char* out, size_t cap)
{
  const char* hit = internal_find_ci(html, len, label);
  if (hit == nullptr) {
    return k_ra8_err_not_found;
  }
  const size_t after_label = (size_t)(hit - html) + strlen(label);
  const char*  anchor      = internal_find_ci(html + after_label, len - after_label, "<a");
  if (anchor == nullptr) {
    return k_ra8_err_not_found;
  }
  const size_t anchor_off = (size_t)(anchor - html);
  const char*  gt         = memchr(anchor, '>', len - anchor_off);
  if (gt == nullptr) {
    return k_ra8_err_not_found;
  }
  const size_t inner_off = (size_t)(gt - html) + 1U;
  const char*  close     = internal_find_ci(html + inner_off, len - inner_off, "</a");
  const size_t inner_len =
    (close == nullptr) ? (len - inner_off) : (size_t)(close - (html + inner_off));
  if (!internal_clean_inner_text(html + inner_off, inner_len, out, cap)) {
    return k_ra8_err_invalid_size;
  }
  return (out[0] == '\0') ? k_ra8_err_not_found : k_ra8_ok;
}

ra8_err_t mdl_extract_selector(const char* html,
                               size_t      html_len,
                               const char* selector,
                               char*       out,
                               size_t      out_cap)
{
  if ((html == nullptr) || (selector == nullptr) || (out == nullptr) || (out_cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  out[0]            = '\0';
  const char* value = strchr(selector, ':');
  if ((value == nullptr) || (value[1] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  ++value;
  if (strncmp(selector, "meta:", sizeof("meta:") - 1U) == 0) {
    return internal_extract_meta(html, html_len, value, out, out_cap);
  }
  if (strncmp(selector, "class:", sizeof("class:") - 1U) == 0) {
    return internal_extract_class(html, html_len, value, out, out_cap);
  }
  if (strncmp(selector, "label:", sizeof("label:") - 1U) == 0) {
    return internal_extract_label(html, html_len, value, out, out_cap);
  }
  if (strncmp(selector, "literal:", sizeof("literal:") - 1U) == 0) {
    return internal_selector_copy(value, out, out_cap);
  }
  return k_ra8_err_invalid_arg;
}
