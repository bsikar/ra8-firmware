/**
 * @file mdl_state.h
 * @brief Persistent per-series library state for the media downloader.
 *
 * @details
 * Without this store `media_dl` kept nothing between runs: resuming was an
 * index into a freshly scraped list (`--start K`), there was no "fetch only
 * what is new", a kill mid-chapter left an unrecorded partial directory, and
 * two runs (or two chapters) sharing an image re-downloaded it. This module is
 * the on-disk record that fixes all four: one bounded, versioned state payload
 * per series holding the series identity, the site descriptor used, and -- per
 * chapter -- the parsed chapter identifier, source URL, page count, completion
 * status and fetch time, plus a series-wide pool of per-page content
 * identities.
 *
 * Because chapters are keyed by a parsed identifier
 * (::mdl_urlname_last_segment) rather than list position, `--start` can mean
 * "resume where we left off" and
 * `--update` can mean "only chapters we do not already have complete", both
 * stable as the site adds or reorders chapters. Because every page carries a
 * source-URL hash and a content hash, a re-run skips a byte-identical image
 * already held (::mdl_state_find_page) and a torn file is detected and
 * refetched rather than silently packaged.
 *
 * ### On-disk payload format (v4)
 * A flat, line-oriented, TAB-separated text payload under the logical
 * `.mdl_state` path, deliberately simple so it is human-readable and ports
 * unchanged to the RA8. A leading `#` marks a comment; blank lines are ignored. Each
 * record is a one-letter type followed by TAB-separated fields:
 *
 *     # media_dl library state v4
 *     V<TAB>4                                         schema version
 *     S<TAB><series-url>                              series URL
 *     T<TAB><series-title>                            series title
 *     N<TAB><site-name>                               site descriptor name
 *     H<TAB><site-host>                               site host
 *     G<TAB><config-path>                             descriptor file used
 *     D/W/A/O/K/L/R<TAB>value                         rich series metadata
 *     C<TAB>id<TAB>known<TAB>binary64-hex<TAB>done<TAB>pages<TAB>ready<TAB>epoch<TAB>url<TAB>title
 *     P<TAB>url_hash_hex<TAB>content_hash_hex<TAB>rel_path<TAB>etag<TAB>last_modified<TAB>epoch<TAB>status
 *
 * Version 1 files remain readable and are migrated in memory; their integral
 * chapter number is considered known only when nonzero because v1 had no
 * explicit presence bit. Version 2 decimal-number records are also migrated
 * with a bounded ASCII-only parser; version 3 stores the exact finite in-memory
 * binary64 identity as 16 canonical hex digits, independent of locale/libc.
 * Version 3 page records are migrated with an unknown fetch time/status. The
 * next save always emits v4. Current saves wrap the
 * payload in a fixed canonical-big-endian envelope containing a monotonic
 * sequence, exact payload length, and CRC-32 checksums. Two physical
 * generations (`.mdl_state` and `.mdl_state.alt`) alternate through validated
 * create-new transactions, so the newest accepted generation is never removed
 * before its successor is published. This provides truthful recovery on both
 * atomic-replace hosts and FAT/VFS backends that cannot replace atomically.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__STDC_IEC_559__) && !defined(__STDC_IEC_60559_BFP__)
#error "media_dl state v3 requires IEC 60559 binary floating-point"
#endif

static_assert(sizeof(double) == 8U, "media_dl state v3 requires binary64 double");
static_assert(FLT_RADIX == 2, "media_dl state v3 requires radix-2 floating-point");
static_assert(DBL_MANT_DIG == 53, "media_dl state requires 53-bit binary64 precision");
static_assert(DBL_MAX_EXP == 1024, "media_dl state requires binary64 exponent range");
static_assert(DBL_MIN_EXP == -1021, "media_dl state requires binary64 exponent range");

#include "mdl_config.h"
#include "mdl_extract.h"
#include "mdl_net.h"
#include "mdl_storage.h"
#include "ra8_err.h"

/** @brief Fixed capacities and the schema version (zero dynamic allocation). */
typedef enum : uint16_t {
  k_mdl_state_version_v1 = 1,    /**< Legacy integral schema accepted.       */
  k_mdl_state_version_v2 = 2,    /**< Legacy decimal schema accepted.        */
  k_mdl_state_version_v3 = 3,    /**< Legacy cache-metadata schema accepted. */
  k_mdl_state_version    = 4,    /**< Timestamped cache schema written now.  */
  k_mdl_chapter_id_max   = 128,  /**< Chapter identifier bytes (sanitised).  */
  k_mdl_title_max        = 192,  /**< Series title bytes.                    */
  k_mdl_summary_max      = 1024, /**< Series summary bytes.                  */
  k_mdl_person_max       = 128,  /**< Writer/artist bytes.                   */
  k_mdl_language_max     = 16,   /**< BCP-47 language bytes.                 */
  k_mdl_relpath_max      = 200,  /**< Page path relative to the series dir.  */
  k_mdl_cfgpath_max      = 512,  /**< Site-descriptor path bytes.            */
  k_mdl_max_chapters     = 512,  /**< Chapters tracked per series.           */
} mdl_state_limit_t;

