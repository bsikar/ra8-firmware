/**
 * @file mdl_fetch.c
 * @brief State-aware, resumable, deduping chapter/page download loop.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_fetch.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "mdl_atomic.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_net.h"
#include "mdl_pathfs.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local buffer sizes and the file-copy chunk. */
typedef enum : uint16_t {
  k_fetch_leaf_bytes = 32,   /**< "page_NNNN.ext" leaf name buffer. */
  k_fetch_copy_chunk = 8192, /**< Reuse-copy stream chunk bytes.    */
} mdl_fetch_size_t;

/** @brief HTTP-status boundaries used when rendering a failure reason. */
typedef enum : uint16_t {
  k_http_server_err = 500,  /**< First status that is a server error.  */
  k_ms_per_sec      = 1000, /**< Milliseconds per second (rate maths). */
} mdl_fetch_http_t;

/**
 * @struct mdl_run_pos_t
 * @brief Where a page sits in the run, for a progress event.
 * @details Bundled so the per-page functions carry one pointer rather than a
 *          fan of position scalars.
 * @since 0.1.0
 */
typedef struct {
  size_t      chapter_index; /**< 1-based chapter position in the run. */
  size_t      chapter_total; /**< Total chapters in the run.           */
  const char* chapter_id;    /**< Chapter identifier (URL leaf).       */
} mdl_run_pos_t;

/**
 * @struct mdl_page_outcome_t
 * @brief How one page transfer went, handed back for a progress event.
 * @since 0.1.0
 */
typedef struct {
  uint64_t bytes;      /**< Bytes transferred (0 when reused).     */
  uint32_t elapsed_ms; /**< Wall time for the transfer (0 reused). */
  bool     reused;     /**< Served from an already-held file.      */
} mdl_page_outcome_t;

/** @brief Loop bound on the reuse-copy stream (any real page is far under). */
typedef enum : uint32_t {
  k_fetch_copy_max_chunks = 1000000U, /**< 8 GiB ceiling.               */
  k_ns_per_ms             = 1000000U, /**< Nanoseconds per millisecond. */
} mdl_fetch_bound_t;

/** @brief Bounded attempts per request: the initial try plus backoff retries. */
typedef enum : uint8_t {
  k_fetch_max_attempts = 4U, /**< 1 initial + up to 3 governed retries on a retryable class. */
} mdl_fetch_retry_t;

/** @brief Per-chapter outcome ::process_chapter reports to the run loop. */
typedef enum : uint8_t {
  k_ch_completed = 0, /**< Every page present and verified.  */
  k_ch_skipped   = 1, /**< Already complete (--update skip). */
  k_ch_failed    = 2, /**< Left partial by a page failure.   */
} mdl_chap_status_t;

/** @brief Larger of two unsigned values. */
RA8_INTERNAL static uint32_t max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

RA8_PRIV bool mdl_fetch_is_retryable(ra8_err_t rc)
{
  return (rc == k_ra8_err_busy) || (rc == k_ra8_err_timeout) || (rc == k_ra8_fail);
}

RA8_PRIV bool mdl_fetch_run_incomplete(const mdl_fetch_stats_t* stats)
{
  if (stats == nullptr) {
    return false;
  }
  return (stats->chapters_failed > 0U) || (stats->pages_failed > 0U);
}

/** @brief Prose for a k_ra8_fail: a 5xx server error versus a transport error. */
RA8_INTERNAL static const char* fail_reason(long status)
{
  return (status >= (long)k_http_server_err) ? "server error" : "transport error";
}

