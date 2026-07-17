/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_extract.h
 * @brief Extract image URLs from an HTML page (v0 bounded tag scanner).
 *
 * @details
 * v0 is a deliberately small `<img>` attribute scanner, NOT a DOM parser: it
 * finds image tags, reads the preferred attribute (with one fallback), and
 * resolves relative URLs against the page URL. It is enough to prove the
 * end-to-end path host-side. On-device this is replaced by litehtml (already
 * vendored) behind this exact signature, and the per-site CSS selectors move
 * into a config descriptor -- neither change touches callers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Fixed capacities for the extracted URL list (zero dynamic alloc). */
typedef enum : uint16_t {
  k_mdl_max_images = 512,  /**< Max image URLs captured per page. */
  k_mdl_url_max    = 1024, /**< Max bytes per URL, including the NUL. */
} mdl_extract_limits_t;

/** @brief Bounded list of absolute image URLs found on a page. */
typedef struct {
  char   urls[k_mdl_max_images][k_mdl_url_max]; /**< Absolute, NUL-terminated. */
  size_t count;                                 /**< Number of valid entries. */
} mdl_url_list_t;

/**
 * @brief Scan `html` for `<img>` image URLs, resolved to absolute form.
 *
 * @param[in]  html        HTML bytes (need not be NUL-terminated).
 * @param[in]  html_len    Length of `html` in bytes.
 * @param[in]  base_url    Absolute URL of the page (for relative resolution).
 * @param[in]  prefer_attr "data-src" or "src"; the other is tried as fallback.
 * @param[out] out         List to fill; `out->count` is reset first.
 *
 * @retval k_ra8_ok            Scan complete (count may be 0).
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_err_no_mem    Reached ::k_mdl_max_images; remainder skipped.
 *
 * @note Duplicate URLs are dropped. Values longer than ::k_mdl_url_max are
 *       skipped rather than truncated.
 */
ra8_err_t mdl_extract_images(const char* html, size_t html_len,
                             const char* base_url, const char* prefer_attr,
                             mdl_url_list_t* out);
