/**
 * @file mdl_robots.c
 * @brief Implementation of the robots.txt parser, matcher, and per-host cache.
 *
 * @details Parses a bounded robots document in two passes, selects the most
 * specific applicable user-agent group, and retains its rules in fixed caller
 * storage. Matching and cache lookup remain allocation-free and network access
 * is supplied through the public fetch callback seam.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_robots.h"

#include <math.h>
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

/**
 * @brief ASCII lower-case of one character (locale-independent).
 * @details Maps `A` through `Z`; all other values pass through.
 * @param[in] c Character to map.
 * @return Lower-case ASCII equivalent.
 * @retval other Mapped or unchanged character.
 * @pre @p c is representable as `char`.
 * @pre Locale-specific folding is not required.
 * @post No state is modified.
 * @post Non-uppercase input is unchanged.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static char lower_ascii(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Case-insensitive equality of two NUL-terminated strings.
 * @details Compares ASCII-folded bytes through both terminators.
 * @param[in] a First string.
 * @param[in] b Second string.
 * @return Whether the strings are equal ignoring ASCII case.
 * @retval true All folded bytes match.
 * @retval false A byte or length differs.
 * @pre @p a and @p b are non-NULL and NUL-terminated.
 * @pre ASCII-only folding is sufficient.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads only arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ieq(const char* a, const char* b)
{
  size_t i = 0U;
  while ((a[i] != '\0') && (lower_ascii(a[i]) == lower_ascii(b[i]))) {
    ++i;
  }
  return lower_ascii(a[i]) == lower_ascii(b[i]);
}

/**
 * @brief True if `prefix` (case-insensitive) is a prefix of `s`.
 * @details Compares the candidate prefix using ASCII folding.
 * @param[in] prefix Candidate prefix.
 * @param[in] s String to inspect.
 * @return Whether the folded prefix matches.
 * @retval true Every prefix byte matches.
 * @retval false A byte differs.
 * @pre Both strings are non-NULL and NUL-terminated.
 * @pre @p s is readable through the candidate prefix length.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads only arguments.
 * @since 0.1.0
 */
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

/**
 * @brief Read one line into `buf`; strip its `#` comment and trim whitespace.
 *
 * @details Consumes through the next newline or @p end. Bytes that exceed the
 * buffer are still consumed and set the caller's sticky truncation flag, while
 * the retained prefix is always NUL-terminated before comment processing.
 *
 * @param[in,out] pp Cursor updated to the start of the following line.
 * @param[in] end One-past-last byte of the robots document.
 * @param[out] buf Storage receiving the bounded line prefix.
 * @param[in] cap Capacity of @p buf in bytes.
 * @param[in,out] truncated Sticky flag set when a line does not fit.
 * @return Whether a line was available to consume.
 * @retval true  One line was consumed and @p buf was populated.
 * @retval false The cursor was already at or beyond @p end.
 * @pre @p pp, @p buf, and @p truncated are non-NULL and @p cap is non-zero.
 * @pre `*pp` and @p end delimit one readable contiguous byte range.
 * @post On true, `*pp` advances past the consumed line and optional newline.
 * @post On true, @p buf is NUL-terminated and contains no inline comment.
 * @note The function preserves an already-true @p truncated value.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
next_line(const char** pp, const char* end, char* buf, size_t cap, bool* truncated)
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
    } else {
      *truncated = true;
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

/**
 * @brief Split a trimmed line into `field`/`value` on the first `:`.
 * @details Replaces the delimiter with NUL and returns trimmed pointers into @p line.
 * @param[in,out] line Writable directive line.
 * @param[out] field Receives the field pointer.
 * @param[out] value Receives the value pointer.
 * @return Whether a non-empty field and delimiter were found.
 * @retval true Both outputs identify substrings in @p line.
 * @retval false No delimiter exists or the field is empty.
 * @pre All arguments are non-NULL.
 * @pre @p line is writable and NUL-terminated.
 * @post On true, @p line is split in place.
 * @post Without a delimiter, outputs are unchanged.
 * @note Returned pointers borrow @p line storage.
 * @since 0.1.0
 */
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