/** @brief Persisted fixed-layout reading direction. */
typedef enum : uint8_t {
  k_mdl_state_read_ltr = 0, /**< Left-to-right page progression. */
  k_mdl_state_read_rtl = 1, /**< Right-to-left page progression. */
} mdl_state_reading_direction_t;

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
  char     chapter_id[k_mdl_chapter_id_max]; /**< Stable identifier (URL leaf).  */
  char     source_url[k_mdl_url_max];        /**< Chapter page URL.              */
  char     title[k_mdl_title_max];           /**< Display title, or empty.       */
  double   number;                           /**< Parsed number; 0 may be valid. */
  bool     number_known;                     /**< Whether @ref number is known.  */
  uint16_t page_count;                       /**< Total pages known (0 = ?).     */
  uint16_t pages_done;                       /**< Pages fetched and verified.    */
  bool     complete;                         /**< All pages present + verified.  */
  int64_t  fetched_at;                       /**< Completion time (epoch s).     */
} mdl_chapter_rec_t;

/**
 * @struct mdl_page_rec_t
 * @brief One page's dedup/verify record in the series-wide pool.
 * @details @ref url_hash keys a source URL so a re-fetch of the same image (a
 *          rerun, or an image shared between chapters) is found without the
 *          network; @ref content_hash is the byte identity used to verify a
 * page already on disk and to prove a reused copy is truly identical.
 * @invariant `rel_path` is a sanitised path relative to the series directory.
 * @see mdl_state_find_page
 * @since 0.1.0
 */
