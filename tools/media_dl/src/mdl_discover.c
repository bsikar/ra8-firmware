/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_discover.c
 * @brief Governed search/browse: build the query URL, fetch it politely, parse
 *        the hits, and present them honestly (with optional select-for-download).
 */
#include "mdl_discover.h"

#include <stdio.h>
#include <string.h>

#include "mdl_fetch_internal.h"
#include "mdl_net.h"
#include "mdl_search.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local buffer sizes and the bounded governed-retry budget. */
typedef enum : uint16_t {
  k_disc_url_max      = 1024, /**< Composed results-URL buffer bytes.    */
  k_disc_term_max     = 768,  /**< Percent-encoded term buffer bytes.    */
  k_disc_max_attempts = 4,    /**< 1 initial + up to 3 governed retries. */
} mdl_discover_size_t;

/** @brief Larger of two unsigned values. */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/** @brief The human word for a discovery mode, for diagnostics. */
RA8_INTERNAL static const char* mode_word(mdl_discover_mode_t m)
{
  return (m == k_mdl_discover_search) ? "search" : "browse";
}

/**
 * @brief Build the results URL for this run; false + message on a config gap.
 * @details Sets `*rc` to 2 (capability/usage) on every false return.
 */
RA8_INTERNAL static bool discover_url(const mdl_discover_req_t* req, char* out, size_t cap, int* rc)
{
  *rc = 2;
  if (req->mode == k_mdl_discover_browse) {
    if (req->site->browse_url[0] == '\0') {
      (void)fprintf(stderr,
                    "media_dl: this site descriptor has no browse_url; browsing is "
                    "unavailable\n");
      return false;
    }
    const int n = snprintf(out, cap, "%s", req->site->browse_url);
    if ((n < 0) || ((size_t)n >= cap)) {
      (void)fprintf(stderr, "media_dl: browse_url is too long\n");
      return false;
    }
    return true;
  }
  if (req->site->search_url[0] == '\0') {
    (void)fprintf(stderr,
                  "media_dl: this site descriptor has no search_url; searching is "
                  "unavailable\n");
    return false;
  }
  if ((req->term == nullptr) || (req->term[0] == '\0')) {
    (void)fprintf(stderr, "media_dl: --search needs a non-empty TERM\n");
    return false;
  }
  char encoded[k_disc_term_max];
  if (!mdl_query_encode(req->term, encoded, sizeof(encoded))) {
    (void)fprintf(stderr, "media_dl: search term is too long to encode\n");
    return false;
  }
  if (!mdl_search_build_url(req->site->search_url, encoded, out, cap)) {
    (void)fprintf(stderr,
                  "media_dl: search_url '%s' has no %s placeholder (or is too long)\n",
                  req->site->search_url,
                  mdl_search_placeholder());
    return false;
  }
  return true;
}

/**
 * @brief Robots-gate and governed-fetch the results page into `req->page_buf`.
 * @details One request through the shared governor, with bounded backoff retries
 *          on a retryable class -- search is never a rate-limit bypass.
 */
RA8_INTERNAL static ra8_err_t
discover_fetch(const mdl_discover_req_t* req, const char* url, size_t* out_len, long* out_status)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(req->session, url, &crawl)) {
    return k_ra8_fail; /* robots refused (message already printed) */
  }
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host = mdl_url_host(url, hostbuf, sizeof(hostbuf)) ? hostbuf : nullptr;
  const uint32_t      jmin = max_u32(req->site->chapter_delay_min, crawl);
  const uint32_t      jmax = max_u32(req->site->chapter_delay_max, crawl);
  const mdl_net_req_t nreq = {.user_agent = req->session->user_agent,
                              .referer    = nullptr,
                              .timeout_ms = req->timeout_ms};
  ra8_err_t           rc   = k_ra8_fail;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_disc_max_attempts; ++attempt) {
    if (mdl_governor_acquire(req->gov, host, jmin, jmax) == k_ra8_err_would_block) {
      rc = k_ra8_err_would_block;
      continue; /* no slot reserved, so nothing to release */
    }
    mdl_net_resp_t resp = {};
    rc =
      mdl_net_get_buf(req->session->net, url, &nreq, req->page_buf, req->page_cap, out_len, &resp);
    mdl_governor_observe(req->gov, host, resp.status, resp.retry_after);
    mdl_governor_release(req->gov, host);
    *out_status = resp.status;
    if (!mdl_fetch_is_retryable(rc)) {
      break;
    }
  }
  return rc;
}

/** @brief Print the numbered results header for the run's mode. */
RA8_INTERNAL static void print_header(const mdl_discover_req_t* req, size_t n)
{
  if (req->mode == k_mdl_discover_search) {
    (void)printf("search results for '%s' (%zu):\n", (req->term != nullptr) ? req->term : "", n);
  } else {
    (void)printf("latest updates on %s (%zu):\n", req->site->host, n);
  }
}