RA8_PRIV void mdl_fetch_reason(ra8_err_t err, long status, char* buf, size_t cap)
{
  if (buf == nullptr) {
    return;
  }
  if (cap == 0U) {
    return;
  }
  const char* base = nullptr;
  switch (err) {
    case k_ra8_ok:
      base = "ok";
      break;
    case k_ra8_err_timeout:
      base = "request timed out";
      break;
    case k_ra8_err_busy:
      base = "rate limited";
      break;
    case k_ra8_err_not_found:
      base = "not found";
      break;
    case k_ra8_err_no_mem:
      base = "response exceeded size cap";
      break;
    case k_ra8_err_retry_limit:
      base = "still failing after retries";
      break;
    case k_ra8_fail:
      base = fail_reason(status);
      break;
    default:
      base = "error";
      break;
  }
  if (status > 0) {
    (void)snprintf(buf, cap, "%s (HTTP %ld)", base, status);
  } else {
    (void)snprintf(buf, cap, "%s", base);
  }
}

/** @brief Append one failure to the run's log (when set); always tally total. */
RA8_INTERNAL static void
record_fail(const mdl_fetch_ctx_t* ctx, const char* url, long status, ra8_err_t err)
{
  if (ctx->faillog == nullptr) {
    return;
  }
  mdl_fetch_faillog_t* log = ctx->faillog;
  log->total += 1U;
  if (log->count >= (size_t)k_mdl_fetch_fail_max) {
    return;
  }
  mdl_fetch_fail_t* item = &log->items[log->count];
  (void)snprintf(item->url, sizeof(item->url), "%s", (url != nullptr) ? url : "");
  item->status = status;
  item->err    = err;
  log->count += 1U;
}

/** @brief Persist state atomically when a checkpoint target is configured. */
RA8_INTERNAL static ra8_err_t checkpoint(const mdl_fetch_ctx_t* ctx)
{
  if (ctx->state_path == nullptr) {
    return k_ra8_ok;
  }
  const ra8_err_t rc = mdl_state_save(ctx->state_path, ctx->state);
  if (rc != k_ra8_ok) {
    record_fail(ctx, ctx->state_path, 0L, rc);
  }
  return rc;
}

