/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_config.h
 * @brief Per-site descriptor loaded from a flat key=value config file.
 *
 * @details
 * Replaces the Kotlin original's hardcoded `when(host)` branches: adding a site
 * is dropping in a `.conf` file, no rebuild. v1 is a deliberately small flat
 * key=value format (`#` comments, `[section]` lines ignored) parsed into a
 * fixed-size struct -- zero dynamic allocation, every field bounded, so the
 * same descriptor model ports to the RA8 unchanged. The richer TOML schema is a
 * later milestone; this proves the data-driven approach end to end.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief How to order chapters extracted from a series page. */
typedef enum : uint8_t {
  k_mdl_order_doc     = 0, /**< Keep document order.                          */
  k_mdl_order_reverse = 1, /**< Reverse document order (newest-first -> old). */
  k_mdl_order_asc     = 2, /**< Sort by the number parsed from the URL.       */
} mdl_chapter_order_t;

/** @brief Fixed field sizes for a site descriptor. */
typedef enum : uint16_t {
  k_mdl_name_max    = 64,  /**< Site display name bytes.        */
  k_mdl_host_max    = 128, /**< Host bytes.                     */
  k_mdl_kind_max    = 16,  /**< "manga"/"manhwa"/... bytes.     */
  k_mdl_match_max   = 128, /**< Selector/substring field bytes. */
  k_mdl_attr_max    = 16,  /**< Image attribute name bytes.     */
  k_mdl_contact_max = 128, /**< Operator contact string bytes.  */
} mdl_config_limits_t;

/**
 * @brief One site's scraping rules (all data, no logic).
 * @invariant Every string field is NUL-terminated.
 */
typedef struct {
  char name[k_mdl_name_max];       /**< Human-readable site name.        */
  char host[k_mdl_host_max];       /**< Site host, e.g. "manhwaus.net".  */
  char kind[k_mdl_kind_max];       /**< Content kind hint (reader mode). */
  char contact[k_mdl_contact_max]; /**< Operator contact for the UA.     */

  /* Chapter list, found on a series page. */
  char                chapter_url_contains[k_mdl_match_max]; /**< href must contain this. */
  mdl_chapter_order_t chapter_order;                         /**< Ordering to apply.      */

  /* Page images, found on a chapter page. */
  char page_img_attr[k_mdl_attr_max];          /**< Preferred attr (fallback other). */
  char page_img_url_contains[k_mdl_match_max]; /**< Keep only URLs with this.        */

  /* Politeness jitter (milliseconds); baseline spacing, jittered in [min,max]. */
  uint32_t img_delay_min;     /**< Per-image spacing floor.       */
  uint32_t img_delay_max;     /**< Per-image spacing ceiling.     */
  uint32_t chapter_delay_min; /**< Inter-chapter spacing floor.   */
  uint32_t chapter_delay_max; /**< Inter-chapter spacing ceiling. */

  /* Governor: closed-loop per-host rate/backoff/concurrency (see mdl_politeness). */
  uint32_t rate_per_min;    /**< Sustained per-host request ceiling (0 = off). */
  uint32_t burst;           /**< Token-bucket capacity, in requests.           */
  uint32_t backoff_base_ms; /**< First backoff window on a 429/503.            */
  uint32_t backoff_max_ms;  /**< Backoff-window ceiling.                       */
  uint32_t max_inflight;    /**< Per-host in-flight request cap.               */
} mdl_site_t;

/**
 * @brief Load a site descriptor from a flat key=value file.
 *
 * @param[in]  path Config file path.
 * @param[out] out  Descriptor to fill; defaults are applied first, then the
 *                  file overrides recognised keys.
 *
 * @retval k_ra8_ok            Loaded (unknown keys warned to stderr, ignored).
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_fail          File could not be opened.
 * @retval k_ra8_err_invalid_state  A required field (host) was missing.
 */
ra8_err_t mdl_config_load(const char* path, mdl_site_t* out);
