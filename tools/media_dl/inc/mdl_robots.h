/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_robots.h
 * @brief robots.txt parser, path matcher, and per-host cache.
 *
 * @details
 * Honouring robots.txt is, for this tool, purely about not getting banned: a
 * site's `Crawl-delay` is free authoritative guidance on the rate that will not
 * draw a block, and a `Disallow` is a request this tool should respect rather
 * than trip a bot trap. The parser selects the most specific matching
 * `User-agent` group, applies longest-match `Allow`/`Disallow` (with `Allow`
 * winning ties), and extracts `Crawl-delay`. Everything is fixed-size and
 * allocation-free so the same model can port to the device later.
 *
 * The parser and matcher are pure and unit-tested directly. The per-host cache
 * takes a fetch callback (dependency injection) so the network stays out of
 * this translation unit and a fake fetcher drives the cache in tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** @brief Fixed capacities for one parsed robots.txt group. */
typedef enum : uint16_t {
  k_mdl_robots_max_rules = 64,  /**< Max Allow/Disallow rules retained.   */
  k_mdl_robots_path_max  = 256, /**< Max bytes per rule path pattern.     */
} mdl_robots_limits_t;

/** @brief Fixed capacities for the per-host robots cache. */
typedef enum : uint16_t {
  k_mdl_robots_max_hosts = 16,  /**< Distinct hosts cached per run.       */
  k_mdl_robots_host_max  = 128, /**< Max host string bytes.               */
} mdl_robots_cache_limits_t;

/** @brief Whether a rule permits or forbids a matching path. */
typedef enum : uint8_t {
  k_mdl_rule_disallow = 0, /**< A `Disallow` rule.  */
  k_mdl_rule_allow    = 1, /**< An `Allow` rule.    */
} mdl_robots_rule_kind_t;

/** @brief Outcome class of a robots.txt fetch, per RFC 9309 convention. */
typedef enum : uint8_t {
  k_mdl_robots_fetch_ok     = 0, /**< A body was retrieved to parse.        */
  k_mdl_robots_fetch_absent = 1, /**< Absent / transport error: allow all.  */
  k_mdl_robots_fetch_denied = 2, /**< 5xx status: disallow all.             */
} mdl_robots_fetch_result_t;

/**
 * @struct mdl_robots_rule_t
 * @brief One Allow/Disallow rule from the selected group.
 * @details Patterns support `*` (any run) and a trailing `$` (end anchor).
 * @invariant `len` equals `strlen(path)` and is < ::k_mdl_robots_path_max.
 * @see mdl_robots_allows()
 * @since 0.1.0
 */
typedef struct {
  mdl_robots_rule_kind_t kind;                        /**< Allow or disallow.        */
  uint16_t               len;                         /**< Pattern length in bytes.  */
  char                   path[k_mdl_robots_path_max]; /**< Path pattern.           */
} mdl_robots_rule_t;

/**
 * @struct mdl_robots_t
 * @brief Rules and crawl-delay for the group matching our user-agent.
 * @details The result of parsing one robots.txt for one user-agent token; an
 *          all-zero value (no rules) permits every path.
 * @invariant `count <= ::k_mdl_robots_max_rules`.
 * @see mdl_robots_parse()
 * @since 0.1.0
 */
typedef struct {
  mdl_robots_rule_t rules[k_mdl_robots_max_rules]; /**< Selected group's rules.   */
  size_t            count;                         /**< Number of valid rules.    */
  bool              have_crawl_delay;              /**< A Crawl-delay was seen.   */
  uint32_t          crawl_delay_ms;                /**< Crawl-delay in ms.        */
} mdl_robots_t;

/**
 * @brief Parse robots.txt text for the group matching `ua_token`.
 *
 * @details
 * Selects the most specific matching `User-agent` group (a case-insensitive
 * `User-agent` value that is a prefix of `ua_token`; `*` is the fallback),
 * merging groups that match at the same specificity, and collects that group's
 * `Allow`/`Disallow` rules and the strictest `Crawl-delay`. Empty `Disallow`
 * values (which impose no restriction) are skipped. Malformed lines are
 * ignored. An input with no matching group leaves `out` empty (allow all).
 *
 * @param[in]  text     robots.txt bytes (need not be NUL-terminated).
 * @param[in]  len      Length of `text` in bytes.
 * @param[in]  ua_token Our user-agent product token (e.g. `"media_dl"`).
 * @param[out] out      Result; fully overwritten (zeroed first).
 *
 * @return Nothing.
 *
 * @pre `ua_token` and `out` are non-NULL; `text` is non-NULL when `len > 0`.
 * @pre `out` points to writable storage of `sizeof(mdl_robots_t)`.
 * @post `out->count <= ::k_mdl_robots_max_rules`.
 * @post `out` is fully initialised even when no group matches.
 *
 * @note Thread-safe: writes only `out`.
 * @since 0.1.0
 */
void mdl_robots_parse(const char* text, size_t len, const char* ua_token, mdl_robots_t* out);

