/**
 * @file mdl_app_internal.h
 * @brief Module-private composition contract for media_dl CLI actions.
 * @details Declares bounded cross-translation-unit run descriptions, the one
 *          composition-owned application context, and private action seams.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_app_storage_internal.h"
#include "mdl_cache.h"
#include "mdl_cli.h"
#include "mdl_config.h"
#include "mdl_discover.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_library.h"
#include "mdl_net.h"
#include "mdl_net_curl.h"
#include "mdl_pack.h"
#include "mdl_pathfs.h"
#include "mdl_politeness.h"
#include "mdl_report.h"
#include "mdl_sanitize.h"
#include "mdl_session.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "mdl_stream_internal.h"
#include "mdl_url_guard.h"
#include "mdl_urlname.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

/** @brief Fixed sizing for the CLI (large buffers live in .bss). */
typedef enum : uint32_t {
  k_page_buf_bytes     = 8U * 1024U * 1024U,  /**< Max HTML page size.          */
  k_export_arena_bytes = 96U * 1024U * 1024U, /**< Host export scratch ceiling. */
  k_storage_work_bytes = 2048U,               /**< Per-handle FS backend state. */
} mdl_cli_limits_t;

/** @brief On-stack string buffer sizes and page-mode delays. */
typedef enum : uint16_t {
  k_slug_bytes         = 128,  /**< Series slug buffer.              */
  k_leaf_name_bytes    = 256,  /**< Composed archive/dir leaf name.  */
  k_dir_path_bytes     = 1024, /**< Directory-path buffer.           */
  k_file_path_bytes    = 1200, /**< File-path buffer.                */
  k_cov_bytes          = 256,  /**< Coverage summary line buffer.    */
  k_ext_bytes          = 8,    /**< Image-extension buffer.          */
  k_page_img_delay_min = 400,  /**< page-mode per-image floor, ms.   */
  k_page_img_delay_max = 800,  /**< page-mode per-image ceiling, ms. */
} mdl_bufsize_t;

/** @brief `--polite` per-image delay floors for page mode (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms = 2000, /**< Polite per-image floor.   */
  k_polite_img_max_ms = 4000, /**< Polite per-image ceiling. */
} mdl_polite_floor_t;

/** @brief Bounded metadata scraped while the series HTML is resident. */
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

/** @brief Parameters of one series download. */
typedef struct {
  const char*           cfg_path;     /**< Site descriptor path.           */
  const char*           series_url;   /**< Series page URL.                */
  const char*           out_dir;      /**< Output library root.            */
  const char*           cache_dir;    /**< Per-host persistent cache root. */
  ra8_mdl_format_t      format;       /**< Output container.               */
  bool                  combine;      /**< Combine into one archive.       */
  bool                  update;       /**< Incremental (skip complete).    */
  bool                  from_present; /**< Whether from_num applies.       */
  double                from_num;     /**< First chapter number to fetch.  */
  size_t                chapters;     /**< Max chapters (window mode).     */
  uint64_t              seed;         /**< Politeness jitter seed.         */
  uint32_t              timeout;      /**< Per-request budget, ms.         */
  const mdl_run_opts_t* opts;         /**< Identity/security knobs.        */
} series_run_t;

/** @brief Shared bounded process state behind one module-private accessor. */
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

RA8_PRIV mdl_app_context_t* priv_mdl_app_context(void);

