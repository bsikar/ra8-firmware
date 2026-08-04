/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_robots.c
 * @brief Implementation of the robots.txt parser, matcher, and per-host cache.
 */
#include "mdl_robots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Local parse/format sizes. */
typedef enum : uint16_t {
  k_robots_line_max = 512, /**< Max robots.txt line length.   */
  k_robots_url_max  = 256, /**< Max `/robots.txt` URL length. */
} mdl_robots_local_size_t;

/** @brief Millisecond conversions and the crawl-delay ceiling. */
typedef enum : uint32_t {
  k_ms_per_s     = 1000U,  /**< Milliseconds per second.            */
  k_crawl_cap_ms = 60000U, /**< Clamp a Crawl-delay to at most 60s. */
} mdl_robots_ms_t;

/** @brief "No user-agent match" sentinel for the specificity score. */
typedef enum : int8_t {
  k_spec_none = -1, /**< No group matched our user-agent. */
} mdl_robots_spec_t;

/** @brief ASCII lower-case of one character (locale-independent). */
RA8_INTERNAL static char lower_ascii(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/** @brief Case-insensitive equality of two NUL-terminated strings. */
RA8_INTERNAL static bool ieq(const char* a, const char* b)
{
  size_t i = 0U;
  while ((a[i] != '\0') && (lower_ascii(a[i]) == lower_ascii(b[i]))) {
    ++i;
  }
  return lower_ascii(a[i]) == lower_ascii(b[i]);
}

/** @brief True if `prefix` (case-insensitive) is a prefix of `s`. */
RA8_INTERNAL static bool ci_prefix(const char* prefix, const char* s)
{
  size_t i = 0U;
  while (prefix[i] != '\0') {
    if (lower_ascii(s[i]) != lower_ascii(prefix[i])) {
      return false;
    }
    ++i;
  }
  return true;
}

/** @brief Trim leading/trailing ASCII whitespace in place; return start. */
RA8_INTERNAL static char* trim_ws(char* s)
{
  while ((*s == ' ') || (*s == '\t') || (*s == '\r')) {
    ++s;
  }
  size_t n = strlen(s);
  while ((n > 0U) && ((s[n - 1U] == ' ') || (s[n - 1U] == '\t') || (s[n - 1U] == '\r'))) {
    s[n - 1U] = '\0';
    --n;
  }
  return s;
}

/** @brief Read one line into `buf`; strip its `#` comment and trim whitespace. */
RA8_INTERNAL static bool next_line(const char** pp, const char* end, char* buf, size_t cap)
{
  const char* p = *pp;
  if (p >= end) {
    return false;
  }
  size_t n = 0U;
  while ((p < end) && (*p != '\n')) {
    if ((n + 1U) < cap) {
      buf[n] = *p;
      ++n;
    }
    ++p;
  }
  if (p < end) {
    ++p; /* consume the '\n' */
  }
  *pp        = p;
  buf[n]     = '\0';
  char* hash = strchr(buf, '#');
  if (hash != nullptr) {
    *hash = '\0';
  }
  return true;
}

/** @brief Split a trimmed line into `field`/`value` on the first `:`. */
RA8_INTERNAL static bool split_field(char* line, char** field, char** value)
{
  char* colon = strchr(line, ':');
  if (colon == nullptr) {
    return false;
  }
  *colon = '\0';
  *field = trim_ws(line);
  *value = trim_ws(colon + 1);
  return (*field)[0] != '\0';
}

/** @brief Specificity of a robots user-agent token vs ours (-1 = no match). */
RA8_INTERNAL static int agent_spec(const char* agent, const char* ua_token)
{
  if (strcmp(agent, "*") == 0) {
    return 0;
  }
  if ((agent[0] != '\0') && ci_prefix(agent, ua_token)) {
    return (int)strlen(agent);
  }
  return (int)k_spec_none;
}

/** @brief Pass 1: find the best specific match length and wildcard presence. */
RA8_INTERNAL static void
scan_spec(const char* text, size_t len, const char* ua_token, int* best, bool* wild)
{
  *best           = (int)k_spec_none;
  *wild           = false;
  const char* p   = text;
  const char* end = text + len;
  char        line[k_robots_line_max];
  while (next_line(&p, end, line, sizeof(line))) {
    char* field = nullptr;
    char* value = nullptr;
    if (!split_field(line, &field, &value)) {
      continue;
    }
    if (ieq(field, "user-agent")) {
      const int spec = agent_spec(value, ua_token);
      if (spec == 0) {
        *wild = true;
      } else if (spec > *best) {
        *best = spec;
      }
    }
  }
}

/** @brief True if a group of this specificity is the selected group. */
RA8_INTERNAL static bool
group_selected(int target, bool wildcard_target, int grp_spec, bool grp_wild)
{
  return wildcard_target ? grp_wild : ((target >= 0) && (grp_spec == target));
}

/** @brief Append an Allow/Disallow rule (non-empty patterns only). */
RA8_INTERNAL static void add_rule(mdl_robots_t* out, mdl_robots_rule_kind_t kind, const char* value)
{
  if ((value[0] == '\0') || (out->count >= (size_t)k_mdl_robots_max_rules)) {
    return; /* empty pattern imposes no restriction; full table stops growing */
  }
  mdl_robots_rule_t* r = &out->rules[out->count];
  r->kind              = kind;
  (void)snprintf(r->path, sizeof(r->path), "%s", value);
  r->len = (uint16_t)strlen(r->path);
  out->count++;
}

/** @brief Record the strictest Crawl-delay seen (clamped to the ceiling). */
RA8_INTERNAL static void set_crawl(mdl_robots_t* out, const char* value)
{
  const double secs = strtod(value, nullptr);
  if (secs <= 0.0) {
    return;
  }
  uint32_t ms = (secs >= ((double)k_crawl_cap_ms / (double)k_ms_per_s))
                  ? (uint32_t)k_crawl_cap_ms
                  : (uint32_t)(secs * (double)k_ms_per_s);
  if (!out->have_crawl_delay || (ms > out->crawl_delay_ms)) {
    out->crawl_delay_ms = ms;
  }
  out->have_crawl_delay = true;
}

/** @brief Apply one non-user-agent directive line to the selected group. */
RA8_INTERNAL static void apply_directive(mdl_robots_t* out, const char* field, const char* value)
{
  if (ieq(field, "disallow")) {
    add_rule(out, k_mdl_rule_disallow, value);
  } else if (ieq(field, "allow")) {
    add_rule(out, k_mdl_rule_allow, value);
  } else if (ieq(field, "crawl-delay")) {
    set_crawl(out, value);
  }
}

/** @brief Pass 2: collect rules/crawl-delay from the selected group(s). */
RA8_INTERNAL static void harvest(const char*   text,
                                 size_t        len,
                                 const char*   ua_token,
                                 int           target,
                                 bool          wildcard_target,
                                 mdl_robots_t* out)
{
  const char* p          = text;
  const char* end        = text + len;
  bool        prev_agent = false;
  bool        selected   = false;
  int         grp_spec   = (int)k_spec_none;
  bool        grp_wild   = false;
  char        line[k_robots_line_max];
  while (next_line(&p, end, line, sizeof(line))) {
    char* field = nullptr;
    char* value = nullptr;
    if (!split_field(line, &field, &value)) {
      continue;
    }
    if (ieq(field, "user-agent")) {
      if (!prev_agent) {
        grp_spec = (int)k_spec_none;
        grp_wild = false;
      }
      const int spec = agent_spec(value, ua_token);
      if (spec == 0) {
        grp_wild = true;
      } else if (spec > grp_spec) {
        grp_spec = spec;
      }
      selected   = group_selected(target, wildcard_target, grp_spec, grp_wild);
      prev_agent = true;
    } else {
      prev_agent = false;
      if (selected) {
        apply_directive(out, field, value);
      }
    }
  }
}

void mdl_robots_parse(const char* text, size_t len, const char* ua_token, mdl_robots_t* out)
{
  if (out == nullptr) {
    return;
  }
  *out = (mdl_robots_t){};
  if ((text == nullptr) || (len == 0U) || (ua_token == nullptr)) {
    return;
  }
  int  best = (int)k_spec_none;
  bool wild = false;
  scan_spec(text, len, ua_token, &best, &wild);
  if ((best < 0) && !wild) {
    return; /* no group matches us -> no restrictions */
  }
  const bool wildcard_target = (best < 0) && wild;
  harvest(text, len, ua_token, best, wildcard_target, out);
}

/** @brief Match `pat` (len `patlen`, `*` glob) against a prefix of `path`. */
RA8_INTERNAL static bool
glob_prefix(const char* pat, size_t patlen, const char* path, bool anchored)
{
  size_t      pi   = 0U;
  const char* s    = path;
  size_t      star = patlen + 1U; /* > patlen means "no star seen yet" */
  const char* ss   = nullptr;
  while (*s != '\0') {
    if ((pi < patlen) && (pat[pi] == '*')) {
      star = pi;
      ++pi;
      ss = s;
    } else if ((pi < patlen) && (pat[pi] == *s)) {
      ++pi;
      ++s;
    } else if ((star <= patlen) && (ss != nullptr)) {
      /* star <= patlen holds only after `ss = s` ran, so ss is non-NULL here;
       * the explicit check makes that invariant visible to the analyser. */
      pi = star + 1U;
      ++ss;
      s = ss;
    } else {
      return false;
    }
    if ((pi == patlen) && !anchored) {
      return true; /* whole pattern consumed -> prefix match */
    }
  }
  while ((pi < patlen) && (pat[pi] == '*')) {
    ++pi;
  }
  return pi == patlen;
}

/** @brief Match a rule pattern (honouring a trailing `$`) against `path`. */
RA8_INTERNAL static bool robots_match(const char* pat, size_t len, const char* path)
{
  bool anchored = false;
  if ((len > 0U) && (pat[len - 1U] == '$')) {
    anchored = true;
    --len;
  }
  return glob_prefix(pat, len, path, anchored);
}

/** @brief Longest matching rule for `path` (Allow wins ties), or NULL. */
RA8_INTERNAL static const mdl_robots_rule_t* best_matching_rule(const mdl_robots_t* robots,
                                                                const char*         path)
{
  const mdl_robots_rule_t* best     = nullptr;
  int                      best_len = (int)k_spec_none;
  for (size_t i = 0U; i < robots->count; ++i) {
    const mdl_robots_rule_t* r = &robots->rules[i];
    if (!robots_match(r->path, r->len, path)) {
      continue;
    }
    const bool is_allow = (r->kind == k_mdl_rule_allow);
    if (((int)r->len > best_len) || (((int)r->len == best_len) && is_allow)) {
      best_len = (int)r->len;
      best     = r;
    }
  }
  return best;
}

bool mdl_robots_allows(const mdl_robots_t* robots, const char* path)
{
  if ((robots == nullptr) || (path == nullptr)) {
    return false;
  }
  const mdl_robots_rule_t* best = best_matching_rule(robots, path);
  return (best == nullptr) || (best->kind == k_mdl_rule_allow);
}

const char* mdl_robots_disallow_reason(const mdl_robots_t* robots, const char* path)
{
  if ((robots == nullptr) || (path == nullptr)) {
    return nullptr;
  }
  const mdl_robots_rule_t* best = best_matching_rule(robots, path);
  return ((best != nullptr) && (best->kind == k_mdl_rule_disallow)) ? best->path : nullptr;
}

/** @brief Locate an existing cache entry for `host`, or NULL if absent. */
RA8_INTERNAL static mdl_robots_cache_entry_t* cache_find(mdl_robots_cache_t* cache,
                                                         const char*         host)
{
  for (size_t i = 0U; i < (size_t)k_mdl_robots_max_hosts; ++i) {
    if (cache->hosts[i].used && (strcmp(cache->hosts[i].host, host) == 0)) {
      return &cache->hosts[i];
    }
  }
  return nullptr;
}

/** @brief Pick a free cache slot, or slot 0 when the cache is full. */
RA8_INTERNAL static mdl_robots_cache_entry_t* cache_slot(mdl_robots_cache_t* cache)
{
  for (size_t i = 0U; i < (size_t)k_mdl_robots_max_hosts; ++i) {
    if (!cache->hosts[i].used) {
      return &cache->hosts[i];
    }
  }
  return &cache->hosts[0];
}

const mdl_robots_t* mdl_robots_cache_consult(mdl_robots_cache_t* cache,
                                             const char*         scheme,
                                             const char*         host,
                                             const char*         ua_token,
                                             mdl_robots_fetch_fn fetch,
                                             void*               ctx,
                                             char*               scratch,
                                             size_t              scratch_cap)
{
  mdl_robots_cache_entry_t* e = cache_find(cache, host);
  if (e == nullptr) {
    e  = cache_slot(cache);
    *e = (mdl_robots_cache_entry_t){};
    (void)snprintf(e->host, sizeof(e->host), "%s", host);
    char url[k_robots_url_max];
    (void)snprintf(url, sizeof(url), "%s://%s/robots.txt", scheme, host);
    size_t                          got = 0U;
    const mdl_robots_fetch_result_t rc  = fetch(ctx, url, scratch, scratch_cap, &got);
    if (rc == k_mdl_robots_fetch_denied) {
      e->disallow_all = true;
    } else if (rc == k_mdl_robots_fetch_ok) {
      mdl_robots_parse(scratch, got, ua_token, &e->rules);
    }
    e->used = true;
  }
  return e->disallow_all ? nullptr : &e->rules;
}