/** @brief Monotonic milliseconds, but only when progress is on (else 0). */
RA8_INTERNAL static int64_t mono_ms(const mdl_fetch_ctx_t* ctx)
{
  if (ctx->progress_fn == nullptr) {
    return 0;
  }
  struct timespec ts = {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return ((int64_t)ts.tv_sec * (int64_t)k_ms_per_sec) +
         ((int64_t)ts.tv_nsec / (int64_t)k_ns_per_ms);
}

/** @brief Emit one per-page progress event through the injected sink, if any. */
RA8_INTERNAL static void emit_progress(const mdl_fetch_ctx_t*    ctx,
                                       const mdl_run_pos_t*      pos,
                                       size_t                    page_index,
                                       const mdl_page_outcome_t* out)
{
  if (ctx->progress_fn == nullptr) {
    return;
  }
  const mdl_fetch_progress_t ev = {.chapter_index = pos->chapter_index,
                                   .chapter_total = pos->chapter_total,
                                   .chapter_id    = pos->chapter_id,
                                   .page_index    = page_index,
                                   .page_total    = ctx->images->count,
                                   .page_bytes    = out->bytes,
                                   .elapsed_ms    = out->elapsed_ms,
                                   .reused        = out->reused};
  ctx->progress_fn(ctx->progress_ctx, &ev);
}

/** @brief Copy a file byte-for-byte; false on any open/read/write error. */
RA8_INTERNAL static bool copy_file(const char* src, const char* dst)
{
  if ((src == nullptr) || (dst == nullptr)) {
    return false;
  }
  FILE* in = fopen(src, "rb");
  if (in == nullptr) {
    return false;
  }
  /* Same rule as every other writer here: a partial copy must not be able to
   * destroy a good `dst` that already exists (see mdl_atomic.h). */
  char tmp[PATH_MAX];
  if (!mdl_atomic_tmp_path(dst, tmp, sizeof(tmp))) {
    (void)fclose(in);
    return false;
  }
  FILE* out = fopen(tmp, "wb");
  if (out == nullptr) {
    (void)fclose(in);
    mdl_atomic_abort(tmp);
    return false;
  }
  bool    ok  = true;
  bool    eof = false;
  uint8_t buf[k_fetch_copy_chunk];
  for (uint32_t chunk = 0U; chunk < (uint32_t)k_fetch_copy_max_chunks; ++chunk) {
    const size_t n = fread(buf, 1U, sizeof(buf), in);
    if ((n > 0U) && (fwrite(buf, 1U, n, out) != n)) {
      ok = false;
      break;
    }
    if (n < sizeof(buf)) {
      ok  = (ferror(in) == 0);
      eof = ok;
      break;
    }
  }
  const bool out_closed = fclose(out) == 0;
  const bool in_closed  = fclose(in) == 0;
  ok                    = eof && ok && out_closed && in_closed;
  if (!ok) {
    mdl_atomic_abort(tmp);
    return false;
  }
  return mdl_atomic_commit(tmp, dst);
}

/** @brief Compose the `page_NNNN.ext` leaf for one page; false if it overran. */
RA8_INTERNAL static bool page_leaf(size_t page_no, const char* url, char* out, size_t cap)
{
  char ext[8];
  mdl_urlname_ext(url, ext, sizeof(ext));
  const int n = snprintf(out, cap, "page_%04zu.%s", page_no, ext);
  return (n > 0) && ((size_t)n < cap);
}

/**
 * @brief Reuse an already-held byte-identical page instead of fetching it.
 * @details Content-hash dedup: if a page with the same source URL is recorded
 *          and its file still verifies, put those bytes at the target position
 *          (a no-op when it is already there, else a copy) and record the new
 *          location -- no network. Returns false to fall through to a fetch.
 */
RA8_INTERNAL static bool
try_reuse(mdl_fetch_ctx_t* ctx, uint64_t url_hash, const char* target_abs, const char* target_rel)
{
  const mdl_page_rec_t* held = mdl_state_find_page(ctx->state, url_hash);
  if (held == nullptr) {
    return false;
  }
  char      src_abs[PATH_MAX];
  const int sn = snprintf(src_abs, sizeof(src_abs), "%s/%s", ctx->series_abs_dir, held->rel_path);
  if ((sn < 0) || ((size_t)sn >= sizeof(src_abs))) {
    return false;
  }
  uint64_t have = 0U;
  if ((mdl_hash_file(src_abs, &have) != k_ra8_ok) || (have != held->content_hash)) {
    return false; /* the held file is gone or torn: refetch */
  }

  char act_abs[PATH_MAX];
  char act_rel[k_mdl_relpath_max];
  (void)snprintf(act_abs, sizeof(act_abs), "%s", target_abs);
  (void)snprintf(act_rel, sizeof(act_rel), "%s", target_rel);

  const char* held_dot = strrchr(held->rel_path, '.');
  const char* tab_dot  = strrchr(act_abs, '.');
  const char* tre_dot  = strrchr(act_rel, '.');
  if ((held_dot != nullptr) && (tab_dot != nullptr) && (tre_dot != nullptr)) {
    const char* held_ext = held_dot + 1;
    if (strcmp(tab_dot + 1, held_ext) != 0) {
      (void)snprintf(act_abs,
                     sizeof(act_abs),
                     "%.*s.%s",
                     (int)(tab_dot - target_abs),
                     target_abs,
                     held_ext);
      (void)snprintf(act_rel,
                     sizeof(act_rel),
                     "%.*s.%s",
                     (int)(tre_dot - target_rel),
                     target_rel,
                     held_ext);
    }
  }

  if (strcmp(held->rel_path, act_rel) == 0) {
    return true; /* already in place (same-chapter resume) */
  }
  if (!copy_file(src_abs, act_abs)) {
    return false;
  }
  return mdl_state_add_page(ctx->state,
                            url_hash,
                            held->content_hash,
                            act_rel,
                            held->etag,
                            held->last_modified);
}

/** @brief Governor host key for `url`, or NULL when it cannot be parsed. */
RA8_INTERNAL static const char* page_host(const char* url, char* buf, size_t cap)
{
  return mdl_url_host(url, buf, cap) ? buf : nullptr;
}

/** @brief One governed image transfer: pace, fetch, feed the outcome back. */
RA8_INTERNAL static ra8_err_t governed_get_file(mdl_fetch_ctx_t*     ctx,
                                                const char*          host,
                                                const char*          url,
                                                const mdl_net_req_t* req,
                                                const char*          target_abs,
                                                uint32_t             jmin,
                                                uint32_t             jmax,
                                                mdl_net_resp_t*      out_resp,
                                                size_t*              out_bytes)
{
  if (mdl_governor_acquire(ctx->gov, host, jmin, jmax) == k_ra8_err_would_block) {
    return k_ra8_err_would_block;
  }
  size_t          got  = 0U;
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc   = mdl_net_get_file(ctx->session->net, url, req, target_abs, &got, &resp);
  mdl_governor_observe(ctx->gov, host, resp.status, resp.retry_after);
  mdl_governor_release(ctx->gov, host);
  if (out_resp != nullptr) {
    *out_resp = resp;
  }
  if (out_bytes != nullptr) {
    *out_bytes = got;
  }
  return rc;
}

/** @brief Bounded, governed retry of one image transfer; last status latched. */
RA8_INTERNAL static ra8_err_t fetch_with_retry(mdl_fetch_ctx_t*     ctx,
                                               const char*          host,
                                               const char*          url,
                                               const mdl_net_req_t* req,
                                               const char*          target_abs,
                                               uint32_t             jmin,
                                               uint32_t             jmax,
                                               mdl_net_resp_t*      out_resp,
                                               size_t*              out_bytes)
{
  ra8_err_t rc = k_ra8_fail;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    rc = governed_get_file(ctx, host, url, req, target_abs, jmin, jmax, out_resp, out_bytes);
    if (!mdl_fetch_is_retryable(rc)) {
      break; /* success, or a permanent failure not worth retrying */
    }
  }
  return rc;
}

