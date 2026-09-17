/**
 * @file reflow_link.c
 * @brief Page tap-target queries: link + image hit-test, anchor lookup, href split.
 *
 * @details
 * Implements the public "what did the user tap on this page" query surface
 * declared in reflow.h (#110, extended for tap-to-zoom in #478). The layout
 * pass (reflow_layout.c) populates `engine->link_rects[]`,
 * `engine->link_targets[]`, `engine->anchors[]` and `engine->image_boxes[]`;
 * this TU reads them:
 *  - reflow_hit_test_link(): point -> href (for a tap).
 *  - reflow_hit_test_image(): point -> laid-out `<img>` box index.
 *  - reflow_find_anchor(): `#fragment` id -> page.
 *  - reflow_href_split(): pure classification of an href string.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "reflow.h"

/** @brief Log tag for the link/anchor query module. */
static const char* const s_tag_link = "ra8_link";

ra8_err_t reflow_hit_test_link(const reflow_t* engine,
                               uint32_t        page_idx,
                               int32_t         x,
                               int32_t         y,
                               uint32_t*       out_href_off,
                               uint32_t*       out_href_len)
{
  RA8_CHECK_NULL_PTR(engine, s_tag_link, "hit: null engine");
  RA8_CHECK_NULL_PTR(out_href_off, s_tag_link, "hit: null out_href_off");
  RA8_CHECK_NULL_PTR(out_href_len, s_tag_link, "hit: null out_href_len");

  for (uint32_t i = 0U; i < engine->link_rect_count; ++i) {
    const reflow_link_rect_t* rect = &engine->link_rects[i];
    if (rect->page_index != page_idx) {
      continue;
    }
    if ((x >= rect->x) && (x < (rect->x + rect->w)) && (y >= rect->y) &&
        (y < (rect->y + rect->h))) {
      const reflow_link_target_t* target = &engine->link_targets[rect->target];
      *out_href_off                      = target->href_off;
      *out_href_len                      = target->href_len;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

ra8_err_t reflow_hit_test_image(const reflow_t* engine,
                                uint32_t        page_idx,
                                int32_t         x,
                                int32_t         y,
                                uint32_t*       out_index)
{
  RA8_CHECK_NULL_PTR(engine, s_tag_link, "img hit: null engine");
  RA8_CHECK_NULL_PTR(out_index, s_tag_link, "img hit: null out_index");

  for (uint32_t i = 0U; i < engine->image_box_count; ++i) {
    const reflow_image_box_t* box = &engine->image_boxes[i];
    if (box->page_index != page_idx) {
      continue;
    }
    /* Decision: the point is inside the box (4 conditions, half-open on the
     * right/bottom edges so abutting figures never both claim a tap). */
    if ((x >= box->x) && (x < (box->x + box->w)) && (y >= box->y) && (y < (box->y + box->h))) {
      *out_index = i;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

ra8_err_t
reflow_find_anchor(const reflow_t* engine, const char* id, uint32_t id_len, uint32_t* out_page)
{
  RA8_CHECK_NULL_PTR(engine, s_tag_link, "anchor: null engine");
  RA8_CHECK_NULL_PTR(id, s_tag_link, "anchor: null id");
  RA8_CHECK_NULL_PTR(out_page, s_tag_link, "anchor: null out_page");
  if (id_len == 0U) {
    return k_ra8_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < engine->anchor_count; ++i) {
    const reflow_anchor_t* anchor = &engine->anchors[i];
    bool                   equal  = anchor->id_len == id_len;
    for (uint32_t j = 0U; equal && (j < id_len); ++j) {
      if (engine->text_pool[anchor->id_off + j] != (uint8_t)id[j]) {
        equal = false;
      }
    }
    if (equal) {
      *out_page = anchor->page_index;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

/**
 * @brief True iff @p href has a URI scheme (a ':' before any '/' in `[0,end)`).
 *
 * @details A scheme makes the link external (http:, https:, mailto:, ...),
 * which the e-reader does not follow. A ':' after a '/' is a path character,
 * not a scheme. The function scans bytes in order from index 0 up to (but not
 * including) @p end; the first '/' terminates the scan with a false result and
 * the first ':' terminates it with a true result. If neither character appears
 * within the range the link is treated as scheme-free.
 *
 * @param[in] href Href byte array; must not be NULL when end > 0.
 * @param[in] end  Exclusive scan bound (the '#' position or total length).
 * @return bool Classification result.
 * @retval true  A URI scheme delimiter ':' was found before any '/'.
 * @retval false No scheme present; ':' absent or appears after the first '/'.
 *
 * @pre @p href is a valid pointer to at least @p end readable bytes.
 * @pre @p end is less than or equal to the total length of the href buffer.
 * @post The href buffer is not modified.
 * @post The return value reflects the first ':' or '/' found in [0, end).
 *
 * @note Not thread-safe; the caller must ensure @p href is stable for the
 *       duration of the call.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_has_scheme(const char* href, uint32_t end)
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
 * one of ::reflow_href_kind_t. Factored out of reflow_href_split() so the
 * public wrapper holds only the pointer validation (keeps each within the
 * cognitive-complexity budget). The algorithm first scans for the first '#'
 * character to separate the path part from the optional fragment. It then
 * delegates scheme detection to internal_has_scheme() on the path sub-range. Based
 * on whether a scheme, a fragment, and a non-empty path are present, one of the
 * five ::reflow_href_kind_t values is written to @p out_kind. Fragment
 * offset and length are written only when a '#' was found; otherwise the
 * caller-initialized zeros remain unchanged.
 *
 * @param[in]  href         Href byte array; must not be NULL and len > 0.
 * @param[in]  len          Length of @p href in bytes; must be greater than 0.
 * @param[out] out_kind     Receives the link classification result.
 * @param[out] out_path_len Receives the byte length of the path part (before '#').
 * @param[out] out_frag_off Receives the byte offset of the fragment (after '#'); 0 if none.
 * @param[out] out_frag_len Receives the byte length of the fragment; 0 if none.
 * @return Nothing.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre @p len is greater than 0 (callers must guard before invoking).
 * @post @p out_kind is set to a valid ::reflow_href_kind_t value.
 * @post @p out_path_len reflects the number of bytes before the first '#' or @p len.
 *
 * @note Not thread-safe; the caller must ensure @p href is stable for the
 *       duration of the call.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_href_classify(const char*         href,
                                   uint32_t            len,
                                   reflow_href_kind_t* out_kind,
                                   uint32_t*           out_path_len,
                                   uint32_t*           out_frag_off,
                                   uint32_t*           out_frag_len)
{
  uint32_t hash = len;
  for (uint32_t i = 0U; i < len; ++i) {
    if (href[i] == '#') {
      hash = i;
      break;
    }
  }
  if (internal_has_scheme(href, hash)) {
    *out_kind     = k_reflow_href_external;
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
    *out_kind = has_frag ? k_reflow_href_fragment : k_reflow_href_empty;
  } else {
    *out_kind = has_frag ? k_reflow_href_chapter_fragment : k_reflow_href_chapter;
  }
}

ra8_err_t reflow_href_split(const char*         href,
                            uint32_t            len,
                            reflow_href_kind_t* out_kind,
                            uint32_t*           out_path_len,
                            uint32_t*           out_frag_off,
                            uint32_t*           out_frag_len)
{
  RA8_CHECK_NULL_PTR(href, s_tag_link, "split: null href");
  RA8_CHECK_NULL_PTR(out_kind, s_tag_link, "split: null out_kind");
  RA8_CHECK_NULL_PTR(out_path_len, s_tag_link, "split: null out_path_len");
  RA8_CHECK_NULL_PTR(out_frag_off, s_tag_link, "split: null out_frag_off");
  RA8_CHECK_NULL_PTR(out_frag_len, s_tag_link, "split: null out_frag_len");

  *out_path_len = 0U;
  *out_frag_off = 0U;
  *out_frag_len = 0U;
  if (len == 0U) {
    *out_kind = k_reflow_href_empty;
    return k_ra8_ok;
  }
  internal_href_classify(href, len, out_kind, out_path_len, out_frag_off, out_frag_len);
  return k_ra8_ok;
}
