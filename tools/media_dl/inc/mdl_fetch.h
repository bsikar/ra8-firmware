/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_fetch.h
 * @brief State-aware chapter/page download orchestrator for the media
 *        downloader.
 *
 * @details
 * The incremental, resumable download loop, lifted out of `main.c` so it can be
 * driven end to end through the `mdl_net` vtable mock with no network: the host
 * tests script a fake backend and assert that a first run fetches N pages, a
 * second run fetches only what is new, and an interrupted run resumes to a
 * byte-identical result. Everything the loop needs is injected through
 * ::mdl_fetch_ctx_t -- the network seam (via the session), the persistent state
 * store, the politeness governor -- so nothing here is host-only.
 *
 * What it does that the old index-based loop did not:
 *   - **Addresses chapters by identity, not position.** Each chapter's stable id
 *     (::mdl_urlname_last_segment) keys a record in ::mdl_state_t, so a rerun
 *     recognises what it already has even as the site adds or reorders chapters.
 *   - **Resumes.** A chapter is marked complete only after every page is fetched
 *     AND its bytes verify against the recorded content hash; an interrupted
 *     chapter is resumed page-wise on the next run, never packaged half-done.
 *   - **Dedups by content.** A page whose source URL is already held (a rerun,
 *     or an image shared across chapters) is reused from the existing verified
 *     file instead of re-fetched.
 *   - **Numbers combined pages from recorded counts,** so a resumed combined
 *     download reproduces the numbering an uninterrupted one would have.
 *
 * The loop only puts page bytes on disk and maintains state; packaging (archive
 * export) is left to the caller, which reads the resulting directories.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_politeness.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "ra8_err.h"

/** @brief Output directory layout the orchestrator writes. */
typedef enum : uint8_t {
  k_mdl_layout_combined = 0, /**< All chapters -> one dir, continuous numbering.  */
  k_mdl_layout_separate = 1, /**< Each chapter -> own dir, per-chapter numbering. */
} mdl_fetch_layout_t;

/**
 * @struct mdl_fetch_stats_t
 * @brief Tallies a fetch run reports back (all zero-initialised by the run).
 * @details These are what the host tests assert on: `pages_fetched` counts real
 *          network transfers, `pages_reused` counts dedup/resume hits served
 *          from disk, so "second run fetches only the new ones" is a direct
 *          numeric check.
 * @since 0.1.0
 */
typedef struct {
  size_t chapters_completed; /**< Chapters finished (all pages present) this run.  */
  size_t chapters_skipped;   /**< Chapters skipped as already complete (--update). */
  size_t chapters_failed;    /**< Chapters left partial by a page failure.         */
  size_t pages_fetched;      /**< Pages transferred over the network.              */
  size_t pages_reused;       /**< Pages served from an already-held verified file. */
  size_t pages_failed;       /**< Page fetches that failed or robots refused.      */
} mdl_fetch_stats_t;

/** @brief Bounded run-failure log capacity (zero dynamic allocation). */
typedef enum : uint16_t {
  k_mdl_fetch_fail_max = 256, /**< Failures stored before the log saturates. */
} mdl_fetch_faillog_size_t;

/**
 * @struct mdl_fetch_fail_t
 * @brief One recorded page/chapter failure: what failed, and with what status.
 * @details Captured by the run so an end-of-run summary can name every lost
 *          page long after its one-line diagnostic has scrolled past. The raw
 *          ::ra8_err_t is kept (not a prose string) so the caller renders a
 *          human-readable reason at print time via ::mdl_fetch_reason.
 * @invariant `status == 0` when no HTTP status was observed (transport error or
 *            a robots refusal before any request).
 * @since 0.1.0
 */
typedef struct {
  char      url[k_mdl_url_max]; /**< Failing page or chapter URL (truncation-safe). */
  long      status;             /**< HTTP status observed, 0 when none.             */
  ra8_err_t err;                /**< Classified failure code (rendered on demand).  */
} mdl_fetch_fail_t;

/**
 * @struct mdl_fetch_faillog_t
 * @brief Bounded record of every failure in a run (declare at file scope).
 * @details About 135 KiB (a fixed ::mdl_fetch_fail_t table), so it lives in
 *          `.bss` beside the other large run buffers. The caller clears it
 *          before a run; the loop only appends. `count` is what is stored (at
 *          most ::k_mdl_fetch_fail_max), `total` is what was observed, so a
 *          saturated log still reports the true failure count.
 * @invariant `count <= k_mdl_fetch_fail_max` and `count <= total`.
 * @see mdl_fetch_run
 * @since 0.1.0
 */
typedef struct {
  mdl_fetch_fail_t items[k_mdl_fetch_fail_max]; /**< Stored failures.               */
  size_t           count;                       /**< Stored (<= capacity).          */
  size_t           total;                       /**< Observed (may exceed `count`). */
} mdl_fetch_faillog_t;

/**
 * @struct mdl_fetch_progress_t
 * @brief One per-page progress event the run emits through ::mdl_progress_fn.
 * @details Carries everything a redirect-safe progress line needs: where we are
 *          in the run (chapter index/total), where we are in the chapter (page
 *          index/total), and how the page went (bytes and wall time, or a reuse
 *          flag). Rate is bytes over `elapsed_ms`; the sink formats it.
 * @invariant `page_bytes == 0` and `elapsed_ms == 0` when `reused` is true.
 * @see mdl_progress_fn
 * @since 0.1.0
 */
