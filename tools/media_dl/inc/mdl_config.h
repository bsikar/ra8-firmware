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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_politeness.h"
#include "ra8_err.h"

/** @brief How to order chapters extracted from a series page. */
typedef enum : uint8_t {
  k_mdl_order_doc     = 0, /**< Keep document order.                          */
  k_mdl_order_reverse = 1, /**< Reverse document order (newest-first -> old). */
  k_mdl_order_asc     = 2, /**< Sort by the number parsed from the URL.       */
} mdl_chapter_order_t;

/** @brief Fixed field sizes for a site descriptor. */
typedef enum : uint16_t {
  k_mdl_name_max       = 64,  /**< Site display name bytes.          */
  k_mdl_host_max       = 128, /**< Host bytes.                       */
  k_mdl_kind_max       = 16,  /**< "manga"/"manhwa"/... bytes.       */
  k_mdl_match_max      = 128, /**< Selector/substring field bytes.   */
  k_mdl_attr_max       = 16,  /**< Image attribute name bytes.       */
  k_mdl_contact_max    = 128, /**< Operator contact string bytes.    */
  k_mdl_search_url_max = 256, /**< Search/browse URL template bytes. */
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
  char                chapter_url_contains[k_mdl_match_max];    /**< href must contain this. */
  char                chapter_url_prefix[k_mdl_search_url_max]; /**< Absolute series-chapter
                                                    prefix. */
  mdl_chapter_order_t chapter_order;                            /**< Ordering to apply. */

  /* Page images, found on a chapter page. */
  char page_img_attr[k_mdl_attr_max];          /**< Preferred attr (fallback other). */
  char page_img_url_contains[k_mdl_match_max]; /**< Keep only URLs with this.        */

  /* Search / discovery (#304). Empty when the site descriptor supplies none. */
  char search_url[k_mdl_search_url_max];        /**< Query template with a `{q}` slot.
                                          */
  char search_result_contains[k_mdl_match_max]; /**< Series-link substring on
                                                   results. */
  char browse_url[k_mdl_search_url_max];        /**< Latest-updates page (no `{q}`). */

  /* Metadata extraction (#306). Selectors use the bounded grammar documented
   * by mdl_extract_selector(): meta:, class:, label:, or literal:. */
  char series_title_selector[k_mdl_match_max];   /**< Series display-title
                                                    selector. */
  char series_summary_selector[k_mdl_match_max]; /**< Series synopsis selector.
                                                  */
  char series_author_selector[k_mdl_match_max];  /**< Series writer selector.    */
  char series_artist_selector[k_mdl_match_max];  /**< Series artist selector.    */
  char series_cover_selector[k_mdl_match_max];   /**< Series cover-URL selector. */
  char chapter_title_selector[k_mdl_match_max];  /**< Per-chapter title selector.
                                                 */
  char chapter_number_selector[k_mdl_match_max]; /**< Per-chapter number
                                                    selector.   */
  char language[k_mdl_kind_max];                 /**< BCP-47 language tag. */
  char reading_direction[k_mdl_kind_max];        /**< `ltr` or `rtl`.      */

  /* Politeness jitter (milliseconds); baseline spacing, jittered in [min,max].
   */
  uint32_t img_delay_min;     /**< Per-image spacing floor.       */
  uint32_t img_delay_max;     /**< Per-image spacing ceiling.     */
  uint32_t chapter_delay_min; /**< Inter-chapter spacing floor.   */
  uint32_t chapter_delay_max; /**< Inter-chapter spacing ceiling. */

  /* Governor: closed-loop per-host rate/backoff/concurrency (see
   * mdl_politeness). */
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
 * @retval k_ra8_ok            Loaded and every key/value passed validation.
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_fail          File could not be opened.
 * @retval k_ra8_err_invalid_state  A line, key, value, or required field was
 * invalid.
 */
ra8_err_t mdl_config_load(const char* path, mdl_site_t* out);

/**
 * @brief Build the politeness-governor tunables for a loaded site descriptor.
 *
 * @details
 * Overlays the descriptor's governor fields (rate/burst/backoff/in-flight) onto
 * the conservative ::mdl_gov_cfg_default baseline, so every run mode -- series
 * download, discovery, library update -- derives its governor identically from
 * one place rather than re-copying the same six assignments.
 *
 * @param[in] site Loaded descriptor (never NULL).
 *
 * @return The governor configuration for @p site.
 * @retval (by value) Defaults with the descriptor's governor fields applied.
 *
 * @pre @p site is non-NULL and was populated by ::mdl_config_load.
 * @pre The caller passes the result to a governor init function.
 * @post `burst >= 1` and `max_inflight >= 1` (the init function clamps).
 *
 * @note Thread-safe: depends only on its argument.
 * @see mdl_governor_init
 * @since 0.1.0
 */
mdl_gov_cfg_t mdl_config_gov_cfg(const mdl_site_t* site);

/**
 * @brief Raise a descriptor's per-request delay floors for `--polite`.
 *
 * @details
 * The `--polite` opt-in lifts the per-image and inter-chapter jitter floors to
 * cautious minimums (never lowering a site that is already slower). Shared by
 * the series and discovery paths so both interpret `--polite` the same way.
 *
 * @param[in,out] site Descriptor whose delay floors are raised in place.
 *
 * @return Nothing.
 *
 * @pre @p site is non-NULL and was populated by ::mdl_config_load.
 * @pre The caller applies this only when `--polite` was requested.
 * @post Each delay floor is at least its polite minimum.
 * @post A floor already above its polite minimum is left unchanged.
 *
 * @note Not thread-safe: mutates @p site in place.
 * @since 0.1.0
 */
void mdl_config_apply_polite(mdl_site_t* site);
