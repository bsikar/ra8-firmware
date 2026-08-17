/**
 * @file mdl_discover.c
 * @brief Governed search/browse: build the query URL, fetch it politely, parse
 *        the hits, and present them honestly (with optional select-for-download).
 * @details Uses injected network, policy, output, and diagnostic dependencies
 *          for bounded discovery and explicit selection.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_discover.h"

#include <stdio.h>
#include <string.h>

#include "mdl_fetch_internal.h"
#include "mdl_net.h"
#include "mdl_search.h"
#include "mdl_stream_internal.h"
#include "mdl_url_guard.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local buffer sizes and the bounded governed-retry budget. */
typedef enum : uint16_t {
  k_disc_url_max      = 1024, /**< Composed results-URL buffer bytes.    */
  k_disc_term_max     = 768,  /**< Percent-encoded term buffer bytes.    */
  k_disc_max_attempts = 4,    /**< 1 initial + up to 3 governed retries. */
} mdl_discover_size_t;

/** @brief Append three discovery fragments and latch the first sink failure.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @param[in,out] stream Destination stream state.
 * @param[in] first First text fragment.
 * @param[in] second Second text fragment.
 * @param[in] third Third text fragment.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_discover_text3(const mdl_discover_req_t* req,
                                                 ra8_io_stream_t*          stream,
                                                 const char*               first,
                                                 const char*               second,
                                                 const char*               third)
{
  if ((req->io_error != nullptr) && (*req->io_error != k_ra8_ok)) {
    return;
  }
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  error           = priv_mdl_stream_text(error, stream, third);
  if ((error != k_ra8_ok) && (req->io_error != nullptr) && (*req->io_error == k_ra8_ok)) {
    *req->io_error = error;
  }
}

/** @brief Larger of two unsigned values.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] a First unsigned operand.
 * @param[in] b Second unsigned operand.
 * @return Larger of @p a and @p b.
 * @retval 0 Both operands were zero.
 * @retval other The larger nonzero operand.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint32_t internal_max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/** @brief The human word for a discovery mode, for diagnostics. */
RA8_INTERNAL static const char* internal_mode_word(mdl_discover_mode_t m)
{
  return (m == k_mdl_discover_search) ? "search" : "browse";
}

/**
 * @brief Build the results URL for this run; false + message on a config gap.
 * @details Sets `*rc` to 2 (capability/usage) on every false return.

 * @param[in] req Validated request description.
 * @param[out] out Caller-owned result storage.
 * @param[in] cap Destination capacity including any terminator.
 * @param[out] rc Status accumulator updated on failure.
 * @return True when a complete results URL was written to @p out.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool
internal_discover_url(const mdl_discover_req_t* req, char* out, size_t cap, int* rc)
{
  *rc = 2;
  if (req->mode == k_mdl_discover_browse) {
    if (req->site->browse_url[0] == '\0') {
      internal_discover_text3(req,
                              req->diagnostic,
                              "media_dl: this site descriptor has no browse_url; browsing is ",
                              "",
                              "unavailable\n");
      return false;
    }
    const int n = snprintf(out, cap, "%s", req->site->browse_url);
    if ((n < 0) || ((size_t)n >= cap)) {
      internal_discover_text3(req, req->diagnostic, "media_dl: browse_url is too long\n", "", "");
      return false;
    }
    return true;
  }
  if (req->site->search_url[0] == '\0') {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: this site descriptor has no search_url; searching is ",
                            "",
                            "unavailable\n");
    return false;
  }
  if ((req->term == nullptr) || (req->term[0] == '\0')) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: --search needs a non-empty TERM\n",
                            "",
                            "");
    return false;
  }
  char encoded[k_disc_term_max];
  if (!mdl_query_encode(req->term, encoded, sizeof(encoded))) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: search term is too long to encode\n",
                            "",
                            "");
    return false;
  }
  if (!mdl_search_build_url(req->site->search_url, encoded, out, cap)) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: search_url '",
                            req->site->search_url,
                            "' has no ");
    internal_discover_text3(req,
                            req->diagnostic,
                            "",
                            mdl_search_placeholder(),
                            " placeholder (or is too long)\n");
    return false;
  }
  return (req->io_error == nullptr) || (*req->io_error == k_ra8_ok);
}

/**
 * @brief Robots-gate and governed-fetch the results page into `req->page_buf`.
 * @details One request through the shared governor, with bounded backoff retries
 *          on a retryable class -- search is never a rate-limit bypass.

 * @param[in] req Validated request description.
 * @param[in] url NUL-terminated URL input.
 * @param[out] out_len Receives the produced byte length.
 * @param[out] out_status Receives the protocol status.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_discover_fetch(const mdl_discover_req_t* req,
                                                      const char*               url,
                                                      size_t*                   out_len,
                                                      long*                     out_status)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(req->session, url, &crawl)) {
    return k_ra8_fail; /* robots refused (message already printed) */
  }
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host = mdl_url_host(url, hostbuf, sizeof(hostbuf)) ? hostbuf : nullptr;
  const uint32_t      jmin = internal_max_u32(req->site->chapter_delay_min, crawl);
  const uint32_t      jmax = internal_max_u32(req->site->chapter_delay_max, crawl);
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
    if (!priv_mdl_fetch_is_retryable(rc)) {
      break;
    }
  }
  return rc;
}

