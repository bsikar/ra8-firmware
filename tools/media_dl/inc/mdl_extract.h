/**
 * @file mdl_extract.h
 * @brief Extract image URLs and anchor links from an HTML page (v1 scanner).
 *
 * @details
 * A deliberately small tag scanner, NOT a DOM parser: it finds `<img>` / `<a>`
 * tags, reads an attribute, resolves relative URLs against the page URL, and
 * filters by a substring. It is enough to drive real sites host-side. On-device
 * this is replaced by litehtml (already vendored) behind these signatures, and
 * the per-site match strings come from the config descriptor -- neither change
 * touches callers.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Fixed capacities for an extracted URL list (zero dynamic alloc). */
typedef enum : uint16_t {
  k_mdl_max_urls = 2048, /**< Max URLs captured per page (chapters or images). */
  k_mdl_url_max  = 512,  /**< Max bytes per URL, including the NUL.            */
} mdl_extract_limits_t;

/** @brief Bounded list of absolute URLs found on a page. */
typedef struct {
  char   urls[k_mdl_max_urls][k_mdl_url_max]; /**< Absolute, NUL-terminated. */
  size_t count;                               /**< Number of valid entries.  */
} mdl_url_list_t;

/** @brief Fixed capacities for a titled-anchor hit list (search/browse). */
typedef enum : uint16_t {
  k_mdl_max_hits      = 128, /**< Max titled hits captured per results page.  */
  k_mdl_hit_title_max = 256, /**< Max title bytes per hit, including the NUL. */
} mdl_hit_limits_t;

/**
 * @struct mdl_hit_t
 * @brief One discovery hit: a human-facing title paired with a series URL.
 * @details Produced by ::mdl_extract_hits from a search or latest-updates page;
 *          the `title` is the anchor's `title=` attribute, its stripped inner
 *          text, or -- when neither is present -- the URL's last path segment.
 * @invariant Both fields are NUL-terminated; `url` is absolute.
 * @see mdl_extract_hits()
 * @since 0.1.0
 */
typedef struct {
  char title[k_mdl_hit_title_max]; /**< Display title, NUL-terminated.       */
  char url[k_mdl_url_max];         /**< Absolute series URL, NUL-terminated. */
} mdl_hit_t;

/**
 * @struct mdl_hit_list_t
 * @brief Bounded list of titled hits plus the raw anchor tally.
 * @details `anchors_seen` counts every `<a>` with a resolvable href scanned on
 *          the page (before the result filter), so a caller can tell a page that
 *          rendered links but matched none (zero results) from one that carried
 *          no links at all (the markup changed or the request was blocked). Large
 *          enough to prefer file scope over a deep stack.
 * @invariant `count <= k_mdl_max_hits`; every stored URL is unique.
 * @see mdl_extract_hits()
 * @see mdl_search_classify()
 * @since 0.1.0
 */
typedef struct {
  mdl_hit_t hits[k_mdl_max_hits]; /**< Deduplicated titled hits.            */
  size_t    count;                /**< Number of valid hits.                */
  size_t    anchors_seen;         /**< Total resolvable `<a href>` anchors. */
} mdl_hit_list_t;

/**
 * @brief Scan `html` for `<img>` image URLs, resolved to absolute form.
 *
 * @param[in]  html         HTML bytes (need not be NUL-terminated).
 * @param[in]  html_len     Length of `html` in bytes.
 * @param[in]  base_url     Absolute URL of the page (for relative resolution).
 * @param[in]  prefer_attr  "data-src" or "src"; the other is tried as fallback.
 * @param[in]  url_contains If non-NULL and non-empty, keep only URLs that
 *                          contain this substring (drops loaders/ads/nav icons).
 * @param[out] out          List to fill; `out->count` is reset first.
 *
 * @retval k_ra8_ok            Scan complete (count may be 0).
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_err_no_mem    Reached ::k_mdl_max_urls; remainder skipped.
 *
 * @note Duplicate URLs are dropped. Values longer than ::k_mdl_url_max are
 *       skipped rather than truncated.
 */
ra8_err_t mdl_extract_images(const char*     html,
                             size_t          html_len,
                             const char*     base_url,
                             const char*     prefer_attr,
                             const char*     url_contains,
                             mdl_url_list_t* out);

/**
 * @brief Scan `html` for `<a href>` links, resolved to absolute form.
 *
 * @param[in]  html          HTML bytes (need not be NUL-terminated).
 * @param[in]  html_len      Length of `html` in bytes.
 * @param[in]  base_url      Absolute URL of the page.
 * @param[in]  href_contains If non-NULL and non-empty, keep only hrefs whose
 *                           absolute URL contains this substring.
 * @param[out] out           List to fill; `out->count` is reset first.
 *
 * @retval k_ra8_ok            Scan complete (count may be 0).
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_err_no_mem    Reached ::k_mdl_max_urls; remainder skipped.
 */
ra8_err_t mdl_extract_anchors(const char*     html,
                              size_t          html_len,
                              const char*     base_url,
                              const char*     href_contains,
                              mdl_url_list_t* out);

/**
 * @brief Scan `html` for `<a href>` links, keeping each hit's title and URL.
 *
 * @details
 * The discovery counterpart to ::mdl_extract_anchors: instead of a bare URL
 * list it yields (title, URL) pairs suitable for a numbered search/browse
 * listing. For every `<a>` whose resolved absolute href contains
 * @p url_contains it records the URL and a best-effort title -- the anchor's
 * `title=` attribute, else its inner text with nested tags stripped, whitespace
 * collapsed and the common HTML entities decoded, else the URL's last path
 * segment. Duplicate URLs are merged, and a later occurrence carrying a real
 * title upgrades an earlier slug-only fallback (a results card is often a
 * thumbnail link followed by a titled text link to the same series). The total
 * number of resolvable anchors scanned -- before the filter -- is reported in
 * `out->anchors_seen` so the caller can distinguish "no match" from "no links".
 *
 * @param[in]  html         HTML bytes (need not be NUL-terminated).
 * @param[in]  html_len     Length of @p html in bytes.
 * @param[in]  base_url     Absolute URL of the page (for relative resolution).
 * @param[in]  url_contains If non-NULL and non-empty, keep only hits whose
 *                          absolute URL contains this substring.
 * @param[out] out          List to fill; `out->count` and `out->anchors_seen`
 *                          are reset first.
 *
 * @return An ::ra8_err_t scan result.
 * @retval k_ra8_ok               Scan complete (count may be 0).
 * @retval k_ra8_err_invalid_arg  A NULL @p html, @p base_url or @p out.
 * @retval k_ra8_err_no_mem       Reached ::k_mdl_max_hits; the rest were skipped
 *                                but `anchors_seen` still counts them.
 *
 * @pre @p html, @p base_url and @p out are non-NULL.
 * @pre @p out points to writable ::mdl_hit_list_t storage.
 * @post `out->count` hits are unique by URL and each has a non-empty title.
 * @post `out->anchors_seen >= out->count`.
 *
 * @note Not thread-safe: writes caller storage.
 * @see mdl_extract_anchors
 * @see mdl_search_classify
 * @since 0.1.0
 */
ra8_err_t mdl_extract_hits(const char*     html,
                           size_t          html_len,
                           const char*     base_url,
                           const char*     url_contains,
                           mdl_hit_list_t* out);
