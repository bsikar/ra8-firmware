/**
 * @file mdl_app.h
 * @brief The downloader's portable application layer: one run mode, one call.
 *
 * @details
 * Everything above the individual subsystems (fetch, state, cache, export,
 * verify, library) and below the thing that parsed a request. A build form
 * supplies four host-shaped resources and then calls one of the
 * `mdl_app_run_*` entry points below; it does not reimplement a mode:
 *
 *  - the bounded ::mdl_app_context_t working set, bound once with
 *    ::mdl_app_bind, so the form decides where the multi-megabyte page and
 *    export buffers live;
 *  - a ::mdl_storage_t over `fw_if_fs`, so a mode names paths and never a
 *    host filesystem call;
 *  - two ::ra8_io_stream_t sinks, so a mode writes bytes and never `stdio`;
 *  - one ::mdl_net_provider_t, so a mode opens a transport for its run
 *    without naming libcurl, the C6 radio, or a test fake.
 *
 * That last one is the seam that used to be absent: the mode functions called
 * the libcurl factory directly, so the orchestration could only ever be
 * compiled by the host CLI. The provider inverts it, and everything else here
 * follows -- these entry points take values, not an argv-shaped struct, so the
 * argv grammar stays in the form that has one.
 *
 * @see mdl_net.h  The transport vtable and the provider dispatcher.
 * @see mdl_storage.h  The filesystem facade a form binds.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_cache.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_format.h"
#include "mdl_library.h"
#include "mdl_net.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @brief Fixed sizing for one application composition (large buffers in .bss).
 * @details A form declares its storage with these so every form agrees on the
 *          bounds the modes were written against.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_page_buf_bytes     = 8U * 1024U * 1024U,  /**< Max HTML page size.          */
  k_export_arena_bytes = 96U * 1024U * 1024U, /**< Host export scratch ceiling. */
  k_storage_work_bytes = 2048U,               /**< Per-handle FS backend state. */
} mdl_app_limits_t;

/**
 * @struct mdl_run_opts_t
 * @brief Cross-cutting policy and injected transport threaded into every mode.
 * @details Bundles the network policy, the identity/politeness knobs and the
 *          transport factory so the run entry points take one options pointer
 *          rather than a long, swappable scalar parameter list. `net` is the
 *          Dependency Injection seam: production wires a real backend factory,
 *          tests wire a scripted fake with the same ::mdl_net_iface_t shape.
 * @invariant `policy.max_response_bytes` is non-zero for a bounded fetch.
 * @invariant `net` is non-NULL for every mode that performs a request.
 * @see mdl_net_provider_t
 * @since 0.1.0
 */
typedef struct {
  mdl_net_policy_t          policy;           /**< Backend security policy.             */
  const mdl_net_provider_t* net;              /**< Injected transport factory.          */
  const char*               contact;          /**< Operator contact override, or NULL.  */
  bool                      honor_robots;     /**< False when robots.txt is ignored.    */
  bool                      polite;           /**< True to raise per-host delays.       */
  bool                      allow_incomplete; /**< True to package a run with failures. */
  bool                      progress;         /**< True to draw a progress bar.         */
  bool                      refetch;          /**< True to bypass verified page reuse.  */
} mdl_run_opts_t;

/**
 * @struct mdl_series_metadata_t
 * @brief Bounded metadata scraped while the series HTML is resident.
 * @details Holds one series' descriptive fields between the scrape that
 *          produced them and the state reconciliation that persists them.
 * @invariant Every field is NUL-terminated within its declared capacity.
 * @see mdl_app_context_t
 * @since 0.1.0
 */
typedef struct {
  char                          title[k_mdl_title_max];       /**< Display title.            */
  char                          summary[k_mdl_summary_max];   /**< Synopsis/description.     */
  char                          writer[k_mdl_person_max];     /**< Writer/author.            */
  char                          artist[k_mdl_person_max];     /**< Artist/illustrator.       */
  char                          cover_url[k_mdl_url_max];     /**< Resolved remote cover.    */
  char                          language[k_mdl_language_max]; /**< BCP-47 language tag.      */
  mdl_state_reading_direction_t direction;                    /**< Page progression.         */
  bool                          title_selected;               /**< Title selector succeeded. */
} mdl_series_metadata_t;

/**
 * @struct mdl_series_run_t
 * @brief Parameters of one series download, as values rather than arguments.
 * @details Every field is either a validated scalar or a borrowed string the
 *          caller keeps alive for the run, so a form that has no command line
 *          can build one just as easily as one that does.
 * @invariant `out_dir` and `cache_dir` are canonical absolute paths.
 * @see mdl_app_run_series()
 * @since 0.1.0
 */