/** @brief Print the numbered results header for the run's mode.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @param[in] n Element or byte count.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_print_header(const mdl_discover_req_t* req, size_t n)
{
  ra8_io_stream_t* output = req->output;
  ra8_err_t        error  = k_ra8_ok;
  if (req->mode == k_mdl_discover_search) {
    error = priv_mdl_stream_text(error, output, "search results for '");
    error = priv_mdl_stream_text(error, output, (req->term != nullptr) ? req->term : "");
    error = priv_mdl_stream_text(error, output, "' (");
  } else {
    error = priv_mdl_stream_text(error, output, "latest updates on ");
    error = priv_mdl_stream_text(error, output, req->site->host);
    error = priv_mdl_stream_text(error, output, " (");
  }
  error = priv_mdl_stream_u64(error, output, n);
  error = priv_mdl_stream_text(error, output, "):\n");
  if ((error != k_ra8_ok) && (req->io_error != nullptr)) {
    *req->io_error = error;
  }
}

/** @brief Print every hit as `[N] title` over its series URL.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_print_hits(const mdl_discover_req_t* req)
{
  const mdl_hit_list_t* hits = req->hits;
  for (size_t i = 0U; i < hits->count; ++i) {
    ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, req->output, "  [");
    error           = priv_mdl_stream_u64(error, req->output, i + 1U);
    error           = priv_mdl_stream_text(error, req->output, "] ");
    error           = priv_mdl_stream_text(error, req->output, hits->hits[i].title);
    error           = priv_mdl_stream_text(error, req->output, "\n      ");
    error           = priv_mdl_stream_text(error, req->output, hits->hits[i].url);
    error           = priv_mdl_stream_text(error, req->output, "\n");
    if (error != k_ra8_ok) {
      *req->io_error = error;
      return;
    }
  }
}

/** @brief The honest "no results" message, distinct from a broken page.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_print_zero(const mdl_discover_req_t* req)
{
  const char* pat = (req->site->search_result_contains[0] != '\0')
                      ? req->site->search_result_contains
                      : "(none configured)";
  if (req->mode == k_mdl_discover_search) {
    ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, req->diagnostic, "media_dl: no results for '");
    error = priv_mdl_stream_text(error, req->diagnostic, (req->term != nullptr) ? req->term : "");
    error = priv_mdl_stream_text(error, req->diagnostic, "' -- the page had ");
    error = priv_mdl_stream_u64(error, req->diagnostic, req->hits->anchors_seen);
    error = priv_mdl_stream_text(error,
                                 req->diagnostic,
                                 " link(s) but none matched the result pattern '");
    error = priv_mdl_stream_text(error, req->diagnostic, pat);
    error = priv_mdl_stream_text(error,
                                 req->diagnostic,
                                 "' (a genuine no-match, or the result markup changed)\n");
    if (error != k_ra8_ok) {
      *req->io_error = error;
    }
  } else {
    ra8_err_t error =
      priv_mdl_stream_text(k_ra8_ok, req->diagnostic, "media_dl: nothing to browse on ");
    error = priv_mdl_stream_text(error, req->diagnostic, req->site->host);
    error = priv_mdl_stream_text(error, req->diagnostic, " -- the page had ");
    error = priv_mdl_stream_u64(error, req->diagnostic, req->hits->anchors_seen);
    error = priv_mdl_stream_text(error,
                                 req->diagnostic,
                                 " link(s) but none matched the result pattern '");
    error = priv_mdl_stream_text(error, req->diagnostic, pat);
    error = priv_mdl_stream_text(error, req->diagnostic, "'\n");
    if (error != k_ra8_ok) {
      *req->io_error = error;
    }
  }
}

/** @brief The honest "the markup changed" message, distinct from no results.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_print_markup_changed(const mdl_discover_req_t* req)
{
  internal_discover_text3(
    req,
    req->diagnostic,
    "media_dl: could not read a results page from ",
    req->site->host,
    " -- the response carried no links, so its markup may have changed or the "
    "request was blocked\n");
}

/** @brief Copy the picked hit's URL out and announce it; false + message on overrun.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @param[in] chosen Selected discovery hit.
 * @param[in] pick One-based selected hit index.
 * @param[out] out_url Receives the selected absolute URL.
 * @param[in] out_cap Destination capacity including any terminator.
 * @return True when the selected URL and announcement were published.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_select_hit(const mdl_discover_req_t* req,
                                             const mdl_hit_t*          chosen,
                                             size_t                    pick,
                                             char*                     out_url,
                                             size_t                    out_cap)
{
  const int n = snprintf(out_url, out_cap, "%s", chosen->url);
  if ((n < 0) || ((size_t)n >= out_cap)) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: selected URL is too long to hand off\n",
                            "",
                            "");
    if (out_cap > 0U) {
      out_url[0] = '\0';
    }
    return false;
  }
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, req->output, "selected [");
  error           = priv_mdl_stream_u64(error, req->output, pick);
  error           = priv_mdl_stream_text(error, req->output, "] ");
  error           = priv_mdl_stream_text(error, req->output, chosen->title);
  error           = priv_mdl_stream_text(error, req->output, "\n  ");
  error           = priv_mdl_stream_text(error, req->output, chosen->url);
  error           = priv_mdl_stream_text(error, req->output, "\n");
  if (error != k_ra8_ok) {
    *req->io_error = error;
    return false;
  }
  return true;
}

/** @brief Present the outcome and, when `pick != 0`, resolve the selection.
 * @details Uses injected stream and network state while preserving the first error.
 *          An unvalidated or out-of-range selection is never reported as success.
 * @param[in] req Validated request description.
 * @param[in] outcome Parsed discovery outcome.
 * @param[in] pick One-based selected hit index.
 * @param[out] out_url Receives the selected absolute URL.
 * @param[in] out_cap Destination capacity including any terminator.
 * @return Process-style presentation and selection status.
 * @retval 0 Results were presented and any requested selection was valid.
 * @retval 1 Markup classification or stream output failed.
 * @retval 2 The requested selection or output buffer was invalid.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static int internal_present_and_pick(const mdl_discover_req_t* req,
                                                  mdl_search_outcome_t      outcome,
                                                  size_t                    pick,
                                                  char*                     out_url,
                                                  size_t                    out_cap)
{
  const mdl_hit_list_t* hits = req->hits;
  if (outcome == k_mdl_search_have_results) {
    internal_print_header(req, hits->count);
    internal_print_hits(req);
    internal_discover_text3(req,
                            req->output,
                            "  (add --pick N to download ",
                            (req->mode == k_mdl_discover_search) ? "result" : "update",
                            " N, e.g. --pick 1 --format cbz)\n");
  } else if (outcome == k_mdl_search_zero_results) {
    internal_print_zero(req);
  } else {
    internal_print_markup_changed(req);
  }

  if ((req->io_error != nullptr) && (*req->io_error != k_ra8_ok)) {
    return 1;
  }

  if (pick == 0U) {
    return (outcome == k_mdl_search_markup_changed) ? 1 : 0;
  }
  if ((out_url == nullptr) || (out_cap == 0U)) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: --pick needs an output buffer to hand off the selection\n",
                            "",
                            "");
    return (*req->io_error == k_ra8_ok) ? 2 : 1;
  }
  if (outcome != k_mdl_search_have_results) {
    ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, req->diagnostic, "media_dl: --pick ");
    error           = priv_mdl_stream_u64(error, req->diagnostic, pick);
    error           = priv_mdl_stream_text(error, req->diagnostic, " but the ");
    error           = priv_mdl_stream_text(error, req->diagnostic, internal_mode_word(req->mode));
    *req->io_error  = priv_mdl_stream_text(error, req->diagnostic, " returned no results\n");
    return 1;
  }
  if (pick > hits->count) {
    ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, req->diagnostic, "media_dl: --pick ");
    error           = priv_mdl_stream_u64(error, req->diagnostic, pick);
    error           = priv_mdl_stream_text(error, req->diagnostic, " out of range (1..");
    error           = priv_mdl_stream_u64(error, req->diagnostic, hits->count);
    *req->io_error  = priv_mdl_stream_text(error, req->diagnostic, ")\n");
    return (*req->io_error == k_ra8_ok) ? 2 : 1;
  }
  return internal_select_hit(req, &hits->hits[pick - 1U], pick, out_url, out_cap) ? 0 : 1;
}

int mdl_discover_run(const mdl_discover_req_t* req, size_t pick, char* out_url, size_t out_cap)
{
  if ((out_url != nullptr) && (out_cap > 0U)) {
    out_url[0] = '\0';
  }
  if ((req == nullptr) || (req->session == nullptr) || (req->gov == nullptr) ||
      (req->site == nullptr) || (req->page_buf == nullptr) || (req->hits == nullptr) ||
      (req->output == nullptr) || (req->diagnostic == nullptr) || (req->io_error == nullptr)) {
    if ((req != nullptr) && (req->diagnostic != nullptr)) {
      (void)priv_mdl_stream_text(k_ra8_ok,
                                 req->diagnostic,
                                 "media_dl: discovery called with missing dependencies\n");
    }
    return 2;
  }
  char url[k_disc_url_max];
  int  rc = 0;
  if (!internal_discover_url(req, url, sizeof(url), &rc)) {
    return (*req->io_error == k_ra8_ok) ? rc : 1;
  }
  size_t          len    = 0U;
  long            status = 0;
  const ra8_err_t frc    = internal_discover_fetch(req, url, &len, &status);
  if (frc != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    priv_mdl_fetch_reason(frc, status, reason, sizeof(reason));
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: ",
                            internal_mode_word(req->mode),
                            " request failed -- ");
    internal_discover_text3(req, req->diagnostic, "", reason, "\n");
    return 1;
  }
  const ra8_err_t parsed =
    mdl_extract_hits(req->page_buf, len, url, req->site->search_result_contains, req->hits);
  if (parsed != k_ra8_ok) {
    internal_discover_text3(req,
                            req->diagnostic,
                            "media_dl: ",
                            internal_mode_word(req->mode),
                            " results exceeded the bounded hit table\n");
    return 1;
  }
  (void)mdl_search_filter_series_hits(req->hits, req->site->chapter_url_contains);
  const mdl_search_outcome_t outcome = mdl_search_classify(req->hits);
  return internal_present_and_pick(req, outcome, pick, out_url, out_cap);
}
