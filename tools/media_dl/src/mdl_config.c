/**
 * @file mdl_config.c
 * @brief Flat key=value site-descriptor parser (host stdio).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_politeness.h"
#include "ra8_attributes.h"

/** @brief Local parser limits. */
typedef enum : uint16_t {
  k_line_max = 512, /**< Max config line length. */
} mdl_parse_limits_t;

/** @brief Radix for integer config values. */
typedef enum : uint8_t {
  k_dec_base = 10, /**< Base-10 for strtoul(). */
} mdl_numparse_t;

/** @brief Result of applying one descriptor key. */
typedef enum : uint8_t {
  k_kv_unknown = 0, /**< Key is not owned by this helper.   */
  k_kv_applied = 1, /**< Key and value were accepted.       */
  k_kv_invalid = 2, /**< Key is known but value is invalid. */
} mdl_kv_result_t;

/** @brief Default politeness bounds (ms) applied before the file is read. */
typedef enum : uint16_t {
  k_def_img_delay_min     = 500,  /**< Per-image spacing floor.       */
  k_def_img_delay_max     = 1200, /**< Per-image spacing ceiling.     */
  k_def_chapter_delay_min = 1500, /**< Inter-chapter spacing floor.   */
  k_def_chapter_delay_max = 3000, /**< Inter-chapter spacing ceiling. */
} mdl_config_defaults_t;

/** @brief `--polite` per-request delay floors (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms  = 2000,  /**< Polite per-image floor.       */
  k_polite_img_max_ms  = 4000,  /**< Polite per-image ceiling.     */
  k_polite_chap_min_ms = 5000,  /**< Polite inter-chapter floor.   */
  k_polite_chap_max_ms = 10000, /**< Polite inter-chapter ceiling. */
} mdl_config_polite_floor_t;

/** @brief Trim leading/trailing ASCII whitespace in place; return start. */
RA8_INTERNAL static char* trim(char* s)
{
  while ((*s == ' ') || (*s == '\t') || (*s == '\r') || (*s == '\n')) {
    ++s;
  }
  size_t n = strlen(s);
  while ((n > 0U) && ((s[n - 1U] == ' ') || (s[n - 1U] == '\t') || (s[n - 1U] == '\r') ||
                      (s[n - 1U] == '\n'))) {
    s[n - 1U] = '\0';
    --n;
  }
  return s;
}

/**
 * @brief Copy one descriptor value without truncation.
 * @details Measures the complete source before copying it into fixed storage.
 * @param[out] dst Destination character array.
 * @param[in]  cap Destination capacity including NUL.
 * @param[in]  val NUL-terminated source value.
 * @return Whether the complete value fit and was copied.
 * @retval true  @p dst contains an exact copy of @p val.
 * @retval false @p val did not fit and @p dst is unchanged.
 * @pre @p dst and @p val are non-NULL.
 * @pre @p cap is greater than zero and describes writable @p dst storage.
 * @post On true, @p dst is NUL-terminated.
 * @post @p val is unchanged.
 * @note Not thread-safe when source and destination storage are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static bool set_str(char* dst, size_t cap, const char* val)
{
  const size_t len = strlen(val);
  if (len >= cap) {
    return false;
  }
  memcpy(dst, val, len + 1U);
  return true;
}

/** @brief Apply a string-valued key and distinguish unknown from invalid. */
RA8_INTERNAL static mdl_kv_result_t apply_kv_str(mdl_site_t* s, const char* key, const char* val)
{
  const struct {
    const char* key; /**< Config key spelling.       */
    char*       dst; /**< Descriptor field it fills. */
    size_t      cap; /**< Capacity of that field.    */
  } fields[] = {
    {"name", s->name, sizeof(s->name)},
    {"host", s->host, sizeof(s->host)},
    {"kind", s->kind, sizeof(s->kind)},
    {"contact", s->contact, sizeof(s->contact)},
    {"chapter_url_contains", s->chapter_url_contains, sizeof(s->chapter_url_contains)},
    {"chapter_url_prefix", s->chapter_url_prefix, sizeof(s->chapter_url_prefix)},
    {"page_img_attr", s->page_img_attr, sizeof(s->page_img_attr)},
    {"page_img_url_contains", s->page_img_url_contains, sizeof(s->page_img_url_contains)},
    {"search_url", s->search_url, sizeof(s->search_url)},
    {"search_result_contains", s->search_result_contains, sizeof(s->search_result_contains)},
    {"browse_url", s->browse_url, sizeof(s->browse_url)},
    {"series_title_selector", s->series_title_selector, sizeof(s->series_title_selector)},
    {"series_summary_selector", s->series_summary_selector, sizeof(s->series_summary_selector)},
    {"series_author_selector", s->series_author_selector, sizeof(s->series_author_selector)},
    {"series_artist_selector", s->series_artist_selector, sizeof(s->series_artist_selector)},
    {"series_cover_selector", s->series_cover_selector, sizeof(s->series_cover_selector)},
    {"chapter_title_selector", s->chapter_title_selector, sizeof(s->chapter_title_selector)},
    {"chapter_number_selector", s->chapter_number_selector, sizeof(s->chapter_number_selector)},
    {"language", s->language, sizeof(s->language)},
    {"reading_direction", s->reading_direction, sizeof(s->reading_direction)},
  };
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    if (strcmp(key, fields[i].key) == 0) {
      return set_str(fields[i].dst, fields[i].cap, val) ? k_kv_applied : k_kv_invalid;
    }
  }
  return k_kv_unknown;
}