typedef struct {
  const char*           cfg_path;     /**< Site descriptor path.           */
  const char*           series_url;   /**< Series page URL.                */
  const char*           out_dir;      /**< Output library root.            */
  const char*           cache_dir;    /**< Per-host persistent cache root. */
  mdl_format_t          format;       /**< Output container.               */
  bool                  combine;      /**< Combine into one archive.       */
  bool                  update;       /**< Incremental (skip complete).    */
  bool                  from_present; /**< Whether from_num applies.       */
  double                from_num;     /**< First chapter number to fetch.  */
  size_t                chapters;     /**< Max chapters (window mode).     */
  uint64_t              seed;         /**< Politeness jitter seed.         */
  uint32_t              timeout;      /**< Per-request budget, ms.         */
  const mdl_run_opts_t* opts;         /**< Identity/security knobs.        */
} mdl_series_run_t;

/**
 * @struct mdl_discover_run_t
 * @brief One search or browse request against a configured site descriptor.
 * @details Names the selection a discovery run makes without naming how the
 *          caller expressed it, so the same values arrive from an argv parser,
 *          a config file, or an on-device menu.
 * @invariant `browse` is true or `term` is a non-empty search string.
 * @see mdl_app_run_discover()
 * @since 0.1.0
 */
typedef struct {
  const char* cfg_path; /**< Site descriptor path.                    */
  const char* term;     /**< Search term; unused when browsing.       */
  size_t      pick;     /**< 1-based hit to download; 0 lists only.   */
  uint32_t    timeout;  /**< Per-request budget, ms.                  */
  uint64_t    seed;     /**< Governor jitter seed.                    */
  bool        browse;   /**< Browse latest updates instead of search. */
} mdl_discover_run_t;

/**
 * @struct mdl_app_context_t
 * @brief The one bounded working set every application mode shares.
 * @details Caller-owned so the form -- not the downloader -- decides where the
 *          multi-megabyte page buffer and the state, cache and library
 *          workspaces live. Bind it once with ::mdl_app_bind before any mode
 *          runs; the modes reach it through one module-private accessor and
 *          never through a link-time global of their own.
 * @invariant Exactly one context is bound while any mode is running.
 * @invariant `output` and `diagnostic` outlive every bound mode call.
 * @see mdl_app_bind()
 * @since 0.1.0
 */
typedef struct {
  mdl_session_t           session;                /**< Network policy session.       */
  mdl_export_workspace_t  export_ws;              /**< Bounded exporter arena view.  */
  mdl_storage_t           storage;                /**< Injected filesystem facade.   */
  ra8_io_stream_t*        output;                 /**< Borrowed command output sink. */
  ra8_io_stream_t*        diagnostic;             /**< Borrowed diagnostic sink.     */
  ra8_err_t               io_error;               /**< First presenter sink failure. */
  char                    page[k_page_buf_bytes]; /**< Whole-page HTML buffer.       */
  mdl_url_list_t          chapters;               /**< Prepared chapter URLs.        */
  mdl_url_list_t          images;                 /**< Prepared page image URLs.     */
  mdl_state_t             state;                  /**< Persistent tracked state.     */
  mdl_cache_index_t       cache_index;            /**< One loaded per-host index.    */
  mdl_library_workspace_t library_workspace;      /**< Bounded tree-removal stack.   */
  union {
    max_align_t align;                         /**< Force backend alignment.    */
    uint8_t     bytes[k_mdl_storage_io_bytes]; /**< Second directory cursor.    */
  } admin_directory_workspace;                 /**< Nested verification cursor. */
  mdl_series_metadata_t series_metadata;       /**< Current scraped metadata.   */
} mdl_app_context_t;

/**
 * @brief Bind the caller-owned working set every application mode will use.
 *
 * @details
 * The composition seam for the application layer. A form declares one
 * ::mdl_app_context_t in its own storage -- static on a host, a reserved SRAM
 * region on the device -- initialises the members it owns (the two streams,
 * the storage facade, the exporter workspace) and hands the address here. Every
 * mode then reads that one context through a module-private accessor, so the
 * downloader needs no global of its own and a test can bind a fixture context
 * instead of the production one.
 *
 * Binding is unconditional and reports nothing: there is exactly one thing it
 * can do and one way to get it wrong, and passing a context the modes cannot
 * dereference is a composition defect rather than a runtime condition -- the
 * same contract ::mdl_export_workspace_init already publishes for the exporter
 * arena.
 *
 * @param[in,out] ctx Caller-owned context, live until the next bind.
 *
 * @return Nothing.
 *
 * @pre @p ctx addresses writable storage for one complete context.
 * @pre No mode call is in progress on another thread.
 * @post @p ctx is the context every subsequent mode reads.
 * @post No caller storage is copied and no ownership is transferred.
 *
 * @note Not thread-safe: it publishes process-wide composition state.
 * @since 0.1.0
 */
