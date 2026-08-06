/**
 * @file mdl_config.c
 * @brief Flat key=value site-descriptor parser (host stdio).
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#include "mdl_config.h"

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

/** @brief Copy `val` into a bounded field (truncation-safe). */
RA8_INTERNAL static void set_str(char* dst, size_t cap, const char* val)
{
  (void)snprintf(dst, cap, "%s", val);
}

/** @brief Apply a string-valued key, or return false if `key` is not one. */
RA8_INTERNAL static bool apply_kv_str(mdl_site_t* s, const char* key, const char* val)
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
    {"page_img_attr", s->page_img_attr, sizeof(s->page_img_attr)},
    {"page_img_url_contains", s->page_img_url_contains, sizeof(s->page_img_url_contains)},
    {"search_url", s->search_url, sizeof(s->search_url)},
    {"search_result_contains", s->search_result_contains, sizeof(s->search_result_contains)},
    {"browse_url", s->browse_url, sizeof(s->browse_url)},
  };
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    if (strcmp(key, fields[i].key) == 0) {
      set_str(fields[i].dst, fields[i].cap, val);
      return true;
    }
  }
  return false;
}

/** @brief Apply an unsigned-integer key (jitter delay or governor bound). */
RA8_INTERNAL static bool apply_kv_u32(mdl_site_t* s, const char* key, const char* val)
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
      *fields[i].dst = (uint32_t)strtoul(val, nullptr, k_dec_base);
      return true;
    }
  }
  return false;
}

/** @brief Map a `chapter_order` value string to its enum. */
RA8_INTERNAL static mdl_chapter_order_t parse_order(const char* val)
{
  if (strcmp(val, "reverse") == 0) {
    return k_mdl_order_reverse;
  }
  if (strcmp(val, "asc") == 0) {
    return k_mdl_order_asc;
  }
  return k_mdl_order_doc;
}

/** @brief Apply one key=value pair to the descriptor. */
RA8_INTERNAL static void apply_kv(mdl_site_t* s, const char* key, const char* val)
{
  if (apply_kv_str(s, key, val) || apply_kv_u32(s, key, val)) {
    return;
  }
  if (strcmp(key, "chapter_order") == 0) {
    s->chapter_order = parse_order(val);
  } else {
    (void)fprintf(stderr, "media_dl: config: unknown key '%s' (ignored)\n", key);
  }
}

/** @brief Apply the polite default descriptor before the file overrides it. */
RA8_INTERNAL static void config_set_defaults(mdl_site_t* out)
{
  *out = (mdl_site_t){};
  set_str(out->name, sizeof(out->name), "site");
  set_str(out->kind, sizeof(out->kind), "manhwa");
  set_str(out->chapter_url_contains, sizeof(out->chapter_url_contains), "chapter");
  set_str(out->page_img_attr, sizeof(out->page_img_attr), "data-src");
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

/** @brief Apply every key=value line of an open config stream to `out`. */
RA8_INTERNAL static void config_parse_stream(FILE* fp, mdl_site_t* out)
{
  char line[k_line_max];
  while (fgets(line, (int)sizeof(line), fp) != nullptr) {
    char* p = trim(line);
    if ((p[0] == '\0') || (p[0] == '#') || (p[0] == '[')) {
      continue; /* blank / comment / section header */
    }
    char* eq = strchr(p, '=');
    if (eq == nullptr) {
      continue; /* not a key=value line */
    }
    *eq             = '\0';
    const char* key = trim(p);
    const char* val = trim(eq + 1);
    if (key[0] != '\0') {
      apply_kv(out, key, val);
    }
  }
}

ra8_err_t mdl_config_load(const char* path, mdl_site_t* out)
{
  if ((path == NULL) || (out == NULL)) {
    return k_ra8_err_invalid_arg;
  }
  config_set_defaults(out);

  FILE* fp = fopen(path, "r");
  if (fp == NULL) {
    (void)fprintf(stderr, "media_dl: cannot open config '%s'\n", path);
    // cppcheck-suppress resourceLeak
    // fp is NULL here
    return k_ra8_fail;
  }
  config_parse_stream(fp, out);
  (void)fclose(fp);

  if (out->host[0] == '\0') {
    (void)fprintf(stderr, "media_dl: config missing required 'host'\n");
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