typedef struct {
  uint64_t url_hash;                    /**< FNV-1a 64 of the source URL.     */
  uint64_t content_hash;                /**< FNV-1a 64 of the fetched bytes.  */
  char     rel_path[k_mdl_relpath_max]; /**< Path under the series directory. */
  char     etag[k_mdl_etag_max];        /**< Cached ETag for conditional GET. */
  /** @brief Cached Last-Modified response value. */
  char     last_modified[k_mdl_last_mod_max];
  int64_t  fetched_at;      /**< Most recent HTTP result time (epoch s).  */
  uint16_t response_status; /**< Most recent HTTP status; zero if legacy. */
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
  uint16_t version;                        /**< Schema version.      */
  char     series_url[k_mdl_url_max];      /**< Series page URL.     */
  char     series_title[k_mdl_title_max];  /**< Series title.        */
  char     site_name[k_mdl_name_max];      /**< Descriptor name.     */
  char     site_host[k_mdl_host_max];      /**< Site host.           */
  char     config_path[k_mdl_cfgpath_max]; /**< Descriptor used.     */
  char     summary[k_mdl_summary_max];     /**< Series synopsis.     */
  char     writer[k_mdl_person_max];       /**< Writer/author.       */
  char     artist[k_mdl_person_max];       /**< Artist/illustrator.  */
  char     cover_url[k_mdl_url_max];       /**< Remote cover URL.    */
  char     cover_path[k_mdl_relpath_max];  /**< Local cover path.    */
  char     language[k_mdl_language_max];   /**< BCP-47 language tag. */
  /** @brief Page progression. */
  mdl_state_reading_direction_t reading_direction;
  /** @brief Chapters recorded. */
  uint16_t chapter_count;
  /** @brief Page records recorded. */
  uint32_t page_rec_count;
  /** @brief Per-chapter coverage. */
  mdl_chapter_rec_t chapters[k_mdl_max_chapters];
  /** @brief Per-page identities. */
  mdl_page_rec_t pages[k_mdl_max_page_recs];
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

 * @details Uses fixed-capacity state supplied by the caller without allocation.
 *          Any text retained by the state is copied into bounded records.
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
 * @param[in]     config_path Descriptor file path used (for update-all), or
 * NULL.
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

 * @details Uses fixed-capacity state supplied by the caller without allocation.
 *          Any text retained by the state is copied into bounded records.
 */
void mdl_state_set_series(mdl_state_t* st,
                          const char*  url,
                          const char*  title,
                          const char*  site_name,
                          const char*  site_host,
                          const char*  config_path);

/**
 * @brief Set the optional rich metadata persisted for a series.
 *
 * @details
 * Copies the complete metadata tuple only when every field fits its fixed
 * destination, contains no TAB/newline record delimiters, the cover path is a
 * relative non-traversing path, and @p direction is a supported value. Empty
 * strings explicitly clear fields; NULL string arguments are invalid.
 *
 * @param[in,out] st         State to update (never NULL).
 * @param[in]     summary    Series synopsis (may be empty).
 * @param[in]     writer     Writer/author name (may be empty).
 * @param[in]     artist     Artist/illustrator name (may be empty).
 * @param[in]     cover_url  Remote cover URL (may be empty).
 * @param[in]     cover_path Local cover path relative to the series directory.
 * @param[in]     language   BCP-47 language tag (may be empty).
 * @param[in]     direction  Page progression direction.
 *
 * @return Whether the complete tuple was accepted.
 * @retval true  Every value was validated and copied.
 * @retval false An argument was NULL, overlong, malformed, or unsupported.
 *
 * @pre @p st is non-NULL and caller-owned.
 * @pre All string arguments are non-NULL and NUL-terminated.
 * @post On true, all rich series metadata fields equal the supplied values.
 * @post On false, @p st is unchanged.
 *
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
bool mdl_state_set_series_metadata(mdl_state_t*                  st,
                                   const char*                   summary,
                                   const char*                   writer,
                                   const char*                   artist,
                                   const char*                   cover_url,
                                   const char*                   cover_path,
                                   const char*                   language,
                                   mdl_state_reading_direction_t direction);

/**
 * @brief Load a series' newest valid state through injected portable storage.
 *
 * @details
 * Authenticates both journal generations, chooses the highest valid sequence,
 * and falls back to the older valid generation if semantic parsing fails. A
 * legacy unenveloped v1/v2 base file remains readable as sequence zero. When
 * neither generation exists, @p st is initialized empty and success is
 * returned. Any final failure also leaves @p st initialized empty.
 *
 * @param[in,out] storage Initialized, exclusively-owned storage binding.
 * @param[in]  path Canonical logical state path (never NULL).
 * @param[out] st   State to fill; always left in a valid (possibly empty) form.
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok               Loaded, or the file was absent (empty state).
 * @retval k_ra8_err_invalid_arg  @p path or @p st was NULL.
 * @retval k_ra8_err_invalid_state The file exists but is corrupt/unsupported.
 *
 * @pre All pointers are non-NULL and @p storage was initialized successfully.
 * @pre The caller exclusively owns every workspace bound to @p storage.
 * @post @p st is always a valid state object on return (empty on any error).
 * @post No filesystem generation is modified.
 * @note Not thread-safe: borrows every workspace in @p storage.
 * @see mdl_state_save
 * @since 0.1.0
 */
ra8_err_t mdl_state_load(mdl_storage_t* storage, const char* path, mdl_state_t* st);

/**
 * @brief Load only an authenticated checksummed state generation.
 * @details Scans the logical base path and its `.alt` peer, rejects legacy
 *          unenveloped text, selects the highest valid authenticated sequence,
 *          and falls back to the older authenticated generation when semantic
 *          decoding fails. An absent marker initializes @p st empty and
 *          succeeds; every failure also leaves @p st initialized empty.
 * @param[in,out] storage Initialized, exclusively-owned storage binding.
 * @param[in] path Canonical logical state path.
 * @param[out] st State filled from one authenticated generation.
 * @return Canonical path, stream, authentication, or schema status.
 * @retval k_ra8_ok An authenticated generation loaded or both peers were absent.
 * @retval k_ra8_err_invalid_state Existing generations failed authentication.
 * @retval k_ra8_err_invalid_arg A pointer, path, or binding is invalid.
 * @pre Required pointers are non-NULL and @p storage is initialized.
 * @pre The caller exclusively owns all workspaces bound to @p storage.
 * @post @p st is a valid state object on every return, empty on failure.
 * @post No filesystem generation is modified.
 * @note Use for library trust decisions; ::mdl_state_load preserves import
 *       compatibility with legacy unenveloped state.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_state_load_authenticated(mdl_storage_t* storage, const char* path, mdl_state_t* st);

/**
 * @brief Publish a checksummed successor without sacrificing the newest state.
 *
 * @details
 * Selects the absent, invalid, or older physical generation; removes only that
 * target; writes a create-new private transaction; independently validates its
 * canonical envelope, exact length, and payload CRC; then commits it. The
 * newest accepted generation remains intact until commit. Sequence exhaustion
 * is reported rather than wrapped. Cleanup failure takes precedence while the
 * transaction is unpublished. A commit-time durability error can accompany a
 * true @p out_published and must not be retried as though nothing changed.
 *
 * @param[in,out] storage Initialized, exclusively-owned storage binding.
 * @param[in] path Canonical logical state path (never NULL).
 * @param[in] st   State to write (never NULL).
 * @param[out] out_published False initially; true exactly when the successor
 *                           became visible, including after durability failure.
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok The validated successor was published.
 * @retval k_ra8_err_invalid_arg A pointer, path, or binding is invalid.
 * @retval k_ra8_err_invalid_state Existing generations are ambiguous/corrupt.
 * @retval k_ra8_err_invalid_size The monotonic sequence or I/O bound is exhausted.
 * @retval other A namespace, transaction, validation, durability, or cleanup error.
 *
 * @pre All pointers are non-NULL and @p storage was initialized successfully.
 * @pre The caller exclusively owns every workspace bound to @p storage.
 * @post On success one generation contains the complete serialized @p st.
 * @post When @p out_published is false, the previously newest valid generation survives.
 * @post When @p out_published is true, the successor is visible regardless of return status.
 *
 * @note Not thread-safe: borrows every workspace in @p storage.
 * @see mdl_state_load
 * @since 0.1.0
 */
ra8_err_t mdl_state_save(mdl_storage_t*     storage,
                         const char*        path,
                         const mdl_state_t* st,
                         bool*              out_published);

/**
 * @brief Probe the complete two-generation state marker through portable storage.
 * @details Reports a tracked marker when either the logical base path or its
 *          bounded alternate generation exists as a regular file. Integrity
 *          and schema validation remain the responsibility of ::mdl_state_load.
 * @param[in,out] storage Initialized, exclusively-owned storage binding.
 * @param[in] path Canonical logical state path.
 * @param[out] out_exists Whether at least one regular state generation exists.
 * @return Canonical namespace/path status.
 * @retval k_ra8_ok The probe completed and initialized @p out_exists.
 * @retval k_ra8_err_invalid_arg A pointer/path is invalid or a marker is not a file.
 * @pre All pointers are non-NULL and @p storage was initialized successfully.
 * @pre The caller exclusively owns the namespace binding in @p storage.
 * @post On every return @p out_exists is initialized false unless a marker was found.
 * @post No filesystem object or state model is modified.
 * @note Not thread-safe because @p storage is a non-reentrant dependency bundle.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_state_probe(mdl_storage_t* storage, const char* path, bool* out_exists);

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
 * @post A new record starts incomplete with `page_count == 0`; a nonzero
 *       @p number is marked known and zero retains legacy "unknown" semantics.
 * @post `st->chapter_count` grows by at most one.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
mdl_chapter_rec_t*
mdl_state_add_chapter(mdl_state_t* st, const char* id, const char* url, long number);

/**
 * @brief Find or append a chapter with explicit parsed-number presence.
 *
 * @details
 * Unlike the source-compatible ::mdl_state_add_chapter wrapper, this API keeps
 * chapter zero distinct from an unknown number and preserves fractional chapter
 * numbers. An unknown number must be supplied canonically as 0.0.
 *
 * @param[in,out] st           State to update (never NULL).
 * @param[in]     id           Chapter identifier (never NULL).
 * @param[in]     url          Chapter page URL (never NULL).
 * @param[in]     number       Finite parsed chapter number, or 0.0 if unknown.
 * @param[in]     number_known Whether @p number was explicitly parsed.
 *
 * @return The existing or newly-added record, or NULL when invalid/full.
 * @retval NULL A NULL/malformed argument, invalid number, or full table.
 *
 * @pre @p st, @p id, and @p url are non-NULL.
 * @pre @p id and @p url are NUL-terminated and fit their fixed fields.
 * @post A new record stores @p number and @p number_known exactly.
 * @post An existing record is returned without changing its metadata.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
mdl_chapter_rec_t* mdl_state_add_chapter_numbered(mdl_state_t* st,
                                                  const char*  id,
                                                  const char*  url,
                                                  double       number,
                                                  bool         number_known);

/**
 * @brief Set a chapter's display title and explicit parsed number.
 *
 * @details
 * Applies both fields transactionally after validating the title and number.
 * A known number may be zero or fractional; an unknown number must be 0.0.
 *
 * @param[in,out] chapter      Chapter record to update (never NULL).
 * @param[in]     title        Display title (may be empty, never NULL).
 * @param[in]     number       Finite parsed chapter number, or 0.0 if unknown.
 * @param[in]     number_known Whether @p number was explicitly parsed.
 *
 * @return Whether the complete metadata tuple was accepted.
 * @retval true  The title and number were stored.
 * @retval false An argument was invalid; @p chapter is unchanged.
 *
 * @pre @p chapter is non-NULL and caller-owned.
 * @pre @p title is non-NULL and NUL-terminated.
 * @post On true, the chapter metadata exactly matches the arguments.
 * @post On false, @p chapter is unchanged.
 *
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
bool mdl_state_set_chapter_metadata(mdl_chapter_rec_t* chapter,
                                    const char*        title,
                                    double             number,
                                    bool               number_known);

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

 * @details Uses fixed-capacity state supplied by the caller without allocation.
 *          Any text retained by the state is copied into bounded records.
 * @post Documented outputs and the return value describe the same outcome.
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

 * @post Documented outputs and the return value describe the same outcome.
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
 * @brief Add or replace a URL-keyed page cache record.
 *
 * @details
 * Adds or refreshes a bounded page identity and its optional HTTP validators.
 * The URL hash is the sole cache key: when magic-byte validation changes a
 * page's canonical extension or a combined layout relocates it, the existing
 * record is replaced instead of leaving an older path first in lookup order.
 * Empty or NULL validator strings are stored as empty values.
 *
 * @param[in,out] st           State to update (never NULL).
 * @param[in]     url_hash     FNV-1a 64 of the page's source URL.
 * @param[in]     content_hash FNV-1a 64 of the fetched page bytes.
 * @param[in]     rel_path     Page path relative to the series dir (never
 * NULL).
 * @param[in]     etag         Cached ETag, or NULL when unavailable.
 * @param[in]     last_modified Cached Last-Modified, or NULL when unavailable.
 * @param[in]     fetched_at Most recent fetch completion time in epoch seconds.
 * @param[in]     response_status Most recent HTTP status, or zero if unknown.
 *
 * @return Whether the record was stored.
 * @retval true  The record was appended or the existing URL record replaced.
 * @retval false The pool is full (::k_mdl_max_page_recs) or a NULL argument;
 *               dedup simply degrades to a refetch next time, never a crash.
 *
 * @pre @p st and @p rel_path are non-NULL; @p rel_path is NUL-terminated.
 * @pre @p rel_path is a sanitised path with no `..`/leading `/`.
 * @post On true, `st->page_rec_count` grows by one only for a new URL hash.
 * @post On false, @p st is unchanged.
 *
 * @note Not thread-safe.
 * @see mdl_state_find_page
 * @since 0.1.0
 */
bool mdl_state_add_page(mdl_state_t* st,
                        uint64_t     url_hash,
                        uint64_t     content_hash,
                        const char*  rel_path,
                        const char*  etag,
                        const char*  last_modified,
                        int64_t      fetched_at,
                        uint16_t     response_status);

/**
 * @brief Refresh the observed HTTP result for one existing URL cache entry.
 * @details Locates the record by its stable URL hash and atomically replaces
 *          only the completion time and response-status fields.
 * @param[in,out] st State containing the URL-keyed record.
 * @param[in] url_hash FNV-1a 64 hash selecting the record.
 * @param[in] fetched_at Completed request time in epoch seconds.
 * @param[in] response_status HTTP response status in 100..599.
 * @return Whether an existing record and valid observation were updated.
 * @retval true The matching record now carries the supplied observation.
 * @retval false No record matched or the time/status was invalid.
 * @pre @p st is non-NULL and exclusively owned.
 * @pre @p fetched_at is nonnegative.
 * @post Success changes only the two observation fields.
 * @post Failure leaves @p st unchanged.
 * @note Used after a bodyless 304 revalidates already-verified local bytes.
 * @since 0.1.0
 */
bool mdl_state_note_page_response(mdl_state_t* st,
                                  uint64_t     url_hash,
                                  int64_t      fetched_at,
                                  uint16_t     response_status);

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