typedef struct {
  size_t      chapter_index; /**< 1-based chapter position in the run.        */
  size_t      chapter_total; /**< Total chapters in the run.                  */
  const char* chapter_id;    /**< Chapter identifier (URL leaf).              */
  size_t      page_index;    /**< 1-based page position within the chapter.   */
  size_t      page_total;    /**< Pages in the chapter.                       */
  uint64_t    page_bytes;    /**< Bytes transferred (0 when reused).          */
  uint32_t    elapsed_ms;    /**< Wall time for the transfer (0 when reused). */
  bool        reused;        /**< Page served from an already-held file.      */
} mdl_fetch_progress_t;

/**
 * @brief Injected per-page progress sink; NULL disables all progress output.
 *
 * @details
 * The dependency-injection seam for run progress. Production wires a sink that
 * prints one redirect-safe line per page; the host tests leave it NULL so the
 * loop is silent and deterministic (and no wall clock is read). It is called
 * once per page after the page is present, fetched or reused.
 *
 * @param[in] ctx Opaque context supplied in ::mdl_fetch_ctx_t::progress_ctx.
 * @param[in] ev  The just-completed page's progress event (never NULL).
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*mdl_progress_fn)(void* ctx, const mdl_fetch_progress_t* ev);

/**
 * @struct mdl_fetch_ctx_t
 * @brief Everything ::mdl_fetch_run needs, injected for testability.
 * @details The network is reached only through @ref session (a ::mdl_net vtable
 *          behind it), and state is the injected ::mdl_state_t, so a test drives
 *          the whole loop against a scripted fake backend and a temp directory.
 * @invariant `page_buf`/`page_cap` describe a scratch buffer large enough for a
 *            chapter HTML page.
 * @invariant `series_abs_dir` is an existing, resolved absolute directory.
 * @see mdl_fetch_run
 * @since 0.1.0
 */
typedef struct {
  mdl_session_t*       session;        /**< Identity + robots + net backend.          */
  mdl_state_t*         state;          /**< Persistent state, read and updated.       */
  const char*          state_path;     /**< Atomic checkpoint target, or NULL.        */
  const char*          series_abs_dir; /**< Absolute series dir (paths resolve here). */
  const char*          series_url;     /**< Series URL, sent as the chapter Referer.  */
  const mdl_site_t*    site;           /**< Selectors + politeness bounds.            */
  mdl_governor_t*      gov;            /**< Per-host rate/backoff governor, or NULL.  */
  uint32_t             timeout_ms;     /**< Per-request time budget.                  */
  char*                page_buf;       /**< Chapter-HTML scratch (caller-owned).      */
  size_t               page_cap;       /**< Capacity of @ref page_buf.                */
  mdl_url_list_t*      images;         /**< Extracted-image scratch (caller-owned).   */
  bool                 update_only;    /**< Skip chapters already recorded complete.  */
  mdl_fetch_faillog_t* faillog;        /**< Caller-cleared failure log, or NULL.      */
  mdl_progress_fn      progress_fn;    /**< Per-page progress sink, or NULL (silent). */
  void*                progress_ctx;   /**< Context passed to @ref progress_fn.       */
} mdl_fetch_ctx_t;

/**
 * @brief Download a series' chapters incrementally, resuming and deduping.
 *
 * @details
 * Walks @p chapters in order. For each chapter it derives a stable identifier,
 * finds-or-adds its state record, and -- unless `update_only` is set and the
 * record is already complete -- fetches the chapter page, extracts its image
 * URLs, and writes each page under @p ctx->series_abs_dir. A page already held
 * (matched by source-URL hash and verified by content hash) is reused from disk
 * rather than re-fetched, including across chapters. Each request is retried up
 * to a small bounded number of times on a retryable outcome (a transport error,
 * timeout, 429 or 5xx), always paced through the politeness governor so a retry
 * never becomes an unpaced hammer; a 404 is never retried. A chapter is marked
 * complete and its record checkpointed only once every page is present and
 * verified; a page failure leaves the chapter partial for the next run to
 * resume, and is appended to @p ctx->faillog (when set) with its URL and status.
 * Per-page progress is emitted through @p ctx->progress_fn (when set). In
 * ::k_mdl_layout_combined the pages of all chapters share one directory with
 * numbering continued across chapters (derived from recorded per-chapter page
 * counts, so a resume reproduces it); ::k_mdl_layout_separate gives each chapter
 * its own directory numbered from one.
 *
 * @param[in,out] ctx              Injected dependencies and scratch (never NULL).
 * @param[in]     chapters         Live, ordered chapter URL list (never NULL).
 * @param[in]     layout           Output directory layout.
 * @param[in]     combined_dir_rel Directory name (relative to the series dir) for
 *                                 ::k_mdl_layout_combined; ignored otherwise.
 * @param[out]    stats            Receives the run tallies (never NULL).
 *
 * @return An ::ra8_err_t summarising the run.
 * @retval k_ra8_ok              Every attempted chapter completed.
 * @retval k_ra8_err_invalid_arg A required pointer argument was NULL.
 * @retval k_ra8_fail            At least one chapter was left partial/failed.
 *
 * @pre @p ctx, @p chapters, @p stats are non-NULL and @p ctx is fully populated.
 * @pre @p ctx->series_abs_dir exists and is an absolute resolved path.
 * @post @p stats reflects the run; `pages_fetched + pages_reused` accounts for
 *       every page of every completed chapter.
 * @post `ctx->state` is updated and, when `ctx->state_path` is set, checkpointed
 *       atomically after each page and each completed chapter.
 *
 * @note Not thread-safe: mutates shared state, scratch and the filesystem.
 * @see mdl_state_find_page
 * @since 0.1.0
 */
ra8_err_t mdl_fetch_run(mdl_fetch_ctx_t*      ctx,
                        const mdl_url_list_t* chapters,
                        mdl_fetch_layout_t    layout,
                        const char*           combined_dir_rel,
                        mdl_fetch_stats_t*    stats);
