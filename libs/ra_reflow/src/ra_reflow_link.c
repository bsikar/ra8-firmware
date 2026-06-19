/**
 * @file ra_reflow_link.c
 * @brief In-content hyperlink hit-test, anchor lookup, and href split (#110).
 *
 * @details
 * Implements the public link/anchor query surface declared in ra_reflow.h. The
 * layout pass (ra_reflow_layout.c) populates `engine->link_rects[]`,
 * `engine->link_targets[]`, and `engine->anchors[]`; this TU reads them:
 *  - ra_reflow_hit_test_link(): point -> href (for a tap).
 *  - ra_reflow_find_anchor(): `#fragment` id -> page.
 *  - ra_reflow_href_split(): pure classification of an href string.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_reflow.h"

/** @brief Log tag for the link/anchor query module. */
static const char* const s_tag_link = "ra_link";

ra_err_t ra_reflow_hit_test_link(const ra_reflow_t* engine,
                                 uint32_t           page_idx,
                                 int32_t            x,
                                 int32_t            y,
                                 uint32_t*          out_href_off,
                                 uint32_t*          out_href_len)
{
  RA_CHECK_NULL_PTR(engine, s_tag_link, "hit: null engine");
  RA_CHECK_NULL_PTR(out_href_off, s_tag_link, "hit: null out_href_off");
  RA_CHECK_NULL_PTR(out_href_len, s_tag_link, "hit: null out_href_len");

  for (uint32_t i = 0U; i < engine->link_rect_count; ++i) {
    const ra_reflow_link_rect_t* rect = &engine->link_rects[i];
    if (rect->page_index != page_idx) {
      continue;
    }
    if ((x >= rect->x) && (x < (rect->x + rect->w)) && (y >= rect->y) &&
        (y < (rect->y + rect->h))) {
      const ra_reflow_link_target_t* target = &engine->link_targets[rect->target];
      *out_href_off                         = target->href_off;
      *out_href_len                         = target->href_len;
      return k_ra_ok;
    }
  }
  return k_ra_err_not_found;
}

ra_err_t ra_reflow_find_anchor(const ra_reflow_t* engine,
                               const char*        id,
                               uint32_t           id_len,
                               uint32_t*          out_page)
{
  RA_CHECK_NULL_PTR(engine, s_tag_link, "anchor: null engine");
  RA_CHECK_NULL_PTR(id, s_tag_link, "anchor: null id");
  RA_CHECK_NULL_PTR(out_page, s_tag_link, "anchor: null out_page");
  if (id_len == 0U) {
    return k_ra_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < engine->anchor_count; ++i) {
    const ra_reflow_anchor_t* anchor = &engine->anchors[i];
    if ((anchor->id_len == id_len) &&
        (memcmp(&engine->text_pool[anchor->id_off], id, (size_t)id_len) == 0)) {
      *out_page = anchor->page_index;
      return k_ra_ok;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief True iff @p href has a URI scheme (a ':' before any '/' in `[0,end)`).
 *
 * @details A scheme makes the link external (http:, https:, mailto:, ...),
 * which the e-reader does not follow. A ':' after a '/' is a path character,
 * not a scheme.
 *
 * @param[in] href Href bytes.
 * @param[in] end  Exclusive scan bound (the '#' position or length).
 * @return true iff a scheme is present.
 */
static bool priv_has_scheme(const char* href, uint32_t end)
{
  for (uint32_t i = 0U; i < end; ++i) {
    if (href[i] == '/') {
      return false;
    }
    if (href[i] == ':') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Core href split + classify (no validation; pure on the inputs).
 *
 * @details Locates the fragment '#', detects a URI scheme, and classifies into
 * one of ::ra_reflow_href_kind_t. Factored out of ra_reflow_href_split() so the
 * public wrapper holds only the pointer validation (keeps each within the
 * cognitive-complexity budget).
 *
 * @param[in]  href         Href bytes.
 * @param[in]  len          Length of @p href (> 0).
 * @param[out] out_kind     Receives the classification.
 * @param[out] out_path_len Receives the path-part length.
 * @param[out] out_frag_off Receives the fragment offset (0 if none).
 * @param[out] out_frag_len Receives the fragment length (0 if none).
 */
static void priv_href_classify(const char*            href,
                               uint32_t               len,
                               ra_reflow_href_kind_t* out_kind,
                               uint32_t*              out_path_len,
                               uint32_t*              out_frag_off,
                               uint32_t*              out_frag_len)
{
  uint32_t hash = len;
  for (uint32_t i = 0U; i < len; ++i) {
    if (href[i] == '#') {
      hash = i;
      break;
    }
  }
  if (priv_has_scheme(href, hash)) {
    *out_kind     = k_ra_reflow_href_external;
    *out_path_len = len;
    return;
  }
  const bool has_frag = (hash < len);
  *out_path_len       = hash;
  if (has_frag) {
    *out_frag_off = hash + 1U;
    *out_frag_len = len - hash - 1U;
  }
  if (hash == 0U) {
    *out_kind = has_frag ? k_ra_reflow_href_fragment : k_ra_reflow_href_empty;
  } else {
    *out_kind = has_frag ? k_ra_reflow_href_chapter_fragment : k_ra_reflow_href_chapter;
  }
}

ra_err_t ra_reflow_href_split(const char*            href,
                              uint32_t               len,
                              ra_reflow_href_kind_t* out_kind,
                              uint32_t*              out_path_len,
                              uint32_t*              out_frag_off,
                              uint32_t*              out_frag_len)
{
  RA_CHECK_NULL_PTR(href, s_tag_link, "split: null href");
  RA_CHECK_NULL_PTR(out_kind, s_tag_link, "split: null out_kind");
  RA_CHECK_NULL_PTR(out_path_len, s_tag_link, "split: null out_path_len");
  RA_CHECK_NULL_PTR(out_frag_off, s_tag_link, "split: null out_frag_off");
  RA_CHECK_NULL_PTR(out_frag_len, s_tag_link, "split: null out_frag_len");

  *out_path_len = 0U;
  *out_frag_off = 0U;
  *out_frag_len = 0U;
  if (len == 0U) {
    *out_kind = k_ra_reflow_href_empty;
    return k_ra_ok;
  }
  priv_href_classify(href, len, out_kind, out_path_len, out_frag_off, out_frag_len);
  return k_ra_ok;
}
