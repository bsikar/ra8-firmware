/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_config.c
 * @brief Flat key=value site-descriptor parser (host stdio).
 */
#include "ra8_attributes.h"
#include "mdl_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/** @brief Apply one key=value pair to the descriptor. */
RA8_INTERNAL static void apply_kv(mdl_site_t* s, const char* key, const char* val)
{
  if (strcmp(key, "name") == 0) {
    set_str(s->name, sizeof(s->name), val);
  } else if (strcmp(key, "host") == 0) {
    set_str(s->host, sizeof(s->host), val);
  } else if (strcmp(key, "kind") == 0) {
    set_str(s->kind, sizeof(s->kind), val);
  } else if (strcmp(key, "chapter_url_contains") == 0) {
    set_str(s->chapter_url_contains, sizeof(s->chapter_url_contains), val);
  } else if (strcmp(key, "chapter_order") == 0) {
    if (strcmp(val, "reverse") == 0) {
      s->chapter_order = k_mdl_order_reverse;
    } else if (strcmp(val, "asc") == 0) {
      s->chapter_order = k_mdl_order_asc;
    } else {
      s->chapter_order = k_mdl_order_doc;
    }
  } else if (strcmp(key, "page_img_attr") == 0) {
    set_str(s->page_img_attr, sizeof(s->page_img_attr), val);
  } else if (strcmp(key, "page_img_url_contains") == 0) {
    set_str(s->page_img_url_contains, sizeof(s->page_img_url_contains), val);
  } else if (strcmp(key, "img_delay_min") == 0) {
    s->img_delay_min = (uint32_t)strtoul(val, nullptr, k_dec_base);
  } else if (strcmp(key, "img_delay_max") == 0) {
    s->img_delay_max = (uint32_t)strtoul(val, nullptr, k_dec_base);
  } else if (strcmp(key, "chapter_delay_min") == 0) {
    s->chapter_delay_min = (uint32_t)strtoul(val, nullptr, k_dec_base);
  } else if (strcmp(key, "chapter_delay_max") == 0) {
    s->chapter_delay_max = (uint32_t)strtoul(val, nullptr, k_dec_base);
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
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  config_set_defaults(out);

  FILE* fp = fopen(path, "r");
  if (fp == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot open config '%s'\n", path);
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
