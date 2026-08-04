/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_state.h
 * @brief Persistent per-series library state for the media downloader.
 *
 * @details
 * Without this store `media_dl` kept nothing between runs: resuming was an index
 * into a freshly scraped list (`--start K`), there was no "fetch only what is
 * new", a kill mid-chapter left an unrecorded partial directory, and two runs
 * (or two chapters) sharing an image re-downloaded it. This module is the on-disk
 * record that fixes all four: one small, versioned state file per series holding
 * the series identity, the site descriptor used, and -- per chapter -- the parsed
 * chapter identifier, source URL, page count, completion status and fetch time,
 * plus a series-wide pool of per-page content identities.
 *
 * Because chapters are keyed by a parsed identifier (::mdl_urlname_last_segment)
 * rather than list position, `--start` can mean "resume where we left off" and
 * `--update` can mean "only chapters we do not already have complete", both
 * stable as the site adds or reorders chapters. Because every page carries a
 * source-URL hash and a content hash, a re-run skips a byte-identical image
 * already held (::mdl_state_find_page) and a torn file is detected and refetched
 * rather than silently packaged.
 *
 * ### On-disk format (v1)
 * A flat, line-oriented, TAB-separated text file (`.mdl_state` in the series
 * directory), deliberately simple so it is human-readable and ports unchanged to
 * the RA8. A leading `#` marks a comment; blank lines are ignored. Each record is
 * a one-letter type followed by TAB-separated fields:
 *
 *     # media_dl library state v1
 *     V<TAB>1                                         schema version
 *     S<TAB><series-url>                              series URL
 *     T<TAB><series-title>                            series title
 *     N<TAB><site-name>                               site descriptor name
 *     H<TAB><site-host>                               site host
 *     G<TAB><config-path>                             descriptor file used
 *     C<TAB>id<TAB>num<TAB>done<TAB>pages<TAB>ready<TAB>epoch<TAB>url   one chapter
 *     P<TAB>url_hash_hex<TAB>content_hash_hex<TAB>rel_path              one page
 *
 * The file is written atomically (temp file + `rename`, ::mdl_state_save) so a
 * kill mid-write cannot corrupt it, and a file that fails to parse degrades to a
 * clear ::k_ra8_err_invalid_state so the caller can rebuild rather than crash or
 * silently refetch everything.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_config.h"
#include "mdl_extract.h"
#include "ra8_err.h"

/** @brief Fixed capacities and the schema version (zero dynamic allocation). */
typedef enum : uint16_t {
  k_mdl_state_version  = 1,   /**< On-disk schema version this build reads. */
  k_mdl_chapter_id_max = 128, /**< Chapter identifier bytes (sanitised).    */
  k_mdl_title_max      = 192, /**< Series title bytes.                      */
  k_mdl_relpath_max    = 200, /**< Page path relative to the series dir.    */
  k_mdl_cfgpath_max    = 512, /**< Site-descriptor path bytes.              */
  k_mdl_max_chapters   = 512, /**< Chapters tracked per series.             */
} mdl_state_limit_t;

/** @brief Page pool capacity (kept separate: it needs a 32-bit count). */
typedef enum : uint32_t {
  k_mdl_max_page_recs = 8192U, /**< Per-page content records per series. */
} mdl_state_pool_t;

/**
 * @struct mdl_chapter_rec_t
 * @brief One chapter's coverage in library state.
 * @details Keyed by @ref chapter_id (a parsed, sanitised identifier), NOT by
 *          position in a scraped list, so it stays valid as the site changes.
 * @invariant `pages_done <= page_count` once `page_count` is known (> 0).
 * @invariant `complete` is true only after every page was fetched and verified.
 * @see mdl_state_find_chapter
 * @since 0.1.0
 */
