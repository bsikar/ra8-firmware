/**
 * @file mdl_export_meta.c
 * @brief Parse, validate, and serialize portable publication metadata.
 *
 * @details Owns bounded key-value and ComicInfo parsing plus deterministic
 * ComicInfo generation. It performs no archive-format selection.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "mdl_sanitize.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"

/** @brief Radices and bounded metadata text expansion sizes. */
typedef enum : uint16_t {
  k_decimal_radix       = 10U,  /**< Decimal integer parsing radix.      */
  k_xml_amp_entity_len  = 5U,   /**< Bytes in the XML entity "&amp;".    */
  k_meta_line_slack     = 128U, /**< Key/delimiter space beyond a path.  */
  k_meta_fragment_slack = 64U,  /**< Fixed bytes around an XML fragment. */
} mdl_meta_text_bounds_t;

/**
 * @brief Validate an optional bounded source-attribution URL.
 * @details Requires complete NUL termination, an HTTP(S) scheme, a nonempty
 *          authority, and no whitespace/control bytes while permitting XML
 *          metacharacters that the container emitters escape later.
 * @param[in] url Fixed-capacity source URL field.
 * @return Source field validation status.
 * @retval k_ra8_ok The field is empty or a valid absolute HTTP(S) URL.
 * @retval k_ra8_err_invalid_size The fixed field lacks a terminating NUL.
 * @retval k_ra8_err_invalid_arg The nonempty URL has an invalid scheme,
 *                               authority, whitespace, or control byte.
 * @pre @p url points to at least ::k_mdl_meta_url_max readable bytes.
 * @pre The caller treats every non-ok result as a hard metadata failure.
 * @post @p url is not modified.
 * @post No allocation, network access, or filesystem access occurs.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_validate_source_url(const char* url)
{
  const size_t len = strnlen(url, (size_t)k_mdl_meta_url_max);
  if (len == (size_t)k_mdl_meta_url_max) {
    return k_ra8_err_invalid_size;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  char host[k_mdl_meta_url_max];
  if (!mdl_url_scheme_allowed(url) || !mdl_url_host(url, host, sizeof(host))) {
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < len; ++i) {
    if (isspace((unsigned char)url[i]) || iscntrl((unsigned char)url[i])) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}
/**
 * @brief Test whether an snprintf result fully fit its buffer
 * @details Rejects encoding errors and results equal to capacity because the
 *          terminating NUL would not fit.
 * @param[in] written Return value produced by snprintf.
 * @param[in] cap Destination capacity passed to snprintf.
 * @return Whether formatting completed without truncation.
 * @retval true @p written is nonnegative and below @p cap.
 * @retval false Formatting failed or truncated.
 * @pre @p cap matches the formatted destination.
 * @pre @p written is the unmodified snprintf result.
 * @post No state is modified.
 * @post A true result permits safe use of the destination string.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_export_snprintf_fit(int written, size_t cap)
{
  return (written >= 0) && ((size_t)written < cap);
}

void mdl_meta_init(mdl_export_meta_t* meta)
{
  if (meta == nullptr) {
    return;
  }
  memset(meta, 0, sizeof(*meta));
  meta->cover_index       = -1;
  meta->reading_direction = k_mdl_read_ltr;
  (void)snprintf(meta->language, sizeof(meta->language), "en");
  meta->modified[0] = '\0';
}

/**
 * @brief Copy trimmed text without truncating fixed metadata storage
 * @details Removes leading/trailing whitespace and fails if the complete text
 *          plus NUL cannot fit, leaving the destination unchanged on overflow.
 * @param[out] dst Destination string buffer.
 * @param[in] cap Writable capacity of @p dst.
 * @param[in] src NUL-terminated source text.
 * @return Whether the complete trimmed text fit.
 * @retval true @p dst received a NUL-terminated copy.
 * @retval false Capacity is zero or the value is too long.
 * @pre @p dst and @p src are non-NULL and do not overlap.
 * @pre @p src is NUL-terminated.
 * @post Success writes exactly the trimmed text.
 * @post Failure performs no partial copy.
 * @note Thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_str_copy_trimmed(char* dst, size_t cap, const char* src)
{
  while ((*src != '\0') && isspace((unsigned char)*src)) {
    src++;
  }
  size_t len = strlen(src);
  while ((len > 0U) && isspace((unsigned char)src[len - 1U])) {
    len--;
  }
  if ((cap == 0U) || (len >= cap)) {
    return false;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
  return true;
}

/**
 * @brief Parse one finite nonnegative decimal value strictly
 * @details Requires the entire trimmed string to parse without range errors.
 * @param[in] text NUL-terminated numeric text.
 * @param[out] out Parsed value on success.
 * @return Whether a valid value was parsed.
 * @retval true The complete input represents a finite nonnegative double.
 * @retval false Input is empty, malformed, negative, nonfinite, or out of
 * range.
 * @pre @p text and @p out are non-NULL.
 * @pre @p text is NUL-terminated.
 * @post Success stores the parsed value in @p out.
 * @post Input text is unchanged.
 * @note Thread-safe subject to the C library strtod implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_double_strict(const char* text, double* out)
{
  errno              = 0;
  char*        end   = nullptr;
  const double value = strtod(text, &end);
  while ((end != nullptr) && isspace((unsigned char)*end)) {
    ++end;
  }
  if ((end == text) || (end == nullptr) || (*end != '\0') || (errno == ERANGE) ||
      !isfinite(value) || (value < 0.0)) {
    return false;
  }
  *out = value;
  return true;
}

/**
 * @brief Parse one bounded decimal cover index strictly
 * @details Accepts minus one as the unset sentinel through INT_MAX and rejects
 * trailing junk.
 * @param[in] text NUL-terminated numeric text.
 * @param[out] out Parsed integer on success.
 * @return Whether a valid bounded index was parsed.
 * @retval true The complete input lies in the accepted range.
 * @retval false Input is malformed, below minus one, or above INT_MAX.
 * @pre @p text and @p out are non-NULL.
 * @pre @p text is NUL-terminated.
 * @post Success stores the parsed integer in @p out.
 * @post Input text is unchanged.
 * @note Thread-safe subject to the C library strtol implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_int_strict(const char* text, int* out)
{
  errno            = 0;
  char*      end   = nullptr;
  const long value = strtol(text, &end, k_decimal_radix);
  while ((end != nullptr) && isspace((unsigned char)*end)) {
    ++end;
  }
  if ((end == text) || (end == nullptr) || (*end != '\0') || (errno == ERANGE) || (value < -1L) ||
      (value > INT_MAX)) {
    return false;
  }
  *out = (int)value;
  return true;
}

/**
 * @brief Decode the five XML entities accepted by metadata input.
 * @details Copies one bounded raw element body and reports truncation.
 * @param[in] raw NUL-terminated raw element body.
 * @param[out] out Destination text buffer.
 * @param[in] cap Writable destination capacity.
 * @return Whether the entire decoded value fit.
 * @retval true The decoded value is complete.
 * @retval false The destination was exhausted.
 * @pre All pointers are non-NULL and @p cap is nonzero.
 * @pre @p raw is NUL-terminated.
 * @post @p out is NUL-terminated.
 * @post Failure never writes beyond @p cap.
 * @note Thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_xml_unescape(const char* raw, char* out, size_t cap)
{
  size_t r = 0U;
  size_t w = 0U;
  while ((raw[r] != '\0') && (w + 1U < cap)) {
    const char* entity   = nullptr;
    char        decoded  = '\0';
    size_t      consumed = 0U;
    if (raw[r] == '&') {
      if (strncmp(raw + r, "&amp;", k_xml_amp_entity_len) == 0) {
        entity  = "&amp;";
        decoded = '&';
      } else if (strncmp(raw + r, "&lt;", 4U) == 0) {
        entity  = "&lt;";
        decoded = '<';
      } else if (strncmp(raw + r, "&gt;", 4U) == 0) {
        entity  = "&gt;";
        decoded = '>';
      } else if (strncmp(raw + r, "&quot;", 6U) == 0) {
        entity  = "&quot;";
        decoded = '"';
      } else if (strncmp(raw + r, "&apos;", 6U) == 0) {
        entity  = "&apos;";
        decoded = '\'';
      }
    }
    consumed = (entity == nullptr) ? 1U : strlen(entity);
    out[w++] = (char)((entity == nullptr) ? raw[r] : decoded);
    r += consumed;
  }
  out[w] = '\0';
  return raw[r] == '\0';
}

/**
 * @brief Extract and unescape one bounded simple XML element.
 * @details Locates plain or attributed opening tags and decodes their body.
 * @param[in] xml NUL-terminated XML document.
 * @param[in] tag NUL-terminated element name.
 * @param[out] out Destination text buffer.
 * @param[in] cap Writable destination capacity.
 * @return Whether extraction completed without overflow.
 * @retval true The tag was absent or its value fit.
 * @retval false Raw or decoded text exceeded a bound.
 * @pre All pointers are non-NULL and inputs are NUL-terminated.
 * @pre @p out addresses @p cap writable bytes.
 * @post Accepted text is NUL-terminated.
 * @post Overflow remains visible.
 * @note Thread-safe across distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_parse_xml_tag(const char* xml, const char* tag, char* out, size_t cap)
{
  char open_tag[64];
  char close_tag[64];
  (void)snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
  (void)snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
  const char* start = strstr(xml, open_tag);
  if (start == nullptr) {
    (void)snprintf(open_tag, sizeof(open_tag), "<%s ", tag);
    start = strstr(xml, open_tag);
    start = (start == nullptr) ? nullptr : strchr(start, '>');
    start = (start == nullptr) ? nullptr : start + 1;
  } else {
    start += strlen(open_tag);
  }
  const char* end = (start == nullptr) ? nullptr : strstr(start, close_tag);
  if (end == nullptr) {
    return true;
  }
  const size_t len = (size_t)(end - start);
  char         raw[k_mdl_meta_path_max + 1U];
  if (len >= sizeof(raw)) {
    return false;
  }
  memcpy(raw, start, len);
  raw[len] = '\0';
  return internal_xml_unescape(raw, out, cap);
}

/** @brief Destination descriptor for a simple metadata XML element. */
typedef struct {
  const char* tag; /**< XML element name.           */
  char*       dst; /**< Fixed metadata destination. */
  size_t      cap; /**< Destination capacity.       */
} mdl_meta_xml_field_t;