/**
 * @brief Decide whether `path` is allowed by a parsed robots group.
 *
 * @details
 * Applies the longest-match rule; when an `Allow` and a `Disallow` match at the
 * same pattern length, the `Allow` wins (RFC 9309). A group with no rules
 * permits everything. Patterns honour `*` and a trailing `$`.
 *
 * @param[in] robots Parsed rules from ::mdl_robots_parse.
 * @param[in] path   URL path to test (with leading `/`).
 *
 * @return Whether a request for `path` is permitted.
 * @retval true  No rule matches, or the winning rule is an `Allow`.
 * @retval false The winning (longest) matching rule is a `Disallow`.
 *
 * @pre `robots` and `path` are non-NULL.
 * @pre `path` begins with `/` (a URL path, not a full URL).
 * @post Neither argument is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
bool mdl_robots_allows(const mdl_robots_t* robots, const char* path);

/**
 * @brief Return the `Disallow` pattern that forbids `path`, or NULL if allowed.
 *
 * @details
 * Names the exact rule responsible for a refusal so the caller can log a clear,
 * actionable message rather than a bare "disallowed". Uses the same
 * longest-match, Allow-wins-ties resolution as ::mdl_robots_allows.
 *
 * @param[in] robots Parsed rules from ::mdl_robots_parse.
 * @param[in] path   URL path to test (with leading `/`).
 *
 * @return The winning `Disallow` pattern, or NULL when `path` is allowed.
 * @retval non-NULL Borrowed pointer to the blocking rule's pattern.
 * @retval NULL     No rule blocks `path` (the winning rule allows, or none).
 *
 * @pre `robots` and `path` are non-NULL.
 * @pre `path` begins with `/`.
 * @post Neither argument is modified; the result is owned by `robots`.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
const char* mdl_robots_disallow_reason(const mdl_robots_t* robots, const char* path);

/**
 * @struct mdl_robots_cache_entry_t
 * @brief One host's cached robots.txt outcome.
 * @details Populated on first contact with a host and reused thereafter.
 * @invariant `used` is true once `host` and `rules` are valid.
 * @see mdl_robots_cache_consult()
 * @since 0.1.0
 */
typedef struct {
  char         host[k_mdl_robots_host_max]; /**< Host this entry describes.  */
  bool         used;                        /**< Entry is populated.         */
  bool         disallow_all;                /**< 5xx convention: refuse all. */
  mdl_robots_t rules;                       /**< Parsed rules for our UA.    */
} mdl_robots_cache_entry_t;

/**
 * @struct mdl_robots_cache_t
 * @brief Fixed-size per-host robots.txt cache for one run.
 * @details Zero-initialise before first use; holds up to
 *          ::k_mdl_robots_max_hosts distinct hosts.
 * @invariant Entries are filled front-to-back; `[i].used` gaps do not occur.
 * @see mdl_robots_cache_consult()
 * @since 0.1.0
 */
typedef struct {
  mdl_robots_cache_entry_t hosts[k_mdl_robots_max_hosts]; /**< Cached hosts.  */
} mdl_robots_cache_t;

/**
 * @brief Fetch-callback type: retrieve `robots_url` into `buf`.
 * @param[in]  ctx     Opaque caller context (the network interface).
 * @param[in]  robots_url Absolute URL of the host's `/robots.txt`.
 * @param[out] buf     Destination buffer for the body.
 * @param[in]  cap     Capacity of `buf`.
 * @param[out] out_len Bytes written to `buf`.
 * @return The fetch outcome class.
 * @since 0.1.0
 */
typedef mdl_robots_fetch_result_t (
  *mdl_robots_fetch_fn)(void* ctx, const char* robots_url, char* buf, size_t cap, size_t* out_len);

/**
 * @brief Return the cached robots rules for `host`, fetching on first contact.
 *
 * @details
 * On a cache miss, builds `<scheme>://<host>/robots.txt`, calls `fetch`,
 * applies the RFC 9309 convention (absent/transport error = allow all; 5xx =
 * disallow all), parses a retrieved body for `ua_token`, and stores the
 * outcome. Subsequent calls for the same host return the stored entry without
 * a network round-trip. When the cache is full, the fetched result is returned
 * uncached rather than evicting a live entry.
 *
 * @param[in]  cache    Per-run cache (zero-initialised before first use).
 * @param[in]  scheme   URL scheme (`"http"` or `"https"`).
 * @param[in]  host     Host to consult.
 * @param[in]  ua_token Our user-agent product token.
 * @param[in]  fetch    Fetch callback (dependency injection seam).
 * @param[in]  ctx      Opaque context passed to `fetch`.
 * @param[out] scratch  Working buffer the callback fills.
 * @param[in]  scratch_cap Capacity of `scratch`.
 *
 * @return Borrowed pointer to the host's parsed rules (owned by `cache`), or
 *         NULL when `disallow_all` applies (caller refuses every path).
 * @retval non-NULL Rules to test paths against.
 * @retval NULL     The host said "disallow all" (5xx); refuse every path.
 *
 * @pre Every pointer argument is non-NULL; `scratch_cap > 0`.
 * @pre `scheme` is `"http"` or `"https"`.
 * @post A cache entry for `host` exists unless the cache was full.
 * @post `scratch` may be overwritten.
 *
 * @note Not thread-safe: mutates `cache`.
 * @since 0.1.0
 */
const mdl_robots_t* mdl_robots_cache_consult(mdl_robots_cache_t* cache,
                                             const char*         scheme,
                                             const char*         host,
                                             const char*         ua_token,
                                             mdl_robots_fetch_fn fetch,
                                             void*               ctx,
                                             char*               scratch,
                                             size_t              scratch_cap);