typedef struct {
  char     chapter_id[k_mdl_chapter_id_max]; /**< Stable identifier (URL leaf). */
  char     source_url[k_mdl_url_max];        /**< Chapter page URL.             */
  long     number;                           /**< Parsed chapter number, 0 = ?. */
  uint16_t page_count;                       /**< Total pages known (0 = ?).    */
  uint16_t pages_done;                       /**< Pages fetched and verified.   */
  bool     complete;                         /**< All pages present + verified. */
  int64_t  fetched_at;                       /**< Completion time (epoch s).    */
} mdl_chapter_rec_t;

/**
 * @struct mdl_page_rec_t
 * @brief One page's dedup/verify record in the series-wide pool.
 * @details @ref url_hash keys a source URL so a re-fetch of the same image (a
 *          rerun, or an image shared between chapters) is found without the
 *          network; @ref content_hash is the byte identity used to verify a page
 *          already on disk and to prove a reused copy is truly identical.
 * @invariant `rel_path` is a sanitised path relative to the series directory.
 * @see mdl_state_find_page
 * @since 0.1.0
 */
typedef struct {
  uint64_t url_hash;                    /**< FNV-1a 64 of the source URL.     */
  uint64_t content_hash;                /**< FNV-1a 64 of the fetched bytes.  */
  char     rel_path[k_mdl_relpath_max]; /**< Path under the series directory. */
} mdl_page_rec_t;

/**
 * @struct mdl_state_t
 * @brief One series' complete persistent state (declare at file scope).
 * @details About 2 MiB (it embeds the chapter table and page pool), so it lives
 *          in `.bss`, not on the stack, exactly like the session and page
 *          buffers. One instance serves one series at a time; library-wide
 *          commands reuse it series by series.
 * @invariant `chapter_count <= k_mdl_max_chapters`.
 * @invariant `page_rec_count <= k_mdl_max_page_recs`.
 * @see mdl_state_load
 * @see mdl_state_save
 * @since 0.1.0
 */
typedef struct {
  uint16_t          version;                        /**< Schema version.        */
  char              series_url[k_mdl_url_max];      /**< Series page URL.       */
  char              series_title[k_mdl_title_max];  /**< Series title.          */
  char              site_name[k_mdl_name_max];      /**< Descriptor name.       */
  char              site_host[k_mdl_host_max];      /**< Site host.             */
  char              config_path[k_mdl_cfgpath_max]; /**< Descriptor used.       */
  uint16_t          chapter_count;                  /**< Chapters recorded.     */
  uint32_t          page_rec_count;                 /**< Page records recorded. */
  mdl_chapter_rec_t chapters[k_mdl_max_chapters];   /**< Per-chapter coverage.  */
  mdl_page_rec_t    pages[k_mdl_max_page_recs];     /**< Per-page identities.   */
} mdl_state_t;

/**
 * @brief Reset a state object to an empty, current-version library.
 *
 * @param[out] st State to clear (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p st is non-NULL and addresses a full ::mdl_state_t.
 * @pre The caller owns @p st for the duration (not thread-safe).
 * @post `st->version == k_mdl_state_version` and all counts are 0.
 * @post Every metadata string is the empty string.
 *
 * @note Not thread-safe: initialises caller storage.
 * @since 0.1.0
 */
void mdl_state_init(mdl_state_t* st);