/**
 * @brief Report corruption recovery or verified cache reuse with staleness.
 * @details Emits a rebuild warning and, for retained-body reuse, the exact age,
 *          observed status, and URL through the bound diagnostic stream.
 * @param[in] url Exact cached URL.
 * @param[in] result Completed cache outcome.
 * @return Canonical diagnostic-stream status.
 * @retval k_ra8_ok Every applicable diagnostic was accepted.
 * @retval other The diagnostic stream rejected a write.
 * @pre Both pointers are non-NULL and the application diagnostic is bound.
 * @pre @p result came from one completed ::mdl_cache_get_buf call.
 * @post Reuse emits one complete staleness line.
 * @post Non-reuse emits only a corruption-rebuild warning when applicable.
 * @note Not thread-safe because it uses process-global application context.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_report_cache(const char* url, const mdl_cache_result_t* result);

/**
 * @brief Ensure series state names one verified cached or freshly fetched cover.
 * @details Applies robots/governor policy, revalidates through the per-host
 *          cache when bound, and publishes only magic-typed complete image bytes.
 * @param[in,out] ctx Fully configured fetch context.
 * @param[in] series_directory Canonical absolute series directory.
 * @return Canonical cache, policy, network, or publication status.
 * @retval k_ra8_ok No cover is configured or a verified local cover exists.
 * @retval k_ra8_err_invalid_arg A required binding, URL, or path is invalid.
 * @retval k_ra8_err_validation_failed Returned bytes are not a supported image.
 * @retval other Cache, governor, network, diagnostic, or storage work failed.
 * @pre Both pointers are non-NULL and the directory is canonical.
 * @pre Application state and storage bindings are initialized and exclusive.
 * @post Success with a configured URL names a verified local cover.
 * @post Failure leaves no active cover publication transaction.
 * @note Not thread-safe because it uses the process application context.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_ensure_series_cover(mdl_fetch_ctx_t* ctx,
                                                    const char*      series_directory);

/**
 * @brief Fetch and extract a series' bounded chapter list.
 * @details Applies the selected descriptor and governed session to the series
 *          document, replacing the shared chapter list only with validated
 * URLs.
 * @param[in] site Selected site descriptor.
 * @param[in] series_url Canonical series URL.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in,out] cache Bound persistent HTTP cache and index workspace.
 * @return Canonical downloader status.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post The call completes synchronously before returning to its dispatcher.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_prepare_chapters(const mdl_site_t* site,
                                                 const char*       series_url,
                                                 uint32_t          timeout,
                                                 mdl_cache_t*      cache);
/**
 * @brief Initialize one bounded network-policy session.
 * @details Builds the identifying user agent in @p ua, applies contact and CLI
 *          policy, and binds @p net without retaining temporary input storage.
 * @param[out] net Injected network interface to configure.
 * @param[in] opts Validated network and execution policy.
 * @param[in] cfg_contact Configured contact identity for governed requests.
 * @param[out] ua Receives the NUL-terminated user-agent string.
 * @param[in] ua_cap Capacity of @p ua in bytes.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre @p ua references @p ua_cap writable bytes and @p net is uninitialized.
 * @post No ownership of caller-provided storage is transferred.
 * @post @p ua is NUL-terminated and @p net reflects the complete session
 * policy.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0

 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_PRIV ra8_err_t priv_mdl_app_start_session(mdl_net_iface_t*      net,
                                              const mdl_run_opts_t* opts,
                                              const char*           cfg_contact,
                                              char*                 ua,
                                              size_t                ua_cap);
/**
 * @brief Prepare a contained canonical directory for one series.
 * @details Derives and sanitizes the URL identity, creates the series directory
 *          through injected storage, and returns its canonical absolute path.
 * @param[in] out_dir Output-library root path.
 * @param[in] series_url Canonical series URL.
 * @param[out] slug Receives the sanitized stable series identifier.
 * @param[in] slug_cap Capacity of @p slug in bytes.
 * @param[out] abs_dir Receives the canonical absolute series directory.
 * @return Whether the requested condition or operation succeeded.
 * @retval true The condition holds or the operation completed.
 * @retval false Input was rejected or the condition does not hold.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre @p slug and @p abs_dir reference their fixed application capacities.
 * @post No ownership of caller-provided storage is transferred.
 * @post Success returns a directory contained beneath @p out_dir.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_app_prepare_series_dir(const char* out_dir,
                                              const char* series_url,
                                              char*       slug,
                                              size_t      slug_cap,
                                              char*       abs_dir);
/**
 * @brief Compose the state-journal path within a series directory.
 * @details Joins the fixed state filename to @p abs_dir and rejects truncation;
 *          no file is opened or published by this helper.
 * @param[in] abs_dir Canonical absolute series directory.
 * @param[out] out Caller-provided result storage.
 * @param[in] cap Capacity of the associated output buffer in bytes.
 * @return Whether the requested condition or operation succeeded.
 * @retval true The condition holds or the operation completed.
 * @retval false Input was rejected or the condition does not hold.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre @p out references @p cap writable bytes.
 * @post No ownership of caller-provided storage is transferred.
 * @post Success leaves @p out NUL-terminated within @p cap.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_app_state_path_of(const char* abs_dir, char* out, size_t cap);
/**
 * @brief Execute one prepared series download.
 * @details Coordinates run series with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] run Prepared series identity, selection, format, and policy.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post The call completes synchronously before returning to its dispatcher.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_series(const series_run_t* run);
/**
 * @brief Execute one selected library-management action.
 * @details Coordinates run library with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] args Validated command arguments.
 * @param[in] run Prepared series defaults used by update actions.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post At most the single validated library action is executed.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_library(const mdl_args_t* args, const series_run_t* run);
/**
 * @brief Download one direct binary artifact atomically.
 * @details Coordinates run artifact with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] url Canonical input URL.
 * @param[in] out_dir Output-library root path.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and execution policy.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post Failure does not publish a partial destination artifact.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_artifact(const char*           url,
                                       const char*           out_dir,
                                       uint32_t              timeout,
                                       const mdl_run_opts_t* opts);
/**
 * @brief Download image resources from one direct page URL.
 * @details Performs run page under the injected network, governor, and storage
 * contracts; dependency failures are propagated before incomplete bytes are
 * published.
 * @param[in] url Canonical input URL.
 * @param[in] out_dir Output-library root path.
 * @param[in] attr Optional image attribute override.
 * @param[in] max_imgs Maximum number of extracted images to transfer.
 * @param[in] seed Deterministic politeness-jitter seed.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and execution policy.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post No staged partial image remains published as a final page.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_page(const char*           url,
                                   const char*           out_dir,
                                   const char*           attr,
                                   uint32_t              max_imgs,
                                   uint64_t              seed,
                                   uint32_t              timeout,
                                   const mdl_run_opts_t* opts);
/**
 * @brief Package an existing image directory without network access.
 * @details Validates @p dir and streams its supported images through the
 * selected exporter using the composition root's caller-owned workspace.
 * @param[in] dir Existing chapter image directory.
 * @param[in] format Selected output format.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared exporter workspace is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post No network request is attempted by this mode.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_pack(const char* dir, ra8_mdl_format_t format);
/**
 * @brief Build a prepared series-run value from validated arguments.
 * @details Copies only validated selections, numeric bounds, output format, and
 *          policy pointers into a by-value runner description.
 * @param[in] args Validated command arguments.
 * @param[in] format Selected output format.
 * @param[in] opts Validated network and execution policy.
 * @param[in] nums Validated numeric command options.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre @p args, @p opts, and @p nums outlive the returned run's execution.
 * @post No ownership of caller-provided storage is transferred.
 * @post The returned value contains no newly allocated or owned storage.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV series_run_t priv_mdl_app_build_run(const mdl_args_t*     args,
                                             ra8_mdl_format_t      format,
                                             const mdl_run_opts_t* opts,
                                             const mdl_nums_t*     nums);
/**
 * @brief Discover and optionally download one selected series.
 * @details Coordinates run discover with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] args Validated command arguments.
 * @param[in] opts Validated network and execution policy.
 * @param[in] nums Validated numeric command options.
 * @param[in] base Prepared series defaults inherited by the chosen hit.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared application context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post At most one validated discovery result is dispatched for download.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_discover(const mdl_args_t*     args,
                                       const mdl_run_opts_t* opts,
                                       const mdl_nums_t*     nums,
                                       const series_run_t*   base);
/**
 * @brief Create a starter descriptor for one validated site URL.
 * @details Derives the descriptor identity, joins it beneath the canonical
 *          composition-selected descriptor directory, and publishes through a
 *          validated create-new transaction.
 * @param[in] url Canonical input URL.
 * @param[in] descriptor_dir Canonical existing directory selected by the host
 *                           or firmware composition root.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared storage context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post Failure does not publish a partially written descriptor.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_init_site(const char* url, const char* descriptor_dir);
/**
 * @brief Verify one library or artifact tree without mutation.
 * @details Coordinates run verify with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] target_dir Directory tree to verify.
 * @return Process or helper status.
 * @retval 0 The operation completed.
 * @retval nonzero Validation or a dependency failed.
 * @pre Required pointer arguments remain valid for the call duration.
 * @pre The shared storage context is exclusively owned by the CLI thread.
 * @post No ownership of caller-provided storage is transferred.
 * @post The target tree and its artifacts remain unmodified.
 * @note The function performs no dynamic allocation and retains no caller
 * pointer.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_app_run_verify(const char* target_dir);
