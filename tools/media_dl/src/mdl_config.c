/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_config.c
 * @brief Flat key=value site-descriptor parser (host stdio).
 */
#include "mdl_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Local parser limits. */
typedef enum : uint16_t {
  k_line_max = 512, /**< Max config line length. */
} mdl_parse_limits_t;

/** @brief Trim leading/trailing ASCII whitespace in place; return start. */
static char* trim(char* s)
{
  while ((*s == ' ') || (*s == '\t') || (*s == '\r') || (*s == '\n')) {
    ++s;
  }
  size_t n = strlen(s);
  while ((n > 0U) && ((s[n - 1U] == ' ') || (s[n - 1U] == '\t') ||
                      (s[n - 1U] == '\r') || (s[n - 1U] == '\n'))) {
    s[n - 1U] = '\0';
    --n;
  }
  return s;
}

/** @brief Copy `val` into a bounded field (truncation-safe). */
static void set_str(char* dst, size_t cap, const char* val)
{
  (void)snprintf(dst, cap, "%s", val);
}

/** @brief Apply one key=value pair to the descriptor. */
static void apply_kv(mdl_site_t* s, const char* key, const char* val)
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
    s->img_delay_min = (uint32_t)strtoul(val, nullptr, 10);
  } else if (strcmp(key, "img_delay_max") == 0) {
    s->img_delay_max = (uint32_t)strtoul(val, nullptr, 10);
  } else if (strcmp(key, "chapter_delay_min") == 0) {
    s->chapter_delay_min = (uint32_t)strtoul(val, nullptr, 10);
  } else if (strcmp(key, "chapter_delay_max") == 0) {
    s->chapter_delay_max = (uint32_t)strtoul(val, nullptr, 10);
  } else {
    (void)fprintf(stderr, "media_dl: config: unknown key '%s' (ignored)\n", key);
  }
}

ra8_err_t mdl_config_load(const char* path, mdl_site_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  /* Defaults (a polite baseline; the file overrides). */
  *out = (mdl_site_t){0};
  set_str(out->name, sizeof(out->name), "site");
  set_str(out->kind, sizeof(out->kind), "manhwa");
  set_str(out->chapter_url_contains, sizeof(out->chapter_url_contains),
          "chapter");
  set_str(out->page_img_attr, sizeof(out->page_img_attr), "data-src");
  out->chapter_order     = k_mdl_order_asc;
  out->img_delay_min     = 500U;
  out->img_delay_max     = 1200U;
  out->chapter_delay_min = 1500U;
  out->chapter_delay_max = 3000U;

  FILE* fp = fopen(path, "r");
  if (fp == nullptr) {
    (void)fprintf(stderr, "media_dl: cannot open config '%s'\n", path);
    return k_ra8_fail;
  }

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
    *eq = '\0';
    char* key = trim(p);
    char* val = trim(eq + 1);
    if (key[0] != '\0') {
      apply_kv(out, key, val);
    }
  }
  (void)fclose(fp);

  if (out->host[0] == '\0') {
    (void)fprintf(stderr, "media_dl: config missing required 'host'\n");
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}
