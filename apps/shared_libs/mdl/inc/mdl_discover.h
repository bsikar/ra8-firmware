/**
 * @file mdl_discover.h
 * @brief Search / browse discovery: governed fetch of a results page, parse,
 *        honest presentation, and optional select-for-download.
 *
 * @details
 * The network-facing half of #304, kept out of `main.c` so the entry point
 * stays a thin dispatcher and so the pure parse policy (::mdl_search) is tested
 * on its own. Given an already-built session (honest User-Agent + robots cache)
 * and politeness governor, ::mdl_discover_run builds the query URL from the site
 * descriptor, gates it against robots.txt, fetches it through the *same*
 * governor every other request uses (search is never a rate-limit bypass),
 * parses the results with ::mdl_extract_hits, and prints one of three honest
 * outcomes: a numbered list, a genuine "no results", or "the markup changed".
 *
 * When a 1-based pick index is supplied it also copies the chosen series URL out
 * for the caller to feed straight into a download -- the combined
 * search-and-select flow that removes the copy-paste step.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_politeness.h"
#include "mdl_session.h"

/**
 * @enum mdl_discover_mode_t
 * @brief Which discovery endpoint of a site descriptor to query.
 * @see mdl_discover_run()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mdl_discover_search = 0, /**< `search_url` with a `{q}` term substitution. */
  k_mdl_discover_browse = 1, /**< `browse_url` latest-updates page, no term.   */
} mdl_discover_mode_t;

/**
 * @struct mdl_discover_req_t
 * @brief Injected dependencies and inputs for one discovery run.
 * @details Bundled so ::mdl_discover_run takes one options pointer rather than a
 *          long positional list. The session and governor are built and owned by
 *          the caller; the scratch buffer and hit list are borrowed for the run.
 * @invariant `page_buf`/`page_cap` describe a writable buffer; `page_cap > 0`.
 * @see mdl_discover_run()
 * @since 0.1.0
 */
typedef struct {
  mdl_session_t*      session;    /**< Initialised session (net + UA + robots).  */
  mdl_governor_t*     gov;        /**< Initialised per-host politeness governor. */
  const mdl_site_t*   site;       /**< Descriptor holding the discovery keys.    */
  mdl_discover_mode_t mode;       /**< Search or browse.                         */
  const char*         term;       /**< Raw search term; ignored/NULL for browse. */
  uint32_t            timeout_ms; /**< Per-request time budget, milliseconds.    */
  char*               page_buf;   /**< Results-page fetch scratch (borrowed).    */
  size_t              page_cap;   /**< Capacity of @ref page_buf in bytes.       */
  mdl_hit_list_t*     hits;       /**< Receives the parsed hits (borrowed).      */
  ra8_io_stream_t*    output;     /**< Borrowed normal-output byte sink.         */
  ra8_io_stream_t*    diagnostic; /**< Borrowed diagnostic byte sink.            */
  ra8_err_t*          io_error;   /**< Caller-owned first sink failure latch.    */
} mdl_discover_req_t;

/**
 * @brief Run one search/browse discovery: fetch, parse, present, and select.
 *
 * @details
 * Builds the results URL for @p req->mode from the site descriptor (encoding the
 * term and expanding `{q}` for search; using `browse_url` verbatim for browse),
 * refuses when the descriptor supplies no such endpoint, gates the URL against
 * robots.txt, and fetches it through @p req->gov with bounded governed retries.
 * It then parses the response with ::mdl_extract_hits and classifies it with
 * ::mdl_search_classify, printing a distinct message for each honest outcome so
 * an empty result is never dressed up as a successful search. When @p pick is
 * non-zero it names the @p pick-th hit (1-based) and copies its series URL into
 * @p out_url for a follow-on download.
 *
 * @param[in]  req     Injected dependencies and inputs (never NULL).
 * @param[in]  pick    1-based hit to select, or 0 to only list results.
 * @param[out] out_url Receives the selected series URL, or "" when nothing was
 *                     selected. May be NULL only when @p pick is 0.
 * @param[in]  out_cap Capacity of @p out_url in bytes (>= 1 when @p out_url set).
 *
 * @return A process exit code.
 * @retval 0 Listed results (possibly zero) or selected a hit successfully.
 * @retval 1 The request failed, or the page could not be read as results, or a
 *           pick was requested with no results to choose from.
 * @retval 2 A capability/usage error: no such discovery endpoint configured, an
 *           empty/oversized term, or a pick index out of range.
 *
 * @pre @p req and its `session`/`gov`/`site`/`page_buf`/`hits` are non-NULL.
 * @pre @p out_url has room for @p out_cap bytes when @p pick is non-zero.
 * @post @p req->hits holds the parsed hits for the run.
 * @post `*out_url` is a selected URL exactly on a ::0 return with @p pick != 0.
 *
 * @note Not thread-safe: mutates the session cache, governor and buffers.
 * @see mdl_extract_hits
 * @see mdl_search_classify
 * @since 0.1.0
 */
int mdl_discover_run(const mdl_discover_req_t* req, size_t pick, char* out_url, size_t out_cap);
