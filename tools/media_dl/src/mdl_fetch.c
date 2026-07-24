/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_fetch.c
 * @brief State-aware, resumable, deduping chapter/page download loop.
 */
#include "mdl_fetch.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mdl_hash.h"
#include "mdl_net.h"
#include "mdl_pathfs.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "ra8_attributes.h"

/** @brief Local buffer sizes and the file-copy chunk. */
typedef enum : uint16_t {
  k_fetch_leaf_bytes = 32,   /**< "page_NNNN.ext" leaf name buffer. */
  k_fetch_copy_chunk = 8192, /**< Reuse-copy stream chunk bytes.    */
} mdl_fetch_size_t;

/** @brief Loop bound on the reuse-copy stream (any real page is far under). */
typedef enum : uint32_t {
  k_fetch_copy_max_chunks = 1000000U, /**< 8 GiB ceiling. */
} mdl_fetch_bound_t;

/** @brief Bounded attempts per request: the initial try plus backoff retries. */
typedef enum : uint8_t {
  k_fetch_max_attempts = 4U, /**< 1 initial + up to 3 governed retries on a 429/503. */
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

/** @brief Persist state atomically when a checkpoint target is configured. */
RA8_INTERNAL static void checkpoint(const mdl_fetch_ctx_t* ctx)
{
  if (ctx->state_path != nullptr) {
    (void)mdl_state_save(ctx->state_path, ctx->state);
  }
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
  FILE* out = fopen(dst, "wb");
  if (out == nullptr) {
    (void)fclose(in);
    return false;
  }
  bool    ok = true;
  uint8_t buf[k_fetch_copy_chunk];
  for (uint32_t chunk = 0U; chunk < (uint32_t)k_fetch_copy_max_chunks; ++chunk) {
    const size_t n = fread(buf, 1U, sizeof(buf), in);
    if ((n > 0U) && (fwrite(buf, 1U, n, out) != n)) {
      ok = false;
      break;
    }
    if (n < sizeof(buf)) {
      ok = (ferror(in) == 0);
      break;
    }
  }
  ok = ((fclose(out) == 0) && ok);
  (void)fclose(in);
  if (!ok) {
    (void)remove(dst);
  }
  return ok;
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
  if (strcmp(held->rel_path, target_rel) == 0) {
    return true; /* already in place (same-chapter resume) */
  }
  if (!copy_file(src_abs, target_abs)) {
    return false;
  }
  (void)mdl_state_add_page(ctx->state, url_hash, held->content_hash, target_rel);
  return true;
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
                                                uint32_t             jmax)
{
  if (mdl_governor_acquire(ctx->gov, host, jmin, jmax) == k_ra8_err_would_block) {
    return k_ra8_err_would_block;
  }
  size_t          got  = 0U;
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc   = mdl_net_get_file(ctx->session->net, url, req, target_abs, &got, &resp);
  mdl_governor_observe(ctx->gov, host, resp.status, resp.retry_after);
  mdl_governor_release(ctx->gov, host);
  return rc;
}

/** @brief Gate, govern (with backoff retry), fetch, hash and record one page. */
RA8_INTERNAL static ra8_err_t do_fetch_page(mdl_fetch_ctx_t* ctx,
                                            const char*      chapter_url,
                                            const char*      url,
                                            const char*      target_abs,
                                            const char*      target_rel)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, url, &crawl)) {
    return k_ra8_fail; /* robots refused (message printed by the session) */
  }
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host = page_host(url, hostbuf, sizeof(hostbuf));
  const uint32_t      jmin = max_u32(ctx->site->img_delay_min, crawl);
  const uint32_t      jmax = max_u32(ctx->site->img_delay_max, crawl);
  const mdl_net_req_t req  = {.user_agent = ctx->session->user_agent,
                              .referer    = chapter_url,
                              .timeout_ms = ctx->timeout_ms};
  ra8_err_t           rc   = k_ra8_fail;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    rc = governed_get_file(ctx, host, url, &req, target_abs, jmin, jmax);
    if (rc != k_ra8_err_busy) {
      break; /* success, or a non-throttle failure not worth retrying */
    }
  }
  if (rc != k_ra8_ok) {
    return k_ra8_fail;
  }
  uint64_t content = 0U;
  if (mdl_hash_file(target_abs, &content) != k_ra8_ok) {
    return k_ra8_fail;
  }
  (void)mdl_state_add_page(ctx->state, mdl_hash_str(url), content, target_rel);
  return k_ra8_ok;
}