/**
 * @brief Parse one complete unsigned 32-bit decimal field.
 * @details Rejects signs, empty text, overflow, and trailing characters.
 * @param[in]  val NUL-terminated decimal text.
 * @param[out] out Parsed integer destination.
 * @return Whether one complete in-range integer was parsed.
 * @retval true  @p out received the parsed value.
 * @retval false The text was empty, signed, malformed, or out of range.
 * @pre @p val and @p out are non-NULL.
 * @pre @p val is NUL-terminated.
 * @post On true, @p out contains the exact parsed value.
 * @post On false, @p out is unchanged.
 * @note Thread-safe except for the C-library thread-local `errno`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_u32(const char* val, uint32_t* out)
{
  if ((val[0] == '\0') || (val[0] == '+') || (val[0] == '-')) {
    return false;
  }
  errno                      = 0;
  char*               end    = nullptr;
  const unsigned long parsed = strtoul(val, &end, (int)k_dec_base);
  if ((errno != 0) || (end == val) || (*end != '\0') || (parsed > (unsigned long)UINT32_MAX)) {
    return false;
  }
  *out = (uint32_t)parsed;
  return true;
}

/** @brief Apply an unsigned-integer key (jitter delay or governor bound). */
RA8_INTERNAL static mdl_kv_result_t apply_kv_u32(mdl_site_t* s, const char* key, const char* val)
{
  const struct {
    const char* key; /**< Config key spelling.       */
    uint32_t*   dst; /**< Descriptor field it fills. */
  } fields[] = {
    {"img_delay_min", &s->img_delay_min},
    {"img_delay_max", &s->img_delay_max},
    {"chapter_delay_min", &s->chapter_delay_min},
    {"chapter_delay_max", &s->chapter_delay_max},
    {"rate_per_min", &s->rate_per_min},
    {"burst", &s->burst},
    {"backoff_base_ms", &s->backoff_base_ms},
    {"backoff_max_ms", &s->backoff_max_ms},
    {"max_inflight", &s->max_inflight},
  };
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    if (strcmp(key, fields[i].key) == 0) {
      return parse_u32(val, fields[i].dst) ? k_kv_applied : k_kv_invalid;
    }
  }
  return k_kv_unknown;
}

