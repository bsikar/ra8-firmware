/**
 * @file mdl_app_internal.h
 * @brief Module-private contract shared by the application-mode translation
 *        units.
 * @details The bounded sizing every mode was written against, the accessor for
 *          the one bound working set, and the helpers the modes share across
 *          translation units. Nothing here names a host facility: the transport
 *          arrives as ::mdl_net_provider_t, the filesystem as ::mdl_storage_t
 *          and the sinks as ::ra8_io_stream_t, which is what lets these units
 *          compile in a form that has no libcurl and no POSIX.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mdl_app.h"
#include "mdl_app_storage_internal.h"
#include "mdl_cache.h"
#include "mdl_config.h"
#include "mdl_discover.h"
#include "mdl_export.h"
#include "mdl_extract.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_hash.h"
#include "mdl_library.h"
#include "mdl_net.h"
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

/** @brief Polite per-image delay floors for page mode (milliseconds). */
typedef enum : uint16_t {
  k_polite_img_min_ms = 2000, /**< Polite per-image floor.   */
  k_polite_img_max_ms = 4000, /**< Polite per-image ceiling. */
} mdl_polite_floor_t;

/**
 * @brief The working set the composition root bound with ::mdl_app_bind.
 * @details The one way an application mode reaches shared bounded state. It is
 *          a function rather than an extern object so the storage stays owned
 *          by the form and this layer keeps no global of its own.
 * @return The bound context.
 * @retval non-NULL The context most recently passed to ::mdl_app_bind.
 * @retval nullptr No form has bound a context yet.
 * @pre A form has called ::mdl_app_bind for any mode to be usable.
 * @pre The caller does not retain the pointer past the next bind.
 * @post No context state is modified.
 * @post Successive calls return the same pointer until the next bind.
 * @note Not thread-safe against a concurrent ::mdl_app_bind.
 * @since 0.1.0
 */
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
 * @note Not thread-safe because it uses the bound application context.
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
 * @note Not thread-safe because it uses the bound application context.
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
 * @pre The bound application context is exclusively owned by this thread.
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
 * @details Builds the identifying user agent in @p ua, applies contact and run
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