/** @brief Gate, govern (with backoff retry), fetch, hash and record one page. */
RA8_INTERNAL static bool page_exists_abs(const char* path)
{
  struct stat st;
  return (path != nullptr) && (stat(path, &st) == 0);
}

RA8_INTERNAL static ra8_err_t do_fetch_page(mdl_fetch_ctx_t* ctx,
                                            const char*      chapter_url,
                                            const char*      url,
                                            const char*      target_abs,
                                            const char*      target_rel,
                                            size_t*          out_bytes,
                                            mdl_net_resp_t*  out_resp)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, url, &crawl)) {
    record_fail(ctx, url, 0, k_ra8_fail); /* robots refused (message printed) */
    return k_ra8_fail;
  }
  const mdl_page_rec_t* held =
    ctx->refetch ? nullptr : mdl_state_find_page(ctx->state, mdl_hash_str(url));
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host = page_host(url, hostbuf, sizeof(hostbuf));
  const uint32_t      jmin = max_u32(ctx->site->img_delay_min, crawl);
  const uint32_t      jmax = max_u32(ctx->site->img_delay_max, crawl);
  const mdl_net_req_t req  = {
    .user_agent    = ctx->session->user_agent,
    .referer       = chapter_url,
    .if_none_match = (held != nullptr && held->etag[0] != '\0') ? held->etag : nullptr,
    .if_modified_since =
      (held != nullptr && held->last_modified[0] != '\0') ? held->last_modified : nullptr,
    .timeout_ms = ctx->timeout_ms,
  };
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc =
    fetch_with_retry(ctx, host, url, &req, target_abs, jmin, jmax, &resp, out_bytes);
  if (out_resp != nullptr) {
    *out_resp = resp;
  }
  if (rc != k_ra8_ok) {
    record_fail(ctx, url, resp.status, rc);
    return k_ra8_fail;
  }
  if (resp.status == 304) {
    if (!page_exists_abs(target_abs) && (held != nullptr)) {
      char src_abs[PATH_MAX];
      if (snprintf(src_abs, sizeof(src_abs), "%s/%s", ctx->series_abs_dir, held->rel_path) > 0) {
        (void)copy_file(src_abs, target_abs);
      }
    }
  }

  char act_abs[PATH_MAX];
  char act_rel[k_mdl_relpath_max];
  (void)snprintf(act_abs, sizeof(act_abs), "%s", target_abs);
  (void)snprintf(act_rel, sizeof(act_rel), "%s", target_rel);

  char true_ext[8];
  if (mdl_urlname_sniff_file(act_abs, resp.content_type, true_ext, sizeof(true_ext), nullptr, 0)) {
    const char* dot_abs = strrchr(act_abs, '.');
    const char* dot_rel = strrchr(act_rel, '.');
    if ((dot_abs != nullptr) && (dot_rel != nullptr)) {
      if (strcmp(dot_abs + 1, true_ext) != 0) {
        char         new_abs[PATH_MAX];
        char         new_rel[k_mdl_relpath_max];
        const size_t pabs = (size_t)(dot_abs - act_abs);
        const size_t prel = (size_t)(dot_rel - act_rel);
        const int    abs_len =
          snprintf(new_abs, sizeof(new_abs), "%.*s.%s", (int)pabs, act_abs, true_ext);
        const int rel_len =
          snprintf(new_rel, sizeof(new_rel), "%.*s.%s", (int)prel, act_rel, true_ext);
        if ((abs_len <= 0) || ((size_t)abs_len >= sizeof(new_abs)) || (rel_len <= 0) ||
            ((size_t)rel_len >= sizeof(new_rel)) || (rename(act_abs, new_abs) != 0)) {
          record_fail(ctx, url, resp.status, k_ra8_fail);
          return k_ra8_fail;
        }
        (void)snprintf(act_abs, sizeof(act_abs), "%s", new_abs);
        (void)snprintf(act_rel, sizeof(act_rel), "%s", new_rel);
      }
    }
  }

  uint64_t content = 0U;
  if (mdl_hash_file(act_abs, &content) != k_ra8_ok) {
    record_fail(ctx, url, resp.status, k_ra8_fail);
    return k_ra8_fail;
  }
  const char* etag_to_save =
    (resp.etag[0] != '\0') ? resp.etag : ((held != nullptr) ? held->etag : "");
  const char* lastmod_to_save = (resp.last_modified[0] != '\0')
                                  ? resp.last_modified
                                  : ((held != nullptr) ? held->last_modified : "");
  if (!mdl_state_add_page(ctx->state,
                          mdl_hash_str(url),
                          content,
                          act_rel,
                          etag_to_save,
                          lastmod_to_save)) {
    record_fail(ctx, url, resp.status, k_ra8_err_no_mem);
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/** @brief Reuse or fetch page @p idx into @p dest_abs; update stats + checkpoint. */
RA8_INTERNAL static ra8_err_t fetch_one_page(mdl_fetch_ctx_t*    ctx,
                                             const char*         chapter_url,
                                             const char*         dest_abs,
                                             const char*         dest_rel,
                                             size_t              page_no,
                                             size_t              idx,
                                             mdl_chapter_rec_t*  rec,
                                             mdl_fetch_stats_t*  stats,
                                             mdl_page_outcome_t* out)
{
  const char* url = ctx->images->urls[idx];
  char        leaf[k_fetch_leaf_bytes];
  if (!page_leaf(page_no, url, leaf, sizeof(leaf))) {
    return k_ra8_fail;
  }
  char      target_abs[PATH_MAX];
  char      target_rel[k_mdl_relpath_max];
  const int an = snprintf(target_abs, sizeof(target_abs), "%s/%s", dest_abs, leaf);
  const int rn = snprintf(target_rel, sizeof(target_rel), "%s/%s", dest_rel, leaf);
  if ((an < 0) || ((size_t)an >= sizeof(target_abs)) || (rn < 0) ||
      ((size_t)rn >= sizeof(target_rel))) {
    return k_ra8_fail;
  }
  const uint64_t        url_hash = mdl_hash_str(url);
  const mdl_page_rec_t* held     = mdl_state_find_page(ctx->state, url_hash);
  const bool            has_validator =
    (held != nullptr) && ((held->etag[0] != '\0') || (held->last_modified[0] != '\0'));
  if (!ctx->refetch && !has_validator && try_reuse(ctx, url_hash, target_abs, target_rel)) {
    stats->pages_reused += 1U;
    *out = (mdl_page_outcome_t){.bytes = 0U, .elapsed_ms = 0U, .reused = true};
  } else {
    const int64_t   t0   = mono_ms(ctx);
    size_t          got  = 0U;
    mdl_net_resp_t  resp = {};
    const ra8_err_t rc = do_fetch_page(ctx, chapter_url, url, target_abs, target_rel, &got, &resp);
    if (rc != k_ra8_ok) {
      stats->pages_failed += 1U;
      return rc;
    }
    if (resp.status == 304) {
      stats->pages_reused += 1U;
      *out = (mdl_page_outcome_t){.bytes      = 0U,
                                  .elapsed_ms = (uint32_t)(mono_ms(ctx) - t0),
                                  .reused     = true};
    } else {
      stats->pages_fetched += 1U;
      *out = (mdl_page_outcome_t){.bytes      = (uint64_t)got,
                                  .elapsed_ms = (uint32_t)(mono_ms(ctx) - t0),
                                  .reused     = false};
    }
  }
  rec->pages_done = (uint16_t)(idx + 1U);
  return checkpoint(ctx);
}

/** @brief Fetch every page of one chapter; fail (partial) on the first bad page. */
RA8_INTERNAL static ra8_err_t fetch_chapter_pages(mdl_fetch_ctx_t*     ctx,
                                                  const char*          chapter_url,
                                                  const char*          dest_abs,
                                                  const char*          dest_rel,
                                                  size_t               base,
                                                  mdl_chapter_rec_t*   rec,
                                                  mdl_fetch_stats_t*   stats,
                                                  const mdl_run_pos_t* pos)
{
  for (size_t i = 0U; i < ctx->images->count; ++i) {
    const size_t       page_no = base + i + 1U;
    mdl_page_outcome_t out     = {};
    const ra8_err_t    rc =
      fetch_one_page(ctx, chapter_url, dest_abs, dest_rel, page_no, i, rec, stats, &out);
    if (rc != k_ra8_ok) {
      return rc;
    }
    emit_progress(ctx, pos, i + 1U, &out);
  }
  return k_ra8_ok;
}

/** @brief One governed chapter-HTML fetch: pace, GET, feed the outcome back. */
RA8_INTERNAL static ra8_err_t governed_get_buf(mdl_fetch_ctx_t*     ctx,
                                               const char*          host,
                                               const char*          url,
                                               const mdl_net_req_t* req,
                                               uint32_t             jmin,
                                               uint32_t             jmax,
                                               size_t*              out_len,
                                               long*                out_status)
{
  if (mdl_governor_acquire(ctx->gov, host, jmin, jmax) == k_ra8_err_would_block) {
    return k_ra8_err_would_block;
  }
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc =
    mdl_net_get_buf(ctx->session->net, url, req, ctx->page_buf, ctx->page_cap, out_len, &resp);
  mdl_governor_observe(ctx->gov, host, resp.status, resp.retry_after);
  mdl_governor_release(ctx->gov, host);
  if (out_status != nullptr) {
    *out_status = resp.status;
  }
  return rc;
}

/** @brief Robots-gate, govern (with backoff retry), fetch and extract page URLs. */
RA8_INTERNAL static ra8_err_t fetch_chapter_html(mdl_fetch_ctx_t* ctx, const char* chapter_url)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, chapter_url, &crawl)) {
    record_fail(ctx, chapter_url, 0, k_ra8_fail);
    return k_ra8_fail;
  }
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host   = page_host(chapter_url, hostbuf, sizeof(hostbuf));
  const uint32_t      jmin   = max_u32(ctx->site->chapter_delay_min, crawl);
  const uint32_t      jmax   = max_u32(ctx->site->chapter_delay_max, crawl);
  const mdl_net_req_t req    = {.user_agent = ctx->session->user_agent,
                                .referer    = ctx->series_url,
                                .timeout_ms = ctx->timeout_ms};
  ra8_err_t           rc     = k_ra8_fail;
  size_t              len    = 0U;
  long                status = 0;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    rc = governed_get_buf(ctx, host, chapter_url, &req, jmin, jmax, &len, &status);
    if (!mdl_fetch_is_retryable(rc)) {
      break; /* success, or a permanent failure not worth retrying */
    }
  }
  if (rc != k_ra8_ok) {
    record_fail(ctx, chapter_url, status, rc);
    return k_ra8_fail;
  }
  const ra8_err_t erc = mdl_extract_images(ctx->page_buf,
                                           len,
                                           chapter_url,
                                           ctx->site->page_img_attr,
                                           ctx->site->page_img_url_contains,
                                           ctx->images);
  if (erc != k_ra8_ok) {
    record_fail(ctx, chapter_url, status, erc);
    return erc;
  }
  if (ctx->images->count == 0U) {
    record_fail(ctx, chapter_url, status, k_ra8_err_no_data);
    return k_ra8_err_no_data;
  }
  return k_ra8_ok;
}