/**
 * @brief Parse the configured chapter-order spelling.
 * @details Accepts only `reverse`, `asc`, or `doc`; there is no silent default.
 * @param[in]  val NUL-terminated order spelling.
 * @param[out] out Parsed order destination.
 * @return Whether @p val is one supported spelling.
 * @retval true  @p out received the corresponding enum value.
 * @retval false @p val is unsupported and @p out is unchanged.
 * @pre @p val and @p out are non-NULL.
 * @pre @p val is NUL-terminated.
 * @post On true, @p out is a valid ::mdl_chapter_order_t value.
 * @post @p val is unchanged.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_order(const char* val, mdl_chapter_order_t* out)
{
  if (strcmp(val, "reverse") == 0) {
    *out = k_mdl_order_reverse;
    return true;
  }
  if (strcmp(val, "asc") == 0) {
    *out = k_mdl_order_asc;
    return true;
  }
  if (strcmp(val, "doc") == 0) {
    *out = k_mdl_order_doc;
    return true;
  }
  return false;
}

/**
 * @brief Apply one descriptor key/value pair.
 * @details Dispatches string, unsigned-integer, and chapter-order keys while
 *          rejecting unknown names and malformed values.
 * @param[in,out] s   Site descriptor being populated.
 * @param[in]     key NUL-terminated key spelling.
 * @param[in]     val NUL-terminated value text.
 * @return Whether the key is known and its complete value is valid.
 * @retval true  The matching descriptor field was updated.
 * @retval false The key is unknown or its value is invalid.
 * @pre @p s, @p key, and @p val are non-NULL.
 * @pre Both string arguments are NUL-terminated.
 * @post On true, exactly the selected field is updated.
 * @post On false, no truncated string is stored.
 * @note Not thread-safe: mutates caller-owned descriptor storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool apply_kv(mdl_site_t* s, const char* key, const char* val)
{
  const mdl_kv_result_t str_result = apply_kv_str(s, key, val);
  if (str_result != k_kv_unknown) {
    return str_result == k_kv_applied;
  }
  const mdl_kv_result_t int_result = apply_kv_u32(s, key, val);
  if (int_result != k_kv_unknown) {
    return int_result == k_kv_applied;
  }
  if (strcmp(key, "chapter_order") == 0) {
    return parse_order(val, &s->chapter_order);
  }
  return false;
}

/** @brief Apply the polite default descriptor before the file overrides it. */
RA8_INTERNAL static void config_set_defaults(mdl_site_t* out)
{
  *out = (mdl_site_t){};
  (void)set_str(out->name, sizeof(out->name), "site");
  (void)set_str(out->kind, sizeof(out->kind), "manhwa");
  (void)set_str(out->chapter_url_contains, sizeof(out->chapter_url_contains), "chapter");
  (void)set_str(out->page_img_attr, sizeof(out->page_img_attr), "data-src");
  (void)set_str(out->language, sizeof(out->language), "en");
  (void)set_str(out->reading_direction, sizeof(out->reading_direction), "ltr");
  out->chapter_order     = k_mdl_order_asc;
  out->img_delay_min     = (uint32_t)k_def_img_delay_min;
  out->img_delay_max     = (uint32_t)k_def_img_delay_max;
  out->chapter_delay_min = (uint32_t)k_def_chapter_delay_min;
  out->chapter_delay_max = (uint32_t)k_def_chapter_delay_max;

  /* Governor bounds default to the conservative single source of truth. */
  const mdl_gov_cfg_t gov = mdl_gov_cfg_default();
  out->rate_per_min       = gov.rate_per_min;
  out->burst              = gov.burst;
  out->backoff_base_ms    = gov.backoff_base_ms;
  out->backoff_max_ms     = gov.backoff_max_ms;
  out->max_inflight       = (uint32_t)gov.max_inflight;
}

/**
 * @brief Parse every descriptor record from an open stream.
 * @details Ignores blank/comment/section lines and rejects overlong, malformed,
 *          unknown, or invalid key/value records with a line diagnostic.
 * @param[in]     fp  Open descriptor stream.
 * @param[in,out] out Descriptor receiving parsed overrides.
 * @return Whether the complete stream parsed without read errors.
 * @retval true  Every active line was valid and the stream ended cleanly.
 * @retval false A line or stream error was observed.
 * @pre @p fp and @p out are non-NULL.
 * @pre @p fp is open for reading and positioned at the descriptor start.
 * @post On true, all active records have been applied to @p out.
 * @post On false, the caller rejects the partially populated descriptor.
 * @note Not thread-safe: reads a stream and mutates @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static bool config_parse_stream(FILE* fp, mdl_site_t* out)
{
  char   line[k_line_max];
  size_t line_no = 0U;
  while (fgets(line, (int)sizeof(line), fp) != nullptr) {
    ++line_no;
    if ((strchr(line, '\n') == nullptr) && (feof(fp) == 0)) {
      (void)fprintf(stderr,
                    "media_dl: config:%zu: line exceeds %u bytes\n",
                    line_no,
                    (unsigned)k_line_max);
      return false;
    }
    char* p = trim(line);
    if ((p[0] == '\0') || (p[0] == '#') || (p[0] == '[')) {
      continue; /* blank / comment / section header */
    }
    char* eq = strchr(p, '=');
    if (eq == nullptr) {
      (void)fprintf(stderr, "media_dl: config:%zu: expected key=value\n", line_no);
      return false;
    }
    *eq             = '\0';
    const char* key = trim(p);
    const char* val = trim(eq + 1);
    if ((key[0] == '\0') || !apply_kv(out, key, val)) {
      (void)
        fprintf(stderr, "media_dl: config:%zu: invalid or unknown key/value '%s'\n", line_no, key);
      return false;
    }
  }
  return ferror(fp) == 0;
}