/**
 * @brief Specificity of a robots user-agent token vs ours (-1 = no match).
 * @details Wildcard scores zero; a specific ASCII-folded prefix scores its length.
 * @param[in] agent Robots user-agent value.
 * @param[in] ua_token This tool's product token.
 * @return Match specificity.
 * @retval -1 No match.
 * @retval other Zero for wildcard or a positive prefix length.
 * @pre Both arguments are non-NULL and NUL-terminated.
 * @pre Robots product-token prefix matching is intended.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads only arguments.
 * @since 0.1.0
 */
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

/**
 * @brief Pass 1: find the best specific match length and wildcard presence.
 *
 * @details Scans every `User-agent` field without retaining rules. The longest
 * case-insensitive prefix match becomes @p best; a `*` group is reported
 * independently through @p wild for use only when no specific group wins.
 *
 * @param[in] text Robots document bytes.
 * @param[in] len Number of readable bytes at @p text.
 * @param[in] ua_token Product token to match against user-agent fields.
 * @param[out] best Longest specific match, or the no-match sentinel.
 * @param[out] wild Whether a wildcard user-agent group was present.
 * @return Whether every input line fit the bounded parser buffer.
 * @retval true  The complete document was scanned without truncation.
 * @retval false At least one line exceeded the parser buffer.
 * @pre @p text and @p ua_token are non-NULL; @p text is readable for @p len bytes.
 * @pre @p best and @p wild point to writable result storage.
 * @post @p best and @p wild describe all complete and truncated line prefixes scanned.
 * @post The input document and user-agent token are unchanged.
 * @note A false result makes the enclosing parse invalid rather than silently permissive.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
scan_spec(const char* text, size_t len, const char* ua_token, int* best, bool* wild)
{
  *best           = (int)k_spec_none;
  *wild           = false;
  const char* p   = text;
  const char* end = text + len;
  char        line[k_robots_line_max];
  bool        truncated = false;
  while (next_line(&p, end, line, sizeof(line), &truncated)) {
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
  return !truncated;
}

/**
 * @brief True if a group of this specificity is the selected group.
 * @details Selects wildcard membership only for wildcard fallback, else exact specificity.
 * @param[in] target Selected specific score.
 * @param[in] wildcard_target Whether wildcard fallback was selected.
 * @param[in] grp_spec Current group's score.
 * @param[in] grp_wild Whether the current group contains wildcard.
 * @return Whether the group contributes directives.
 * @retval true The group matches the selected target.
 * @retval false The group is not selected.
 * @pre Scores use the parser's sentinel convention.
 * @pre Flags describe their corresponding groups.
 * @post No state is modified.
 * @post Inputs are unchanged.
 * @note Thread-safe: pure comparison.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
group_selected(int target, bool wildcard_target, int grp_spec, bool grp_wild)
{
  return wildcard_target ? grp_wild : ((target >= 0) && (grp_spec == target));
}

/**
 * @brief Append an Allow/Disallow rule (non-empty patterns only).
 * @details Empty patterns are no-ops; capacity or length overflow invalidates the parse.
 * @param[in,out] out Parsed rule set.
 * @param[in] kind Allow or disallow classification.
 * @param[in] value NUL-terminated rule pattern.
 * @return Nothing.
 * @pre @p out and @p value are non-NULL.
 * @pre @p out owns writable fixed rule storage.
 * @post A fitting non-empty rule increments the count once.
 * @post Overflow clears `valid` without writing past bounds.
 * @note Not thread-safe: mutates @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static void add_rule(mdl_robots_t* out, mdl_robots_rule_kind_t kind, const char* value)
{
  if ((value[0] == '\0') || (out->count >= (size_t)k_mdl_robots_max_rules)) {
    if (value[0] != '\0') {
      out->valid = false;
    }
    return; /* empty pattern imposes no restriction; full table stops growing */
  }
  if (strnlen(value, k_mdl_robots_path_max) >= k_mdl_robots_path_max) {
    out->valid = false;
    return;
  }
  mdl_robots_rule_t* r = &out->rules[out->count];
  r->kind              = kind;
  (void)snprintf(r->path, sizeof(r->path), "%s", value);
  r->len = (uint16_t)strlen(r->path);
  out->count++;
}