void mdl_app_bind(mdl_app_context_t* ctx);

/**
 * @brief Execute one prepared series download.
 * @details Loads the descriptor, opens a transport through the injected
 *          provider, prepares the chapter list, reconciles persisted metadata,
 *          fetches, checkpoints and exports.
 * @param[in] run Prepared series identity, selection, format, and policy.
 * @return Run status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre @p run is non-NULL and a context is bound.
 * @pre The bound context is exclusively owned by the calling thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post The transport opened for the run is destroyed on every path.
 * @note Not thread-safe because it uses the bound application context.
 * @since 0.1.0
 */
int mdl_app_run_series(const mdl_series_run_t* run);

/**
 * @brief List every tracked series under one library root.
 * @details Visits only authenticated `.mdl_state` markers and reports each
 *          series' directory, source URL, and coverage summary.
 * @param[in] out_dir Canonical library root to enumerate.
 * @return Run status.
 * @retval 0 Enumeration completed and every state file was readable.
 * @retval 1 Traversal failed or at least one state file was unreadable.
 * @pre @p out_dir is non-NULL and a context is bound.
 * @pre The bound output stream accepts writes.
 * @post Every discovered tracked series is reported once.
 * @post No library data is modified.
 * @note Not thread-safe because callbacks use the shared state scratch.
 * @since 0.1.0
 */
int mdl_app_run_list(const char* out_dir);

/**
 * @brief Remove one explicitly tracked series directory.
 * @details Derives the bounded slug, refuses symbolic-link and untracked
 *          targets, and requires the recorded series URL to resolve to the same
 *          slug before delegating recursive removal.
 * @param[in] out_dir Canonical library root.
 * @param[in] url_or_slug Series URL or slug selected by the caller.
 * @return Run status.
 * @retval 0 The tracked tree was removed.
 * @retval 1 Resolution, marker validation, or removal failed.
 * @pre Both pointers are non-NULL and a context is bound.
 * @pre The caller has authorised a destructive operation.
 * @post Untracked, corrupt, mismatched or symlinked directories are never
 *       removed.
 * @post Success reports the removed directory path.
 * @note Not safe for concurrent mutation of the same tree.
 * @since 0.1.0
 */
int mdl_app_run_remove(const char* out_dir, const char* url_or_slug);

/**
 * @brief Incrementally update every tracked series under a library root.
 * @details Enumerates the library with a fixed run template, inherits each
 *          series' recorded URL and descriptor, and summarises the failures.
 * @param[in] base Template run parameters, including the library root.
 * @return Run status.
 * @retval 0 Traversal and every attempted update succeeded.
 * @retval 1 Traversal or at least one series update failed.
 * @pre @p base is non-NULL and names a valid output root.
 * @pre A context is bound and its workspaces are initialised.
 * @post Every tracked series is visited at most once.
 * @post Failures remain visible in the returned status and the diagnostics.
 * @note Not thread-safe because updates reuse the bound context.
 * @since 0.1.0
 */
int mdl_app_run_update_all(const mdl_series_run_t* base);

/**
 * @brief Run search or browse discovery and optionally download one result.
 * @details Loads the descriptor, opens a transport through the injected
 *          provider, lists the bounded hits, and feeds an explicitly selected
 *          hit into series mode.
 * @param[in] req Validated discovery selection.
 * @param[in] opts Validated network and execution policy.
 * @param[in] base Series defaults inherited by the chosen hit.
 * @return Run status.
 * @retval 0 Results were listed or the selected series succeeded.
 * @retval nonzero Descriptor, network, discovery, or series work failed.
 * @pre All pointers are non-NULL and a context is bound.
 * @pre @p req names a descriptor the bound storage can read.
 * @post The discovery transport is destroyed before return.
 * @post A picked URL is downloaded only when selection produced a complete URL.
 * @note Not thread-safe because it uses the shared discovery buffers.
 * @since 0.1.0
 */
int mdl_app_run_discover(const mdl_discover_run_t* req,
                         const mdl_run_opts_t*     opts,
                         const mdl_series_run_t*   base);