/** @brief Print every hit as `[N] title` over its series URL. */
RA8_INTERNAL static void print_hits(const mdl_hit_list_t* hits)
{
  for (size_t i = 0U; i < hits->count; ++i) {
    (void)printf("  [%zu] %s\n      %s\n", i + 1U, hits->hits[i].title, hits->hits[i].url);
  }
}

/** @brief The honest "no results" message, distinct from a broken page. */
RA8_INTERNAL static void print_zero(const mdl_discover_req_t* req)
{
  const char* pat = (req->site->search_result_contains[0] != '\0')
                      ? req->site->search_result_contains
                      : "(none configured)";
  if (req->mode == k_mdl_discover_search) {
    (void)fprintf(stderr,
                  "media_dl: no results for '%s' -- the page had %zu link(s) but none matched the "
                  "result pattern '%s' (a genuine no-match, or the result markup changed)\n",
                  (req->term != nullptr) ? req->term : "",
                  req->hits->anchors_seen,
                  pat);
  } else {
    (void)fprintf(stderr,
                  "media_dl: nothing to browse on %s -- the page had %zu link(s) but none matched "
                  "the result pattern '%s'\n",
                  req->site->host,
                  req->hits->anchors_seen,
                  pat);
  }
}

/** @brief The honest "the markup changed" message, distinct from no results. */
RA8_INTERNAL static void print_markup_changed(const mdl_discover_req_t* req)
{
  (void)fprintf(stderr,
                "media_dl: could not read a results page from %s -- the response carried no links, "
                "so its markup may have changed or the request was blocked\n",
                req->site->host);
}

/** @brief Copy the picked hit's URL out and announce it; false + message on overrun. */
RA8_INTERNAL static bool
select_hit(const mdl_hit_t* chosen, size_t pick, char* out_url, size_t out_cap)
{
  const int n = snprintf(out_url, out_cap, "%s", chosen->url);
  if ((n < 0) || ((size_t)n >= out_cap)) {
    (void)fprintf(stderr, "media_dl: selected URL is too long to hand off\n");
    if (out_cap > 0U) {
      out_url[0] = '\0';
    }
    return false;
  }
  (void)printf("selected [%zu] %s\n  %s\n", pick, chosen->title, chosen->url);
  return true;
}

/** @brief Present the outcome and, when `pick != 0`, resolve the selection. */
RA8_INTERNAL static int present_and_pick(const mdl_discover_req_t* req,
                                         mdl_search_outcome_t      outcome,
                                         size_t                    pick,
                                         char*                     out_url,
                                         size_t                    out_cap)
{
  const mdl_hit_list_t* hits = req->hits;
  if (outcome == k_mdl_search_have_results) {
    print_header(req, hits->count);
    print_hits(hits);
    (void)printf("  (add --pick N to download %s N, e.g. --pick 1 --format cbz)\n",
                 (req->mode == k_mdl_discover_search) ? "result" : "update");
  } else if (outcome == k_mdl_search_zero_results) {
    print_zero(req);
  } else {
    print_markup_changed(req);
  }

  if (pick == 0U) {
    return (outcome == k_mdl_search_markup_changed) ? 1 : 0;
  }
  if ((out_url == nullptr) || (out_cap == 0U)) {
    (void)fprintf(stderr, "media_dl: --pick needs an output buffer to hand off the selection\n");
    return 2; /* precondition: a pick requires somewhere to return the URL */
  }
  if (outcome != k_mdl_search_have_results) {
    (void)fprintf(stderr,
                  "media_dl: --pick %zu but the %s returned no results\n",
                  pick,
                  mode_word(req->mode));
    return 1;
  }
  if (pick > hits->count) {
    (void)fprintf(stderr, "media_dl: --pick %zu out of range (1..%zu)\n", pick, hits->count);
    return 2;
  }
  return select_hit(&hits->hits[pick - 1U], pick, out_url, out_cap) ? 0 : 1;
}

int mdl_discover_run(const mdl_discover_req_t* req, size_t pick, char* out_url, size_t out_cap)
{
  if ((out_url != nullptr) && (out_cap > 0U)) {
    out_url[0] = '\0';
  }
  if ((req == nullptr) || (req->session == nullptr) || (req->gov == nullptr) ||
      (req->site == nullptr) || (req->page_buf == nullptr) || (req->hits == nullptr)) {
    (void)fprintf(stderr, "media_dl: discovery called with missing dependencies\n");
    return 2;
  }
  char url[k_disc_url_max];
  int  rc = 0;
  if (!discover_url(req, url, sizeof(url), &rc)) {
    return rc;
  }
  size_t          len    = 0U;
  long            status = 0;
  const ra8_err_t frc    = discover_fetch(req, url, &len, &status);
  if (frc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    mdl_fetch_reason(frc, status, reason, sizeof(reason));
    (void)fprintf(stderr, "media_dl: %s request failed -- %s\n", mode_word(req->mode), reason);
    return 1;
  }
  (void)mdl_extract_hits(req->page_buf, len, url, req->site->search_result_contains, req->hits);
  const mdl_search_outcome_t outcome = mdl_search_classify(req->hits);
  return present_and_pick(req, outcome, pick, out_url, out_cap);
}
