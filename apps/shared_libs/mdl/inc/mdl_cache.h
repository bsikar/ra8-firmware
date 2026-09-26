/**
 * @file mdl_cache.h
 * @brief Bounded per-host HTTP document cache for the media downloader.
 * @details Stores exact URL identities, validators, response observations, and
 *          content hashes in one versioned index per host. Bodies and indexes
 *          publish through injected storage transactions; no ownership or host
 *          filesystem API crosses this interface.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_config.h"
#include "mdl_net.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

/** @brief Persistent cache schema and fixed-capacity record bound. */
typedef enum : uint16_t {
  k_mdl_cache_schema_version = 1U,   /**< Current binary index schema. */
  k_mdl_cache_record_max     = 128U, /**< Records retained per host.   */
} mdl_cache_limit_t;

/**
 * @struct mdl_cache_record_t
 * @brief One exact URL-keyed cached document observation.
 * @details The URL is retained in full so a non-cryptographic hash collision
 *          cannot select another document.
 * @invariant Strings are NUL-terminated and contain no line-control bytes.
 * @since 0.1.0
 */
typedef struct {
  char     url[k_mdl_url_max];                /**< Exact canonical request URL.    */
  char     relative_path[k_mdl_relpath_max];  /**< Body leaf beneath the host dir. */
  char     etag[k_mdl_etag_max];              /**< Last response ETag, or empty.   */
  char     last_modified[k_mdl_last_mod_max]; /**< Last-Modified, or empty.        */
  uint64_t url_hash;                          /**< FNV identity accelerator.       */
  uint64_t content_hash;                      /**< Exact persisted body identity.  */
  int64_t  fetched_at;                        /**< Completion epoch seconds.       */
  uint16_t response_status;                   /**< Last observed HTTP status.      */
} mdl_cache_record_t;

/**
 * @struct mdl_cache_index_t
 * @brief Caller-owned workspace for one loaded host index.
 * @invariant `record_count <= k_mdl_cache_record_max`.
 * @since 0.1.0
 */
typedef struct {
  mdl_cache_record_t records[k_mdl_cache_record_max]; /**< Retained observations. */
  uint64_t           host_hash;                       /**< Bound host identity.   */
  uint16_t           schema_version;                  /**< Binary schema version. */
  uint16_t           record_count;                    /**< Populated rows.        */
} mdl_cache_index_t;

/**
 * @brief Injected bounded GET callback used by the cache.
 * @details Callers may dispatch directly to a network backend or wrap the call
 *          in a politeness governor and retry policy.
 * @param[in,out] context Caller-owned fetch context.
 * @param[in] url Exact absolute request URL.
 * @param[in] request Request headers and timeout.
 * @param[out] buffer Destination body storage.
 * @param[in] capacity Writable bytes at @p buffer.
 * @param[out] out_length Body bytes written.
 * @param[out] response Finished response metadata.
 * @return Canonical network or policy status.
 * @pre Every pointer is non-NULL and @p capacity is nonzero.
 * @post Success initializes @p out_length and @p response.
 * @note The callback retains no argument pointer.
 * @since 0.1.0
 */
typedef ra8_err_t (*mdl_cache_fetch_fn)(void*                context,
                                        const char*          url,
                                        const mdl_net_req_t* request,
                                        char*                buffer,
                                        size_t               capacity,
                                        size_t*              out_length,
                                        mdl_net_resp_t*      response);

/**
 * @struct mdl_cache_t
 * @brief One non-reentrant cache binding over caller storage.
 * @invariant @ref root is a canonical absolute path.
 * @since 0.1.0
 */
typedef struct {
  mdl_storage_t*     storage;    /**< Injected portable storage binding. */
  mdl_cache_index_t* index;      /**< Caller-owned index workspace.      */
  const char*        root;       /**< Canonical cache root.              */
  ra8_io_stream_t*   diagnostic; /**< Optional cache event sink.         */
  bool               refetch;    /**< Force a network revalidation.      */
} mdl_cache_t;

/**
 * @struct mdl_cache_result_t
 * @brief Observable outcome of one cache lookup.
 * @since 0.1.0
 */
typedef struct {
  int64_t  age_seconds;     /**< Age before this operation, or -1 if unknown. */
  uint16_t observed_status; /**< Network or retained status.                  */
  bool     body_reused;     /**< Returned bytes came from verified storage.   */
  bool     revalidated;     /**< A network request was issued.                */
  bool     index_rebuilt;   /**< A corrupt index was discarded first.         */
} mdl_cache_result_t;

/**
 * @struct mdl_cache_get_req_t
 * @brief One cache lookup request, as a config struct rather than an argument list.
 * @details Carries the request identity, the injected fetch dependency, and the
 *          caller-owned body destination. Every field is borrowed for the
 *          duration of one ::mdl_cache_get call and retained afterwards by
 *          neither the cache nor the callback.
 * @invariant @ref url and @ref request describe the same single document.
 * @since 0.1.0
 */
typedef struct {
  const char*          url;           /**< Exact absolute request URL.         */
  const mdl_net_req_t* request;       /**< Request identity, referer, timeout. */
  mdl_cache_fetch_fn   fetch;         /**< Injected network/governor callback. */
  void*                fetch_context; /**< Context supplied to @ref fetch.     */
  char*                buffer;        /**< Caller-owned body destination.      */
  size_t               capacity;      /**< Writable bytes at @ref buffer.      */
} mdl_cache_get_req_t;

/**
 * @struct mdl_cache_get_out_t
 * @brief Everything one cache lookup reports back.
 * @details Groups the three outputs the lookup used to write through three
 *          separate pointers, so a caller declares one object and a failure
 *          path cannot leave two of the three uninitialized.
 * @invariant @ref length is zero unless the call returned ::k_ra8_ok.
 * @since 0.1.0
 */
typedef struct {
  mdl_net_resp_t     response; /**< Finished network or retained metadata. */
  mdl_cache_result_t result;   /**< Cache-specific outcome.                */
  size_t             length;   /**< Exact returned body bytes.             */
} mdl_cache_get_out_t;

/**
 * @brief Fetch one document through the per-host persistent cache.
 * @details Verifies a retained body before reuse, sends saved validators when
 *          available, treats HTTP 304 as a body-free observation, and retries
 *          an unexpected 304 once without validators when no valid entity is
 *          held. Successful new bodies and index generations publish
 *          transactionally. A malformed/truncated index is removed and
 *          rebuilt rather than trusted or treated as library state.
 * @param[in,out] cache Initialized cache binding.
 * @param[in] req Complete lookup request.
 * @param[out] out Body extent, response metadata, and cache outcome.
 * @return Canonical cache, storage, network, or validation status.
 * @retval k_ra8_ok A nonempty verified body is in `req->buffer`.
 * @retval k_ra8_err_invalid_arg A binding, path, callback, or URL is invalid.
 * @retval k_ra8_err_invalid_size The response or persistent record is oversized.
 * @retval other The injected storage or fetch operation failed.
 * @pre @p cache and @p req are fully populated and @p cache is exclusively owned.
 * @pre `req->buffer` spans `req->capacity` nonzero writable bytes.
 * @post Every @p out field is initialized before the first failure can return.
 * @post Success returns complete bytes and a truthful cache result.
 * @post Failure never publishes a partial body or index generation.
 * @note Thread-safe across independent bindings and storage workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_cache_get(mdl_cache_t*               cache,
                                      const mdl_cache_get_req_t* req,
                                      mdl_cache_get_out_t*       out);