/**
 * @brief Record the strictest Crawl-delay seen (clamped to the ceiling).
 * @details Parses a finite non-negative decimal and retains the largest bounded delay.
 * @param[in,out] out Parsed robots result.
 * @param[in] value NUL-terminated crawl-delay value.
 * @return Nothing.
 * @pre @p out and @p value are non-NULL.
 * @pre @p out contains writable result storage.
 * @post Valid input can only maintain or increase `crawl_delay_ms`.
 * @post Invalid text leaves the delay unchanged.
 * @note Not thread-safe: may mutate @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static void set_crawl(mdl_robots_t* out, const char* value)
{
  char*        end  = nullptr;
  const double secs = strtod(value, &end);
  if ((end == value) || (*end != '\0') || !isfinite(secs) || (secs <= 0.0)) {
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

/**
 * @brief Apply one non-user-agent directive line to the selected group.
 * @details Dispatches Allow, Disallow, and Crawl-delay; unknown fields are ignored.
 * @param[in,out] out Parsed robots result.
 * @param[in] field Normalised directive name.
 * @param[in] value Trimmed directive value.
 * @return Nothing.
 * @pre All arguments are non-NULL and NUL-terminated.
 * @pre @p out owns writable fixed storage.
 * @post Recognised directives update only their policy field.
 * @post Unknown directives leave @p out unchanged.
 * @note Not thread-safe: may mutate @p out.
 * @since 0.1.0
 */
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

/**
 * @brief Pass 2: collect rules/crawl-delay from the selected group(s).
 * @details Rescans the document and applies directives only while a selected group is active.
 * @param[in] text Robots document bytes.
 * @param[in] len Readable document length.
 * @param[in] ua_token Product token used for specificity.
 * @param[in] target Selected specific score.
 * @param[in] wildcard_target Whether wildcard fallback was selected.
 * @param[in,out] out Result receiving directives.
 * @return Nothing.
 * @pre Pointer arguments are non-NULL and @p text is readable for @p len bytes.
 * @pre Selection inputs came from the first pass.
 * @post Only selected-group directives contribute to @p out.
 * @post Any truncated line invalidates @p out.
 * @note Not thread-safe: mutates @p out.
 * @since 0.1.0
 */
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
  bool        truncated = false;
  while (next_line(&p, end, line, sizeof(line), &truncated)) {
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
  if (truncated) {
    out->valid = false;
  }
}

void mdl_robots_parse(const char* text, size_t len, const char* ua_token, mdl_robots_t* out)
{
  if (out == nullptr) {
    return;
  }
  *out       = (mdl_robots_t){};
  out->valid = true;
  if ((text == nullptr) && (len != 0U)) {
    out->valid = false;
    return;
  }
  if (ua_token == nullptr) {
    out->valid = false;
    return;
  }
  if (len == 0U) {
    return;
  }
  int  best = (int)k_spec_none;
  bool wild = false;
  if (!scan_spec(text, len, ua_token, &best, &wild)) {
    out->valid = false;
    return;
  }
  if ((best < 0) && !wild) {
    return; /* no group matches us -> no restrictions */
  }
  const bool wildcard_target = (best < 0) && wild;
  harvest(text, len, ua_token, best, wildcard_target, out);
}