/**
 * @brief Copy one optional XML field into fixed metadata storage.
 * @details Treats an absent or empty element as no update.
 * @param[in] text XML document.
 * @param[in] field Destination descriptor.
 * @return Field parsing status.
 * @retval k_ra8_ok The field was absent, empty, or copied completely.
 * @retval k_ra8_err_invalid_size Raw, decoded, or trimmed text did not fit.
 * @pre @p text and @p field are non-NULL.
 * @pre Descriptor members name live storage.
 * @post Success leaves the destination NUL-terminated.
 * @post Failure is reported without a truncated destination.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_xml_field(const char*                 text,
                                                       const mdl_meta_xml_field_t* field)
{
  char value[k_mdl_meta_path_max + 1U] = {};
  if (!internal_parse_xml_tag(text, field->tag, value, sizeof(value))) {
    return k_ra8_err_invalid_size;
  }
  if ((value[0] != '\0') && !internal_str_copy_trimmed(field->dst, field->cap, value)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Apply XML fields that require semantic validation.
 * @details Validates the source URL and parses number and reading direction.
 * @param[in,out] meta Metadata destination.
 * @param[in] text XML document.
 * @return Metadata parsing status.
 * @retval k_ra8_ok Every present supported field was accepted.
 * @retval k_ra8_err_invalid_size A bounded field overflowed.
 * @retval k_ra8_err_invalid_arg A URL or number was invalid.
 * @pre Both pointers are non-NULL and @p text is NUL-terminated.
 * @pre @p meta is exclusively owned.
 * @post Success applies every recognized field.
 * @post Source URL validation failure clears that field.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_xml_semantics(mdl_export_meta_t* meta,
                                                           const char*        text)
{
  char value[k_mdl_meta_path_max + 1U] = {};
  if (!internal_parse_xml_tag(text, "Web", value, sizeof(value))) {
    return k_ra8_err_invalid_size;
  }
  if ((value[0] != '\0') &&
      !internal_str_copy_trimmed(meta->source_url, sizeof(meta->source_url), value)) {
    return k_ra8_err_invalid_size;
  }
  if (value[0] != '\0') {
    const ra8_err_t rc = priv_mdl_export_validate_source_url(meta->source_url);
    if (rc != k_ra8_ok) {
      meta->source_url[0] = '\0';
      return rc;
    }
  }
  value[0] = '\0';
  if (!internal_parse_xml_tag(text, "Number", value, sizeof(value))) {
    return k_ra8_err_invalid_size;
  }
  if ((value[0] != '\0') && !internal_parse_double_strict(value, &meta->chapter_number)) {
    return k_ra8_err_invalid_arg;
  }
  value[0] = '\0';
  if (!internal_parse_xml_tag(text, "Manga", value, sizeof(value))) {
    return k_ra8_err_invalid_size;
  }
  if (strcmp(value, "YesAndRightToLeft") == 0) {
    meta->reading_direction = k_mdl_read_rtl;
  } else if (strcmp(value, "No") == 0) {
    meta->reading_direction = k_mdl_read_ltr;
  }
  return k_ra8_ok;
}

/**
 * @brief Parse ComicInfo-style XML into bounded metadata.
 * @details Applies simple text fields through a table, then validates URL,
 * number, and reading-direction fields with their semantic parsers.
 * @param[in,out] meta Metadata destination.
 * @param[in] text XML document.
 * @return Metadata parsing status.
 * @retval k_ra8_ok Every present supported field was accepted.
 * @retval k_ra8_err_invalid_size A bounded field overflowed.
 * @retval k_ra8_err_invalid_arg A URL or number was invalid.
 * @pre Both pointers are non-NULL and @p text is NUL-terminated.
 * @pre @p meta is exclusively owned.
 * @post Success applies every recognized field.
 * @post Source URL validation failure clears that field.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_xml(mdl_export_meta_t* meta, const char* text)
{
  mdl_meta_xml_field_t fields[] = {{"Title", meta->chapter_title, sizeof(meta->chapter_title)},
                                   {"Series", meta->series_title, sizeof(meta->series_title)},
                                   {"Summary", meta->summary, sizeof(meta->summary)},
                                   {"Writer", meta->writer, sizeof(meta->writer)},
                                   {"Artist", meta->artist, sizeof(meta->artist)},
                                   {"LanguageISO", meta->language, sizeof(meta->language)},
                                   {"CoverImage", meta->cover_path, sizeof(meta->cover_path)}};
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    const ra8_err_t rc = internal_parse_xml_field(text, &fields[i]);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  return internal_parse_xml_semantics(meta, text);
}

/**
 * @brief Compare one normalized metadata key with up to five aliases.
 * @details NULL aliases are ignored.
 * @param[in] key Normalized key.
 * @param[in] a First alias.
 * @param[in] b Second alias, or NULL.
 * @param[in] c Third alias, or NULL.
 * @param[in] d Fourth alias, or NULL.
 * @param[in] e Fifth alias, or NULL.
 * @return Whether any supplied alias matches.
 * @retval true One alias matched exactly.
 * @retval false No alias matched.
 * @pre @p key and @p a are non-NULL.
 * @pre Every non-NULL alias is NUL-terminated.
 * @post Inputs are unchanged.
 * @post Matching is case-sensitive after normalization.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_key_is(const char* key,
                                         const char* a,
                                         const char* b,
                                         const char* c,
                                         const char* d,
                                         const char* e)
{
  return (strcmp(key, a) == 0) || ((b != nullptr) && (strcmp(key, b) == 0)) ||
         ((c != nullptr) && (strcmp(key, c) == 0)) || ((d != nullptr) && (strcmp(key, d) == 0)) ||
         ((e != nullptr) && (strcmp(key, e) == 0));
}

/**
 * @brief Resolve title and descriptive text aliases.
 * @details Maps the supported title, series, and descriptive aliases.
 * @param[in,out] meta Metadata destination.
 * @param[in] key Normalized key.
 * @param[out] dst Receives the selected metadata text buffer.
 * @param[out] cap Receives the selected buffer capacity in bytes.
 * @return Whether @p key names a supported descriptive field.
 * @retval true @p dst and @p cap identify the selected field.
 * @retval false @p key is not a descriptive alias.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p meta is exclusively owned.
 * @post Recognized keys initialize both @p dst and @p cap.
 * @post Unknown keys leave metadata unchanged.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_find_descriptive_key(mdl_export_meta_t* meta, const char* key, char** dst, size_t* cap)
{
  if (internal_key_is(key, "series", "series_title", "seriestitle", "series title", nullptr)) {
    *dst = meta->series_title;
    *cap = sizeof(meta->series_title);
  } else if (internal_key_is(key, "summary", "description", "abstract", nullptr, nullptr)) {
    *dst = meta->summary;
    *cap = sizeof(meta->summary);
  } else if (internal_key_is(key,
                             "chapter_title",
                             "chaptertitle",
                             "chapter title",
                             "title",
                             nullptr)) {
    *dst = meta->chapter_title;
    *cap = sizeof(meta->chapter_title);
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Resolve attribution and publication text aliases.
 * @details Maps supported creator, cover, language, identity, and date aliases.
 * @param[in,out] meta Metadata destination owning the returned storage.
 * @param[in] key Normalized key.
 * @param[out] dst Resolved destination pointer.
 * @param[out] cap Resolved destination capacity.
 * @return Whether the key names a supported text destination.
 * @retval true Output parameters identify one metadata field.
 * @retval false The key is not handled by this resolver.
 * @pre All pointers are non-NULL.
 * @pre @p key is NUL-terminated.
 * @post Success initializes both output parameters.
 * @post Failure leaves output parameters unchanged.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_find_publication_key(mdl_export_meta_t* meta, const char* key, char** dst, size_t* cap)
{
  if (internal_key_is(key, "writer", "author", "creator", nullptr, nullptr)) {
    *dst = meta->writer;
    *cap = sizeof(meta->writer);
  } else if (internal_key_is(key, "artist", "penciller", "illustrator", nullptr, nullptr)) {
    *dst = meta->artist;
    *cap = sizeof(meta->artist);
  } else if (internal_key_is(key,
                             "cover",
                             "cover_path",
                             "coverpath",
                             "cover path",
                             "cover_image")) {
    *dst = meta->cover_path;
    *cap = sizeof(meta->cover_path);
  } else if (internal_key_is(key, "language", "language_iso", "lang", nullptr, nullptr)) {
    *dst = meta->language;
    *cap = sizeof(meta->language);
  } else if (internal_key_is(key, "identifier", "uuid", nullptr, nullptr, nullptr)) {
    *dst = meta->identifier;
    *cap = sizeof(meta->identifier);
  } else if (internal_key_is(key, "modified", "date_modified", nullptr, nullptr, nullptr)) {
    *dst = meta->modified;
    *cap = sizeof(meta->modified);
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Apply one text-valued metadata key.
 * @details Resolves aliases to one bounded destination and leaves unknown keys
 * distinguishable for numeric handling.
 * @param[in,out] meta Metadata destination.
 * @param[in] key Normalized key.
 * @param[in] value Raw value.
 * @return Text-field application status.
 * @retval k_ra8_ok A recognized text field was copied.
 * @retval k_ra8_err_not_found The key is not a text field.
 * @retval k_ra8_err_invalid_size The value did not fit.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p meta is exclusively owned.
 * @post Recognized values are trimmed and NUL-terminated.
 * @post Unknown keys leave metadata unchanged.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_apply_text_key(mdl_export_meta_t* meta, const char* key, const char* value)
{
  char*  dst = nullptr;
  size_t cap = 0U;
  if (!internal_find_descriptive_key(meta, key, &dst, &cap) &&
      !internal_find_publication_key(meta, key, &dst, &cap)) {
    return k_ra8_err_not_found;
  }
  return internal_str_copy_trimmed(dst, cap, value) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Apply one semantic metadata key.
 * @details Handles URL, chapter number, cover index, and reading direction.
 * @param[in,out] meta Metadata destination.
 * @param[in] key Normalized key.
 * @param[in] value Raw value.
 * @return Semantic-field application status.
 * @retval k_ra8_ok The key was unknown or its value was accepted.
 * @retval k_ra8_err_invalid_size Temporary or destination storage overflowed.
 * @retval k_ra8_err_invalid_arg A semantic value was malformed.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p meta is exclusively owned.
 * @post Recognized fields are updated only from complete values.
 * @post Invalid source URLs are cleared.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_apply_semantic_key(mdl_export_meta_t* meta, const char* key, const char* value)
{
  char parsed[64];
  if (internal_key_is(key, "source_url", "source", "web", nullptr, nullptr)) {
    if (!internal_str_copy_trimmed(meta->source_url, sizeof(meta->source_url), value)) {
      return k_ra8_err_invalid_size;
    }
    const ra8_err_t rc = priv_mdl_export_validate_source_url(meta->source_url);
    if (rc != k_ra8_ok) {
      meta->source_url[0] = '\0';
    }
    return rc;
  }
  if (internal_key_is(key, "number", "chapter_number", "chapternumber", "chapter number", "num")) {
    if (!internal_str_copy_trimmed(parsed, sizeof(parsed), value)) {
      return k_ra8_err_invalid_size;
    }
    return internal_parse_double_strict(parsed, &meta->chapter_number) ? k_ra8_ok
                                                                       : k_ra8_err_invalid_arg;
  }
  if (internal_key_is(key, "cover_index", "coverindex", "cover index", "cover_idx", nullptr)) {
    if (!internal_str_copy_trimmed(parsed, sizeof(parsed), value)) {
      return k_ra8_err_invalid_size;
    }
    return internal_parse_int_strict(parsed, &meta->cover_index) ? k_ra8_ok : k_ra8_err_invalid_arg;
  }
  if (internal_key_is(key,
                      "reading_direction",
                      "direction",
                      "page_progression",
                      nullptr,
                      nullptr)) {
    if (!internal_str_copy_trimmed(parsed, sizeof(parsed), value)) {
      return k_ra8_err_invalid_size;
    }
    if (strcmp(parsed, "rtl") == 0) {
      meta->reading_direction = k_mdl_read_rtl;
    } else if (strcmp(parsed, "ltr") == 0) {
      meta->reading_direction = k_mdl_read_ltr;
    } else {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse and apply one metadata descriptor line.
 * @details Ignores comments, blanks, malformed separators, and unknown keys.
 * @param[in,out] meta Metadata destination.
 * @param[in,out] line Writable NUL-terminated line.
 * @return Line application status.
 * @retval k_ra8_ok The line was ignored or applied.
 * @retval k_ra8_err_invalid_size A key or value exceeded a bound.
 * @retval k_ra8_err_invalid_arg A semantic value was malformed.
 * @pre Both pointers are non-NULL and @p line is writable.
 * @pre @p meta is exclusively owned.
 * @post Recognized keys update exactly one field.
 * @post Unknown keys leave metadata unchanged.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_kv_line(mdl_export_meta_t* meta, char* line)
{
  char* start = line;
  while ((*start != '\0') && isspace((unsigned char)*start)) {
    ++start;
  }
  if ((*start == '\0') || (*start == '#') || (*start == ';')) {
    return k_ra8_ok;
  }
  char* separator = strchr(start, '=');
  separator       = (separator == nullptr) ? strchr(start, ':') : separator;
  if (separator == nullptr) {
    return k_ra8_ok;
  }
  *separator = '\0';
  char key[128];
  if (!internal_str_copy_trimmed(key, sizeof(key), start)) {
    return k_ra8_err_invalid_size;
  }
  for (size_t i = 0U; key[i] != '\0'; ++i) {
    key[i] = (char)tolower((unsigned char)key[i]);
  }
  ra8_err_t rc = internal_apply_text_key(meta, key, separator + 1);
  if (rc == k_ra8_err_not_found) {
    rc = internal_apply_semantic_key(meta, key, separator + 1);
  }
  return rc;
}

ra8_err_t mdl_meta_parse(mdl_export_meta_t* meta, const char* text)
{
  if ((meta == nullptr) || (text == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((strstr(text, "<ComicInfo") != nullptr) || (strstr(text, "<Title>") != nullptr) ||
      (strstr(text, "<Series>") != nullptr) || (strstr(text, "<Web>") != nullptr)) {
    return internal_parse_xml(meta, text);
  }
  const char* cursor = text;
  while (*cursor != '\0') {
    const char*  end    = strchr(cursor, '\n');
    const size_t length = (end == nullptr) ? strlen(cursor) : (size_t)(end - cursor);
    char         line[k_mdl_meta_path_max + k_meta_line_slack];
    if (length >= sizeof(line)) {
      return k_ra8_err_invalid_size;
    }
    memcpy(line, cursor, length);
    line[length]       = '\0';
    const ra8_err_t rc = internal_parse_kv_line(meta, line);
    if (rc != k_ra8_ok) {
      return rc;
    }
    cursor += length;
    if (*cursor == '\n') {
      ++cursor;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Compose one metadata candidate without a traversal component
 * @details Resolves the fixed candidate relative to the canonical chapter or
 *          its parent without emitting a traversal segment.
 * @param[in] directory Canonical chapter directory.
 * @param[in] candidate Candidate leaf.
 * @param[in] parent Whether the candidate belongs in the parent directory.
 * @param[out] out Composed canonical path.
 * @param[in] capacity Destination capacity.
 * @return Canonical path construction status.
 * @retval k_ra8_ok The complete candidate path fits.
 * @retval k_ra8_err_invalid_size The bounded destination is insufficient.
 * @pre Inputs are non-null and NUL-terminated.
 * @pre @p out is writable for @p capacity bytes.
 * @post Success never emits `.` or `..` components.
 * @post Failure never claims a truncated candidate as valid.
 * @note Thread-safe across distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_meta_candidate_path(const char* directory,
                                                           const char* candidate,
                                                           bool        parent,
                                                           char*       out,
                                                           size_t      capacity)
{
  if (!parent) {
    return priv_mdl_export_path_join(out, capacity, directory, candidate);
  }
  char   parent_path[k_fw_fs_path_cap];
  size_t length = strnlen(directory, sizeof(parent_path));
  if ((length == 0U) || (length >= sizeof(parent_path))) {
    return k_ra8_err_invalid_arg;
  }
  memcpy(parent_path, directory, length + 1U);
  while ((length > 1U) && (parent_path[length - 1U] != '/')) {
    --length;
  }
  if (length > 1U) {
    parent_path[length - 1U] = '\0';
  } else {
    parent_path[1] = '\0';
  }
  return priv_mdl_export_path_join(out, capacity, parent_path, candidate);
}

/**
 * @brief Load and parse one metadata candidate file, if present.
 * @details Resolves the candidate path, stats it, and -- when it exists as a
 * regular nonempty file -- slurps and parses it into @p meta.
 * @param[in,out] storage Portable namespace binding.
 * @param[in] dir Canonical chapter directory.
 * @param[in] candidate Candidate file basename.
 * @param[in] is_fallback Whether @p candidate is the final fallback name.
 * @param[in,out] meta Accumulated metadata being filled in.
 * @return Candidate status.
 * @retval k_ra8_ok The candidate was absent, empty, or parsed successfully.
 * @retval other Path resolution, stat, read, or parse failed.
 * @pre @p storage and @p meta are non-NULL. @pre @p dir and @p candidate resolve under @p storage.
 * @post On success @p meta reflects the parsed candidate, if any was found.
 * @post No file under @p dir is created, modified, or removed.
 * @note Not thread-safe with respect to concurrent mutation of @p storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_meta_load_candidate(mdl_storage_t*     storage,
                                                           const char*        dir,
                                                           const char*        candidate,
                                                           bool               is_fallback,
                                                           mdl_export_meta_t* meta)
{
  char      path[k_fw_fs_path_cap];
  ra8_err_t err = internal_meta_candidate_path(dir, candidate, is_fallback, path, sizeof(path));
  if (err != k_ra8_ok) {
    return err;
  }
  fw_fs_stat_t stat = {};
  err               = fw_fs_stat(&storage->fs->names, path, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_ok;
  }
  if (stat.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  if (stat.size_bytes == 0U) {
    return k_ra8_ok;
  }
  char buf[4096];
  if (stat.size_bytes >= sizeof(buf)) {
    return k_ra8_err_invalid_size;
  }
  size_t got = 0U;
  err        = priv_mdl_export_source_slurp(storage, path, (uint8_t*)buf, sizeof(buf) - 1U, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  buf[got] = '\0';
  return mdl_meta_parse(meta, buf);
}

ra8_err_t mdl_meta_load_dir(mdl_storage_t* storage, mdl_export_meta_t* meta, const char* dir)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (meta == nullptr) || (dir == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_meta_init(meta);

  static const char* const candidate_files[] = {"metadata.txt",
                                                "ComicInfo.xml",
                                                ".mdl_meta",
                                                "metadata.conf",
                                                "metadata.txt"};

  for (size_t i = 0U; i < (sizeof(candidate_files) / sizeof(candidate_files[0])); ++i) {
    const ra8_err_t err =
      internal_meta_load_candidate(storage, dir, candidate_files[i], i == 4U, meta);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/** @brief Escaped text storage used while rendering ComicInfo. */
