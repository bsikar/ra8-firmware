/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
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
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Fixed capacities for an extracted URL list (zero dynamic alloc). */
typedef enum : uint16_t {
  k_mdl_max_urls = 2048, /**< Max URLs captured per page (chapters or images). */
  k_mdl_url_max  = 512,  /**< Max bytes per URL, including the NUL. */
} mdl_extract_limits_t;

/** @brief Bounded list of absolute URLs found on a page. */
typedef struct {
  char   urls[k_mdl_max_urls][k_mdl_url_max]; /**< Absolute, NUL-terminated. */
  size_t count;                               /**< Number of valid entries. */
} mdl_url_list_t;

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