/**
 * @brief Record the series identity and the descriptor used.
 *
 * @param[in,out] st          State to update (never NULL).
 * @param[in]     url         Series page URL, or NULL to leave unchanged.
 * @param[in]     title       Series title, or NULL to leave unchanged.
 * @param[in]     site_name   Descriptor display name, or NULL.
 * @param[in]     site_host   Site host, or NULL.
 * @param[in]     config_path Descriptor file path used (for update-all), or NULL.
 *
 * @return Nothing.
 *
 * @pre @p st is non-NULL.
 * @pre Any non-NULL string argument is NUL-terminated.
 * @post Each non-NULL argument is copied (truncation-safe) into @p st.
 * @post Fields whose argument was NULL keep their previous value.
 *
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
void mdl_state_set_series(mdl_state_t* st,
                          const char*  url,
                          const char*  title,
                          const char*  site_name,
                          const char*  site_host,
                          const char*  config_path);

/**
 * @brief Load a series' state from disk.
 *
 * @details
 * Parses the v1 format (see the file header). A file that is absent is NOT an
 * error -- @p st is initialised empty and ::k_ra8_ok is returned, so the first
 * run of a new series just starts fresh. A file that exists but does not parse
 * (wrong version, malformed record, over-capacity) leaves @p st initialised
 * empty and returns ::k_ra8_err_invalid_state, giving the caller a clear rebuild
 * path rather than a crash or a silent full refetch.
 *
 * @param[in]  path State file path (never NULL).
 * @param[out] st   State to fill; always left in a valid (possibly empty) form.
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok               Loaded, or the file was absent (empty state).
 * @retval k_ra8_err_invalid_arg  @p path or @p st was NULL.
 * @retval k_ra8_err_invalid_state The file exists but is corrupt/unsupported.
 *
 * @pre @p path and @p st are non-NULL.
 * @pre The caller treats ::k_ra8_err_invalid_state as "rebuild", not "abort".
 * @post @p st is always a valid state object on return (empty on any error).
 * @post On ::k_ra8_err_invalid_state a diagnostic naming @p path is printed.
 *
 * @note Not thread-safe.
 * @see mdl_state_save
 * @since 0.1.0
 */
ra8_err_t mdl_state_load(const char* path, mdl_state_t* st);

/**
 * @brief Persist a series' state atomically.
 *
 * @details
 * Writes to `<path>.tmp` and then `rename`s it over @p path, so a process killed
 * mid-write can never leave a half-written (thus corrupt) state file -- the old
 * file survives intact until the complete new one replaces it in one step. This
 * is what makes per-page checkpointing safe: the fetch loop can save after every
 * page without risking the record it is protecting.
 *
 * @param[in] path State file path (never NULL).
 * @param[in] st   State to write (never NULL).
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok              Written and renamed into place.
 * @retval k_ra8_err_invalid_arg @p path or @p st was NULL.
 * @retval k_ra8_fail            The temp file could not be written or renamed.
 *
 * @pre @p path and @p st are non-NULL.
 * @pre @p path's directory exists and is writable.
 * @post On ::k_ra8_ok @p path holds the complete serialised @p st.
 * @post On failure @p path is left as it was (the temp file is removed).
 *
 * @note Not thread-safe.
 * @see mdl_state_load
 * @since 0.1.0
 */
ra8_err_t mdl_state_save(const char* path, const mdl_state_t* st);

