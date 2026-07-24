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
  mdl_session_t*    session;        /**< Identity + robots + net backend.          */
  mdl_state_t*      state;          /**< Persistent state, read and updated.       */
  const char*       state_path;     /**< Atomic checkpoint target, or NULL.        */
  const char*       series_abs_dir; /**< Absolute series dir (paths resolve here). */
  const char*       series_url;     /**< Series URL, sent as the chapter Referer.  */
  const mdl_site_t* site;           /**< Selectors + politeness bounds.            */
  mdl_governor_t*   gov;            /**< Per-host rate/backoff governor, or NULL.  */
  uint32_t          timeout_ms;     /**< Per-request time budget.                  */
  char*             page_buf;       /**< Chapter-HTML scratch (caller-owned).      */
  size_t            page_cap;       /**< Capacity of @ref page_buf.                */
  mdl_url_list_t*   images;         /**< Extracted-image scratch (caller-owned).   */
  bool              update_only;    /**< Skip chapters already recorded complete.  */
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
 * rather than re-fetched, including across chapters. A chapter is marked complete
 * and its record checkpointed only once every page is present and verified; a
 * page failure leaves the chapter partial for the next run to resume. In
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