/** @brief Resolve the output directory + starting page number for one chapter. */
RA8_INTERNAL static bool resolve_dest(mdl_fetch_ctx_t*   ctx,
                                      mdl_fetch_layout_t layout,
                                      const char*        id,
                                      const char*        combined_abs,
                                      const char*        combined_rel,
                                      size_t             global_no,
                                      char*              chap_abs,
                                      size_t             chap_cap,
                                      const char**       dest_abs,
                                      const char**       dest_rel,
                                      size_t*            base)
{
  if (layout == k_mdl_layout_combined) {
    *dest_abs = combined_abs;
    *dest_rel = combined_rel;
    *base     = global_no;
    return true;
  }
  if (!mdl_join_dir_under(ctx->series_abs_dir, id, chap_abs, chap_cap)) {
    return false;
  }
  *dest_abs = chap_abs;
  *dest_rel = id;
  *base     = 0U;
  return true;
}

/** @brief Mark a chapter complete and advance combined numbering. */
RA8_INTERNAL static void mark_complete(mdl_fetch_ctx_t*   ctx,
                                       mdl_fetch_layout_t layout,
                                       mdl_chapter_rec_t* rec,
                                       size_t*            global_no)
{
  rec->pages_done = (uint16_t)ctx->images->count;
  rec->complete   = true;
  rec->fetched_at = (int64_t)time(nullptr);
  if (layout == k_mdl_layout_combined) {
    *global_no += ctx->images->count;
  }
}