/**
 * @brief Find a chapter record by its stable identifier.
 *
 * @param[in] st State to search (never NULL).
 * @param[in] id Chapter identifier (never NULL).
 *
 * @return The matching record, or NULL when none is recorded.
 * @retval NULL No chapter with @p id, or a NULL argument.
 *
 * @pre @p st and @p id are non-NULL; @p id is NUL-terminated.
 * @pre The caller must not retain the pointer across an add that may move data.
 * @post @p st is not modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
mdl_chapter_rec_t* mdl_state_find_chapter(mdl_state_t* st, const char* id);

/**
 * @brief Find or append a chapter record, returning it.
 *
 * @param[in,out] st     State to update (never NULL).
 * @param[in]     id     Chapter identifier (never NULL).
 * @param[in]     url    Chapter page URL (never NULL).
 * @param[in]     number Parsed chapter number (0 when unnumbered).
 *
 * @return The existing or newly-added record, or NULL when the table is full.
 * @retval NULL A NULL argument, or ::k_mdl_max_chapters already reached.
 *
 * @pre @p st, @p id, @p url are non-NULL and NUL-terminated.
 * @pre The caller treats NULL as "table full" and degrades, never crashes.
 * @post A new record starts incomplete with `page_count == 0`.
 * @post `st->chapter_count` grows by at most one.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
mdl_chapter_rec_t*
mdl_state_add_chapter(mdl_state_t* st, const char* id, const char* url, long number);

/**
 * @brief Whether a chapter is recorded fully fetched and verified.
 *
 * @param[in] st State to query (never NULL).
 * @param[in] id Chapter identifier (never NULL).
 *
 * @return Whether @p id is present AND its record is complete.
 * @retval true  The chapter exists and every page is fetched and verified.
 * @retval false Not recorded, incomplete, or a NULL argument.
 *
 * @pre @p st and @p id are non-NULL; @p id is NUL-terminated.
 * @pre The caller uses this to decide `--update` skips.
 * @post @p st is not modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool mdl_state_chapter_complete(const mdl_state_t* st, const char* id);

/**
 * @brief Recorded page count for a chapter (0 when unknown).
 *
 * @details
 * The combined-download page numbering is derived from these counts, so a run
 * that resumes reproduces the same continuous numbering an uninterrupted run
 * would have produced.
 *
 * @param[in] st State to query (never NULL).
 * @param[in] id Chapter identifier (never NULL).
 *
 * @return The chapter's recorded `page_count`, or 0 if unknown/absent.
 * @retval 0 The chapter is absent, unlearned, or a NULL argument was passed.
 *
 * @pre @p st and @p id are non-NULL; @p id is NUL-terminated.
 * @pre The caller treats 0 as "unknown", not "zero-page chapter".
 * @post @p st is not modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
uint16_t mdl_state_chapter_pages(const mdl_state_t* st, const char* id);

/**
 * @brief Find a page record by its source-URL hash (the dedup lookup).
 *
 * @param[in] st       State to search (never NULL).
 * @param[in] url_hash FNV-1a 64 of the candidate source URL.
 *
 * @return The first matching page record, or NULL when none is held.
 * @retval NULL No page with @p url_hash, or a NULL @p st.
 *
 * @pre @p st is non-NULL.
 * @pre @p url_hash came from ::mdl_hash_str on the source URL.
 * @post @p st is not modified.
 *
 * @note Not thread-safe.
 * @see mdl_state_add_page
 * @since 0.1.0
 */
const mdl_page_rec_t* mdl_state_find_page(const mdl_state_t* st, uint64_t url_hash);

/**
 * @brief Append a page's dedup/verify record to the series pool.
 *
 * @param[in,out] st           State to update (never NULL).
 * @param[in]     url_hash     FNV-1a 64 of the page's source URL.
 * @param[in]     content_hash FNV-1a 64 of the fetched page bytes.
 * @param[in]     rel_path     Page path relative to the series dir (never NULL).
 *
 * @return Whether the record was stored.
 * @retval true  The record was appended.
 * @retval false The pool is full (::k_mdl_max_page_recs) or a NULL argument;
 *               dedup simply degrades to a refetch next time, never a crash.
 *
 * @pre @p st and @p rel_path are non-NULL; @p rel_path is NUL-terminated.
 * @pre @p rel_path is a sanitised path with no `..`/leading `/`.
 * @post On true, `st->page_rec_count` grows by one.
 * @post On false, @p st is unchanged.
 *
 * @note Not thread-safe.
 * @see mdl_state_find_page
 * @since 0.1.0
 */
bool mdl_state_add_page(mdl_state_t* st,
                        uint64_t     url_hash,
                        uint64_t     content_hash,
                        const char*  rel_path);

/**
 * @brief Render a one-line coverage summary (chapter span, count, gaps).
 *
 * @details
 * Summarises which chapters are complete for the library `--list` view: the
 * count, the numeric span, and the missing chapter numbers inside that span, so
 * a reader can spot a hole without reading the directory tree.
 *
 * @param[in]  st  State to summarise (never NULL).
 * @param[out] buf Destination buffer for the NUL-terminated line (never NULL).
 * @param[in]  cap Capacity of @p buf in bytes (must be > 0).
 *
 * @return Nothing.
 *
 * @pre @p st and @p buf are non-NULL; @p cap > 0.
 * @pre The caller prints @p buf as one line.
 * @post @p buf is NUL-terminated.
 * @post @p st is not modified.
 *
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
void mdl_state_coverage(const mdl_state_t* st, char* buf, size_t cap);