typedef struct {
  char title[k_mdl_meta_title_max * 6U];     /**< Escaped chapter title. */
  char series[k_mdl_meta_title_max * 6U];    /**< Escaped series title.  */
  char summary[k_mdl_meta_summary_max * 6U]; /**< Escaped summary.       */
  char writer[k_mdl_meta_name_max * 6U];     /**< Escaped writer.        */
  char artist[k_mdl_meta_name_max * 6U];     /**< Escaped artist.        */
  char language[k_mdl_meta_lang_max * 6U];   /**< Escaped language.      */
  char source[k_mdl_meta_url_max * 6U];      /**< Escaped source URL.    */
} mdl_comicinfo_text_t;

/**
 * @brief Escape metadata fields with the original fallback policy.
 * @details Builds escape comicinfo within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in] meta Metadata record to read or update.
 * @param[in,out] escaped Caller-owned escaped metadata fields to populate.
 * @return Canonical downloader status.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_escape_comicinfo(const mdl_export_meta_t* meta,
                                                        mdl_comicinfo_text_t*    escaped)
{
  const char* title_fallback = (meta->series_title[0] != '\0') ? meta->series_title : "Chapter";
  const char* title  = (meta->chapter_title[0] != '\0') ? meta->chapter_title : title_fallback;
  const char* series = (meta->series_title[0] != '\0') ? meta->series_title : "Series";
  if (!mdl_xml_escape(title, escaped->title, sizeof(escaped->title))) {
    (void)snprintf(escaped->title, sizeof(escaped->title), "Chapter");
  }
  if (!mdl_xml_escape(series, escaped->series, sizeof(escaped->series))) {
    (void)snprintf(escaped->series, sizeof(escaped->series), "Series");
  }
  if (!mdl_xml_escape(meta->summary, escaped->summary, sizeof(escaped->summary))) {
    escaped->summary[0] = '\0';
  }
  if (!mdl_xml_escape(meta->writer, escaped->writer, sizeof(escaped->writer))) {
    escaped->writer[0] = '\0';
  }
  if (!mdl_xml_escape(meta->artist, escaped->artist, sizeof(escaped->artist))) {
    escaped->artist[0] = '\0';
  }
  return (mdl_xml_escape(meta->language, escaped->language, sizeof(escaped->language)) &&
          mdl_xml_escape(meta->source_url, escaped->source, sizeof(escaped->source)))
           ? k_ra8_ok
           : k_ra8_err_invalid_size;
}

/**
 * @brief Render already-escaped ComicInfo fields into caller storage.
 * @details Builds render comicinfo within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in] meta Metadata record to read or update.
 * @param[in] escaped Validated escaped metadata fields to render.
 * @param[in] page_count Number of pages represented in the archive.
 * @param[in] web Web-reader metadata value.
 * @param[in] number Chapter-number metadata value.
 * @param[in] pages Optional caller page metadata.
 * @param[out] buf Caller-provided bounded buffer.
 * @param[in] cap Capacity of the associated output buffer in bytes.
 * @return Canonical downloader status.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post No ownership of caller-provided storage is transferred.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note The function performs no dynamic allocation and retains no caller pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_render_comicinfo(const mdl_export_meta_t*    meta,
                                                        const mdl_comicinfo_text_t* escaped,
                                                        size_t                      page_count,
                                                        const char*                 web,
                                                        const char*                 number,
                                                        const char*                 pages,
                                                        char*                       buf,
                                                        size_t                      cap)
{
  const char* manga = (meta->reading_direction == k_mdl_read_rtl) ? "YesAndRightToLeft" : "No";
  const int written = snprintf(buf,
                               cap,
                               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                               "<ComicInfo xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                               "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
                               "  <Title>%s</Title>\n  <Series>%s</Series>\n  <Number>%s</Number>\n"
                               "  <Summary>%s</Summary>\n  <Writer>%s</Writer>\n  "
                               "<Artist>%s</Artist>\n%s"
                               "  <PageCount>%zu</PageCount>\n  <LanguageISO>%s</LanguageISO>\n"
                               "  <Manga>%s</Manga>\n%s</ComicInfo>",
                               escaped->title,
                               escaped->series,
                               number,
                               escaped->summary,
                               escaped->writer,
                               escaped->artist,
                               web,
                               page_count,
                               escaped->language,
                               manga,
                               pages);
  return priv_mdl_export_snprintf_fit(written, cap) ? k_ra8_ok : k_ra8_err_invalid_size;
}

ra8_err_t mdl_export_build_comicinfo_pages(const mdl_export_meta_t* meta,
                                           size_t                   page_count,
                                           char*                    buf,
                                           size_t                   cap)
{
  if ((buf == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_export_meta_t resolved;
  if (meta != nullptr) {
    resolved = *meta;
  } else {
    mdl_meta_init(&resolved);
  }
  ra8_err_t            rc      = priv_mdl_export_validate_source_url(resolved.source_url);
  mdl_comicinfo_text_t escaped = {};
  if (rc == k_ra8_ok) {
    rc = internal_escape_comicinfo(&resolved, &escaped);
  }
  if (rc != k_ra8_ok) {
    return rc;
  }
  char web[k_mdl_meta_url_max * 6U + k_meta_fragment_slack] = {};
  if (escaped.source[0] != '\0') {
    const int n = snprintf(web, sizeof(web), "  <Web>%s</Web>\n", escaped.source);
    if (!priv_mdl_export_snprintf_fit(n, sizeof(web))) {
      return k_ra8_err_invalid_size;
    }
  }
  char number[32];
  if (resolved.chapter_number <= 0.0) {
    (void)snprintf(number, sizeof(number), "1");
  } else if (resolved.chapter_number == (double)(long)resolved.chapter_number) {
    (void)snprintf(number, sizeof(number), "%ld", (long)resolved.chapter_number);
  } else {
    (void)snprintf(number, sizeof(number), "%.1f", resolved.chapter_number);
  }
  char pages[160] = {};
  if ((resolved.cover_index >= 0) && ((size_t)resolved.cover_index < page_count)) {
    const int n = snprintf(pages,
                           sizeof(pages),
                           "  <Pages><Page Image=\"%d\" Type=\"FrontCover\"/></Pages>\n",
                           resolved.cover_index);
    if (!priv_mdl_export_snprintf_fit(n, sizeof(pages))) {
      return k_ra8_err_invalid_size;
    }
  }
  return internal_render_comicinfo(&resolved, &escaped, page_count, web, number, pages, buf, cap);
}

ra8_err_t mdl_export_build_comicinfo(const mdl_export_meta_t* meta, char* buf, size_t cap)
{
  return mdl_export_build_comicinfo_pages(meta, 0U, buf, cap);
}