/** @brief Download one chapter (or skip it), returning its outcome. */
RA8_INTERNAL static mdl_chap_status_t process_chapter(mdl_fetch_ctx_t*   ctx,
                                                      const char*        chapter_url,
                                                      mdl_fetch_layout_t layout,
                                                      const char*        combined_abs,
                                                      const char*        combined_rel,
                                                      size_t*            global_no,
                                                      mdl_fetch_stats_t* stats,
                                                      size_t             chapter_index,
                                                      size_t             chapter_total)
{
  char id[k_mdl_chapter_id_max];
  mdl_urlname_last_segment(chapter_url, id, sizeof(id));
  mdl_chapter_rec_t* rec =
    mdl_state_add_chapter(ctx->state, id, chapter_url, mdl_urlname_chapter_number(chapter_url));
  if (rec == nullptr) {
    return k_ch_failed; /* chapter table full */
  }
  if (ctx->update_only && rec->complete) {
    *global_no += rec->page_count;
    return k_ch_skipped;
  }
  rec->complete   = false;
  rec->fetched_at = 0;
  if (fetch_chapter_html(ctx, chapter_url) != k_ra8_ok) {
    return k_ch_failed;
  }
  rec->page_count = (uint16_t)ctx->images->count;
  if (rec->pages_done > rec->page_count) {
    rec->pages_done = 0U;
  }
  char        chap_abs[PATH_MAX];
  const char* dest_abs = nullptr;
  const char* dest_rel = nullptr;
  size_t      base     = 0U;
  if (!resolve_dest(ctx,
                    layout,
                    id,
                    combined_abs,
                    combined_rel,
                    *global_no,
                    chap_abs,
                    sizeof(chap_abs),
                    &dest_abs,
                    &dest_rel,
                    &base)) {
    return k_ch_failed;
  }
  const mdl_run_pos_t pos = {.chapter_index = chapter_index,
                             .chapter_total = chapter_total,
                             .chapter_id    = id};
  if (fetch_chapter_pages(ctx, chapter_url, dest_abs, dest_rel, base, rec, stats, &pos) !=
      k_ra8_ok) {
    (void)checkpoint(ctx);
    return k_ch_failed;
  }
  mark_complete(ctx, layout, rec, global_no);
  if (checkpoint(ctx) != k_ra8_ok) {
    return k_ch_failed;
  }
  return k_ch_completed;
}