/**
 * @brief Verify one library or artifact tree without mutating it.
 * @details Detects a direct tracked-series marker or enumerates child series,
 *          rehashes every recorded page, validates recognised artifacts through
 *          their format readers, and prints the totals.
 * @param[in] target_dir Canonical directory tree to verify.
 * @return Run status.
 * @retval 0 At least one target was found and every check passed.
 * @retval 1 The root was invalid, empty of targets, or a check failed.
 * @pre @p target_dir is non-NULL and a context is bound.
 * @pre The bound exporter workspace is initialised.
 * @post The target tree and its artifacts remain unmodified.
 * @post Every discovered failure contributes to the status and the summary.
 * @note Not thread-safe because it uses the shared validator workspace.
 * @since 0.1.0
 */
int mdl_app_run_verify(const char* target_dir);

/**
 * @brief Create a starter descriptor for one validated site URL.
 * @details Derives the descriptor identity, joins it beneath the caller-selected
 *          descriptor directory, and publishes it through a validated
 *          create-new transaction that never replaces an existing file.
 * @param[in] url Canonical absolute site URL.
 * @param[in] descriptor_dir Canonical existing directory chosen by the form.
 * @return Run status.
 * @retval 0 The descriptor template was created.
 * @retval 1 The destination could not be created.
 * @retval 2 The URL was absent or its host could not be extracted.
 * @pre @p descriptor_dir is a canonical absolute path and a context is bound.
 * @pre The bound storage owns the descriptor namespace exclusively.
 * @post Failure preserves any existing descriptor byte-for-byte.
 * @post Success publishes exactly the generated descriptor bytes.
 * @note Not safe for concurrent creation of the same descriptor path.
 * @since 0.1.0
 */
int mdl_app_run_init_site(const char* url, const char* descriptor_dir);

/**
 * @brief Package an existing image directory without network access.
 * @details Validates @p dir, distinguishes directory-output formats from file
 *          containers, and streams the supported images through the exporter.
 * @param[in] dir Existing canonical chapter image directory.
 * @param[in] format Selected output format.
 * @return Run status.
 * @retval 0 Packaging completed successfully.
 * @retval 1 Path resolution or export failed.
 * @retval 2 The requested format is absent or invalid for pack mode.
 * @pre @p dir is non-NULL and a context is bound.
 * @pre The bound exporter workspace is exclusively owned by the caller.
 * @post No network request is attempted by this mode.
 * @post Export errors are reported with their exact format.
 * @note Not thread-safe because it uses the shared exporter workspace.
 * @since 0.1.0
 */
int mdl_app_run_pack(const char* dir, mdl_format_t format);

/**
 * @brief Download, validate and atomically publish one direct artifact.
 * @details Accepts only formats with structural validators, stages the response
 *          through a transaction, validates it through the reader path, and
 *          only then commits it.
 * @param[in] url Canonical absolute artifact URL.
 * @param[in] out_dir Canonical output directory.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and execution policy.
 * @return Run status.
 * @retval 0 A structurally valid artifact was published.
 * @retval 1 Format, path, network, validation, or commit failed.
 * @pre All pointers are non-NULL and a context is bound.
 * @pre @p opts carries a transport provider.
 * @post Failure does not publish a partial destination artifact.
 * @post The transport opened for the run is destroyed on every path.
 * @note Not thread-safe because validation uses the shared exporter workspace.
 * @since 0.1.0
 */
int mdl_app_run_artifact(const char*           url,
                         const char*           out_dir,
                         uint32_t              timeout,
                         const mdl_run_opts_t* opts);

/**
 * @brief Download image resources from one direct page URL.
 * @details Opens a transport through the injected provider, enforces URL
 *          policy, extracts the selected image attribute and downloads the
 *          bounded image set under the governor.
 * @param[in] url Canonical absolute page URL.
 * @param[in] out_dir Canonical output directory.
 * @param[in] attr Image attribute selector.
 * @param[in] max_imgs Maximum images to transfer, or zero for every match.
 * @param[in] seed Deterministic politeness-jitter seed.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and execution policy.
 * @return Run status.
 * @retval 0 Every attempted image succeeded.
 * @retval 1 Network, policy, or image download failed.
 * @pre All pointers are non-NULL and a context is bound.
 * @pre @p opts carries a transport provider.
 * @post No staged partial image remains published as a final page.
 * @post The transport opened for the run is destroyed on every path.
 * @note Not thread-safe because it uses the shared extraction buffers.
 * @since 0.1.0
 */
int mdl_app_run_page(const char*           url,
                     const char*           out_dir,
                     const char*           attr,
                     uint32_t              max_imgs,
                     uint64_t              seed,
                     uint32_t              timeout,
                     const mdl_run_opts_t* opts);