/**
 * @brief Match `pat` (len `patlen`, `*` glob) against a prefix of `path`.
 * @details Uses bounded wildcard backtracking and optionally requires complete path consumption.
 * @param[in] pat Pattern bytes.
 * @param[in] patlen Pattern length without a terminal anchor.
 * @param[in] path NUL-terminated URL path.
 * @param[in] anchored Whether the match must consume the complete path.
 * @return Whether the pattern matches.
 * @retval true The required prefix or full path matched.
 * @retval false No wildcard expansion matched.
 * @pre @p pat and @p path are non-NULL and readable for their lengths.
 * @pre @p patlen excludes a terminal `$` marker.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Runtime is bounded by fixed rule and path limits.
 * @since 0.1.0
 */
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

/**
 * @brief Match a rule pattern (honouring a trailing `$`) against `path`.
 * @details Removes a terminal anchor from the glob length and delegates matching.
 * @param[in] pat Rule pattern bytes.
 * @param[in] len Stored pattern length.
 * @param[in] path NUL-terminated URL path.
 * @return Whether the rule matches @p path.
 * @retval true The rule matches.
 * @retval false The rule does not match.
 * @pre @p pat and @p path are non-NULL.
 * @pre @p pat is readable for @p len bytes.
 * @post Inputs are unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads only arguments.
 * @since 0.1.0
 */
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
  if ((robots == nullptr) || (path == nullptr) || !robots->valid) {
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

/** @brief Locate an existing cache entry for one origin, or NULL if absent. */
RA8_INTERNAL static mdl_robots_cache_entry_t*
cache_find(mdl_robots_cache_t* cache, const char* scheme, const char* host)
{
  for (size_t i = 0U; i < (size_t)k_mdl_robots_max_hosts; ++i) {
    if (cache->hosts[i].used && (strcmp(cache->hosts[i].scheme, scheme) == 0) &&
        (strcmp(cache->hosts[i].host, host) == 0)) {
      return &cache->hosts[i];
    }
  }
  return nullptr;
}

/** @brief Pick a free cache slot, or the dedicated uncached overflow slot. */
RA8_INTERNAL static mdl_robots_cache_entry_t* cache_slot(mdl_robots_cache_t* cache)
{
  for (size_t i = 0U; i < (size_t)k_mdl_robots_max_hosts; ++i) {
    if (!cache->hosts[i].used) {
      return &cache->hosts[i];
    }
  }
  return &cache->overflow;
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
  if ((cache == nullptr) || (scheme == nullptr) || (host == nullptr) || (ua_token == nullptr) ||
      (fetch == nullptr) || (scratch == nullptr) || (scratch_cap == 0U) ||
      ((strcmp(scheme, "http") != 0) && (strcmp(scheme, "https") != 0)) ||
      (strnlen(host, k_mdl_robots_host_max) >= k_mdl_robots_host_max)) {
    return nullptr;
  }
  mdl_robots_cache_entry_t* e = cache_find(cache, scheme, host);
  if (e == nullptr) {
    e              = cache_slot(cache);
    *e             = (mdl_robots_cache_entry_t){};
    e->rules.valid = true;
    (void)snprintf(e->scheme, sizeof(e->scheme), "%s", scheme);
    (void)snprintf(e->host, sizeof(e->host), "%s", host);
    char      url[k_robots_url_max];
    const int url_len = snprintf(url, sizeof(url), "%s://%s/robots.txt", scheme, host);
    if ((url_len < 0) || ((size_t)url_len >= sizeof(url))) {
      e->disallow_all = true;
      e->used         = true;
      return nullptr;
    }
    size_t                          got = 0U;
    const mdl_robots_fetch_result_t rc  = fetch(ctx, url, scratch, scratch_cap, &got);
    if (rc == k_mdl_robots_fetch_denied) {
      e->disallow_all = true;
    } else if (rc == k_mdl_robots_fetch_ok) {
      mdl_robots_parse(scratch, got, ua_token, &e->rules);
      if (!e->rules.valid) {
        e->disallow_all = true;
      }
    }
    e->used = true;
  }
  return e->disallow_all ? nullptr : &e->rules;
}