/** @brief Fold one chapter's outcome into the run stats; true when it failed. */
RA8_INTERNAL static bool tally(mdl_chap_status_t s, mdl_fetch_stats_t* stats)
{
  if (s == k_ch_completed) {
    stats->chapters_completed += 1U;
  } else if (s == k_ch_skipped) {
    stats->chapters_skipped += 1U;
  } else {
    stats->chapters_failed += 1U;
  }
  return s == k_ch_failed;
}

/** @brief Validate that every required ::mdl_fetch_ctx_t dependency is present. */
RA8_INTERNAL static bool ctx_ready(const mdl_fetch_ctx_t* ctx)
{
  return (ctx->session != nullptr) && (ctx->state != nullptr) && (ctx->series_abs_dir != nullptr) &&
         (ctx->series_url != nullptr) && (ctx->site != nullptr) && (ctx->page_buf != nullptr) &&
         (ctx->images != nullptr);
}

ra8_err_t mdl_fetch_run(mdl_fetch_ctx_t*      ctx,
                        const mdl_url_list_t* chapters,
                        mdl_fetch_layout_t    layout,
                        const char*           combined_dir_rel,
                        mdl_fetch_stats_t*    stats)
{
  if ((ctx == nullptr) || (chapters == nullptr) || (stats == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!ctx_ready(ctx)) {
    return k_ra8_err_invalid_arg;
  }
  memset(stats, 0, sizeof(*stats));

  char combined_abs[PATH_MAX];
  if (layout == k_mdl_layout_combined) {
    if ((combined_dir_rel == nullptr) || !mdl_join_dir_under(ctx->series_abs_dir,
                                                             combined_dir_rel,
                                                             combined_abs,
                                                             sizeof(combined_abs))) {
      return k_ra8_err_invalid_arg;
    }
  } else {
    combined_abs[0] = '\0';
  }

  size_t global_no = 0U;
  bool   any_fail  = false;
  for (size_t i = 0U; i < chapters->count; ++i) {
    const mdl_chap_status_t s = process_chapter(ctx,
                                                chapters->urls[i],
                                                layout,
                                                combined_abs,
                                                combined_dir_rel,
                                                &global_no,
                                                stats,
                                                i + 1U,
                                                chapters->count);
    if (tally(s, stats)) {
      any_fail = true;
      if (layout == k_mdl_layout_combined) {
        break; /* the combined archive cannot be completed past a gap */
      }
    }
  }
  return any_fail ? k_ra8_fail : k_ra8_ok;
}