/**
 * @brief Validate descriptor cross-field and selector invariants.
 * @details Checks selector grammar, required static metadata, delay ordering,
 *          governor bounds, and reading direction after parsing completes.
 * @param[in] site Fully populated site descriptor.
 * @return Whether every descriptor invariant is satisfied.
 * @retval true  The descriptor is safe for fetch/search use.
 * @retval false A required value, selector, or cross-field bound is invalid.
 * @pre @p site is non-NULL.
 * @pre Every fixed string field in @p site is NUL-terminated.
 * @post @p site is unchanged.
 * @post No filesystem or network state is accessed.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool config_valid(const mdl_site_t* site)
{
  const char* selectors[] = {site->series_title_selector,
                             site->series_summary_selector,
                             site->series_author_selector,
                             site->series_artist_selector,
                             site->series_cover_selector,
                             site->chapter_title_selector,
                             site->chapter_number_selector};
  for (size_t i = 0U; i < (sizeof(selectors) / sizeof(selectors[0])); ++i) {
    const char* selector = selectors[i];
    if (selector[0] == '\0') {
      continue;
    }
    const char* prefixes[] = {"meta:", "class:", "label:", "literal:"};
    bool        valid      = false;
    for (size_t prefix_index = 0U; prefix_index < (sizeof(prefixes) / sizeof(prefixes[0]));
         ++prefix_index) {
      const size_t prefix_len = strlen(prefixes[prefix_index]);
      if ((strncmp(selector, prefixes[prefix_index], prefix_len) == 0) &&
          (selector[prefix_len] != '\0')) {
        valid = true;
        break;
      }
    }
    if (!valid) {
      return false;
    }
  }
  return (site->host[0] != '\0') && (site->language[0] != '\0') &&
         ((strcmp(site->reading_direction, "ltr") == 0) ||
          (strcmp(site->reading_direction, "rtl") == 0)) &&
         (site->img_delay_min <= site->img_delay_max) &&
         (site->chapter_delay_min <= site->chapter_delay_max) && (site->burst > 0U) &&
         (site->max_inflight > 0U) && (site->backoff_base_ms <= site->backoff_max_ms);
}

ra8_err_t mdl_config_load(const char* path, mdl_site_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  config_set_defaults(out);

  FILE* fp = fopen(path, "r");
  if (fp == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot open config '%s'\n", path);
    return k_ra8_fail;
  }
  const bool parsed = config_parse_stream(fp, out);
  const bool closed = fclose(fp) == 0;

  if (!parsed || !closed || !config_valid(out)) {
    (void)fprintf(stderr, "media_dl: config is invalid or missing required fields\n");
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

mdl_gov_cfg_t mdl_config_gov_cfg(const mdl_site_t* site)
{
  mdl_gov_cfg_t cfg   = mdl_gov_cfg_default();
  cfg.rate_per_min    = site->rate_per_min;
  cfg.burst           = site->burst;
  cfg.backoff_base_ms = site->backoff_base_ms;
  cfg.backoff_max_ms  = site->backoff_max_ms;
  cfg.max_inflight    = (uint16_t)site->max_inflight;
  return cfg;
}

/** @brief Larger of two unsigned values. */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

void mdl_config_apply_polite(mdl_site_t* site)
{
  site->img_delay_min     = max_u32(site->img_delay_min, (uint32_t)k_polite_img_min_ms);
  site->img_delay_max     = max_u32(site->img_delay_max, (uint32_t)k_polite_img_max_ms);
  site->chapter_delay_min = max_u32(site->chapter_delay_min, (uint32_t)k_polite_chap_min_ms);
  site->chapter_delay_max = max_u32(site->chapter_delay_max, (uint32_t)k_polite_chap_max_ms);
}