/** @brief Reuse or fetch page @p idx into @p dest_abs; update stats + checkpoint. */
RA8_INTERNAL static ra8_err_t fetch_one_page(mdl_fetch_ctx_t*   ctx,
                                             const char*        chapter_url,
                                             const char*        dest_abs,
                                             const char*        dest_rel,
                                             size_t             page_no,
                                             size_t             idx,
                                             mdl_chapter_rec_t* rec,
                                             mdl_fetch_stats_t* stats)
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
  if (try_reuse(ctx, mdl_hash_str(url), target_abs, target_rel)) {
    stats->pages_reused += 1U;
  } else {
    const ra8_err_t rc = do_fetch_page(ctx, chapter_url, url, target_abs, target_rel);
    if (rc != k_ra8_ok) {
      stats->pages_failed += 1U;
      return rc;
    }
    stats->pages_fetched += 1U;
  }
  rec->pages_done = (uint16_t)(idx + 1U);
  checkpoint(ctx);
  return k_ra8_ok;
}

/** @brief Fetch every page of one chapter; fail (partial) on the first bad page. */
RA8_INTERNAL static ra8_err_t fetch_chapter_pages(mdl_fetch_ctx_t*   ctx,
                                                  const char*        chapter_url,
                                                  const char*        dest_abs,
                                                  const char*        dest_rel,
                                                  size_t             base,
                                                  mdl_chapter_rec_t* rec,
                                                  mdl_fetch_stats_t* stats)
{
  for (size_t i = 0U; i < ctx->images->count; ++i) {
    const size_t    page_no = base + i + 1U;
    const ra8_err_t rc =
      fetch_one_page(ctx, chapter_url, dest_abs, dest_rel, page_no, i, rec, stats);
    if (rc != k_ra8_ok) {
      return rc;
    }
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
                                               size_t*              out_len)
{
  if (mdl_governor_acquire(ctx->gov, host, jmin, jmax) == k_ra8_err_would_block) {
    return k_ra8_err_would_block;
  }
  mdl_net_resp_t  resp = {};
  const ra8_err_t rc =
    mdl_net_get_buf(ctx->session->net, url, req, ctx->page_buf, ctx->page_cap, out_len, &resp);
  mdl_governor_observe(ctx->gov, host, resp.status, resp.retry_after);
  mdl_governor_release(ctx->gov, host);
  return rc;
}

/** @brief Robots-gate, govern (with backoff retry), fetch and extract page URLs. */
RA8_INTERNAL static ra8_err_t fetch_chapter_html(mdl_fetch_ctx_t* ctx, const char* chapter_url)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(ctx->session, chapter_url, &crawl)) {
    return k_ra8_fail;
  }
  char                hostbuf[k_mdl_gov_host_max];
  const char*         host = page_host(chapter_url, hostbuf, sizeof(hostbuf));
  const uint32_t      jmin = max_u32(ctx->site->chapter_delay_min, crawl);
  const uint32_t      jmax = max_u32(ctx->site->chapter_delay_max, crawl);
  const mdl_net_req_t req  = {.user_agent = ctx->session->user_agent,
                              .referer    = ctx->series_url,
                              .timeout_ms = ctx->timeout_ms};
  ra8_err_t           rc   = k_ra8_fail;
  size_t              len  = 0U;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_fetch_max_attempts; ++attempt) {
    rc = governed_get_buf(ctx, host, chapter_url, &req, jmin, jmax, &len);
    if (rc != k_ra8_err_busy) {
      break; /* success, or a non-throttle failure not worth retrying */
    }
  }
  if (rc != k_ra8_ok) {
    return k_ra8_fail;
  }
  const ra8_err_t erc = mdl_extract_images(ctx->page_buf,
                                           len,
                                           chapter_url,
                                           ctx->site->page_img_attr,
                                           ctx->site->page_img_url_contains,
                                           ctx->images);
  return ((erc == k_ra8_ok) || (erc == k_ra8_err_no_mem)) ? k_ra8_ok : erc;
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
  rec->page_count = (uint16_t)ctx->images->count;
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
                                                      mdl_fetch_stats_t* stats)
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
  if (fetch_chapter_html(ctx, chapter_url) != k_ra8_ok) {
    return k_ch_failed;
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
  if (fetch_chapter_pages(ctx, chapter_url, dest_abs, dest_rel, base, rec, stats) != k_ra8_ok) {
    checkpoint(ctx);
    return k_ch_failed;
  }
  mark_complete(ctx, layout, rec, global_no);
  checkpoint(ctx);
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
                                                stats);
    if (tally(s, stats)) {
      any_fail = true;
      if (layout == k_mdl_layout_combined) {
        break; /* the combined archive cannot be completed past a gap */
      }
    }
  }
  return any_fail ? k_ra8_fail : k_ra8_ok;
}
