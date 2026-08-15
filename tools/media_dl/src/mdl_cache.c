/**
 * @file mdl_cache.c
 * @brief Per-host HTTP cache lookup and revalidation state machine.
 * @details Coordinates verified body reuse, conditional requests, corruption
 *          recovery, and transactional index publication over injected seams.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_cache.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "mdl_cache_internal.h"
#include "mdl_hash.h"
#include "ra8_attributes.h"

/** @brief HTTP response bounds used by the cache state machine. */
typedef enum : uint16_t {
  k_cache_http_success_min  = 200U, /**< First successful status.     */
  k_cache_http_success_max  = 299U, /**< Last successful status.      */
  k_cache_http_not_modified = 304U, /**< Conditional reuse response. */
  k_cache_http_status_max   = 599U, /**< Last canonical HTTP status.  */
} mdl_cache_http_t;

/**
 * @brief Locate one exact URL record.
 * @param[in,out] index Loaded host index.
 * @param[in] url Exact URL.
 * @param[in] url_hash Precomputed URL hash.
 * @return Matching mutable record or NULL.
 * @pre Both pointers are non-NULL.
 * @pre Record count is within capacity.
 * @post No record is modified.
 * @post A hash collision cannot match a different URL.
 * @note Linear bounded lookup keeps the persistent format simple.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_cache_record_t*
internal_cache_find(mdl_cache_index_t* index, const char* url, uint64_t url_hash)
{
  for (uint16_t i = 0U; i < index->record_count; ++i) {
    if ((index->records[i].url_hash == url_hash) && (strcmp(index->records[i].url, url) == 0)) {
      return &index->records[i];
    }
  }
  return nullptr;
}

/**
 * @brief Select a record slot, evicting the oldest observation when full.
 * @param[in,out] index Loaded host index.
 * @return Writable record slot.
 * @pre @p index is non-NULL and valid.
 * @pre Capacity is nonzero.
 * @post A spare slot increases record count exactly once.
 * @post A full index returns the oldest slot without changing count.
 * @note Ties select the first oldest row deterministically.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_cache_record_t* internal_cache_slot(mdl_cache_index_t* index)
{
  if (index->record_count < (uint16_t)k_mdl_cache_record_max) {
    mdl_cache_record_t* slot = &index->records[index->record_count];
    index->record_count += 1U;
    return slot;
  }
  uint16_t oldest = 0U;
  for (uint16_t i = 1U; i < index->record_count; ++i) {
    if (index->records[i].fetched_at < index->records[oldest].fetched_at) {
      oldest = i;
    }
  }
  return &index->records[oldest];
}

/**
 * @brief Compute a nonnegative retained-observation age.
 * @details Compares the retained epoch with the current wall clock while
 *          treating an unavailable clock or timestamp as unknown metadata.
 * @param[in] fetched_at Retained epoch seconds.
 * @return Age in seconds, or -1 when the clock/record is unavailable.
 * @retval -1 The clock or retained timestamp is unavailable.
 * @retval nonnegative The retained observation age in seconds.
 * @pre Integer input uses its declared width.
 * @pre The system clock may be unavailable.
 * @post No global or caller state is modified.
 * @post A clock rollback reports zero rather than a negative age.
 * @note Used for truthful CLI staleness reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_cache_age(int64_t fetched_at)
{
  const time_t now = time(nullptr);
  if ((fetched_at < 0) || (now < (time_t)0)) {
    return -1;
  }
  const int64_t current = (int64_t)now;
  return (current >= fetched_at) ? (current - fetched_at) : 0;
}

/**
 * @brief Capture the current epoch for one response observation.
 * @details Converts the process wall clock to the signed persistent field and
 *          substitutes zero only when the platform reports no usable time.
 * @return Nonnegative epoch seconds, or zero when unavailable.
 * @retval 0 The platform clock is unavailable or is exactly the epoch.
 * @retval positive Current epoch seconds.
 * @pre The platform time provider is initialized when required.
 * @pre Return zero is accepted as an unknown-but-valid epoch.
 * @post No cache or storage state is modified.
 * @post Result is always nonnegative.
 * @note Wall time is metadata, never a security decision.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_cache_now(void)
{
  const time_t now = time(nullptr);
  return (now < (time_t)0) ? 0 : (int64_t)now;
}

/**
 * @brief Copy a complete bounded string without truncation.
 * @details Measures before copying so an oversized validator or URL cannot
 *          leave a truncated identity in persistent state.
 * @param[out] destination Writable destination.
 * @param[in] capacity Destination byte capacity.
 * @param[in] source NUL-terminated source.
 * @return Whether the complete source fit.
 * @retval true The exact source and terminating NUL fit.
 * @retval false The destination was cleared because the source was oversized.
 * @pre All pointers are non-NULL and capacity is nonzero.
 * @pre @p capacity is the true destination extent.
 * @post Success leaves an exact NUL-terminated copy.
 * @post Failure clears the destination.
 * @note Performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cache_copy(char* destination, size_t capacity, const char* source)
{
  const size_t length = strlen(source);
  if (length >= capacity) {
    destination[0] = '\0';
    return false;
  }
  memcpy(destination, source, length + 1U);
  return true;
}

/**
 * @brief Update or insert one freshly published body record.
 * @details Builds the complete next record off to the side, then atomically
 *          assigns it to the existing or deterministically selected slot.
 * @param[in,out] index Loaded host index.
 * @param[in] existing Existing exact URL record, or NULL.
 * @param[in] url Exact URL.
 * @param[in] url_hash URL identity.
 * @param[in] content_hash Body identity.
 * @param[in] relative_path Published body leaf.
 * @param[in] response Finished successful response.
 * @return Whether the complete record fit.
 * @retval true Every string fit and one record was updated.
 * @retval false At least one string exceeded its persistent field.
 * @pre All required pointers are non-NULL.
 * @pre Response status is a canonical successful code.
 * @post Success publishes one complete in-memory record.
 * @post Failure leaves an existing record unchanged.
 * @note A full index evicts its oldest observation.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cache_record_response(mdl_cache_index_t*    index,
                                                        mdl_cache_record_t*   existing,
                                                        const char*           url,
                                                        uint64_t              url_hash,
                                                        uint64_t              content_hash,
                                                        const char*           relative_path,
                                                        const mdl_net_resp_t* response)
{
  mdl_cache_record_t next = {.url_hash        = url_hash,
                             .content_hash    = content_hash,
                             .fetched_at      = internal_cache_now(),
                             .response_status = (uint16_t)response->status};
  if (!internal_cache_copy(next.url, sizeof(next.url), url) ||
      !internal_cache_copy(next.relative_path, sizeof(next.relative_path), relative_path) ||
      !internal_cache_copy(next.etag, sizeof(next.etag), response->etag) ||
      !internal_cache_copy(next.last_modified,
                           sizeof(next.last_modified),
                           response->last_modified)) {
    return false;
  }
  mdl_cache_record_t* destination = (existing != nullptr) ? existing : internal_cache_slot(index);
  *destination                    = next;
  return true;
}

/**
 * @brief Issue a cache-controlled request with selected validators.
 * @details Copies the request template, attaches only validators belonging to
 *          the hash-verified record, and delegates one bounded fetch.
 * @param[in] record Verified held record, or NULL.
 * @param[in] url Exact URL.
 * @param[in] base_request Caller request template.
 * @param[in] fetch Injected fetch callback.
 * @param[in,out] fetch_context Callback context.
 * @param[out] buffer Body destination.
 * @param[in] capacity Body capacity.
 * @param[out] out_length Response body extent.
 * @param[out] response Finished response metadata.
 * @return Canonical callback status.
 * @retval k_ra8_ok The callback completed and initialized response metadata.
 * @retval other The injected callback rejected or failed the request.
 * @pre Every required pointer is non-NULL.
 * @pre @p record is hash-verified when non-NULL.
 * @post Saved validators are sent exactly when available.
 * @post Base request remains unchanged.
 * @note Refetch forces this call but still permits conditional revalidation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_fetch(const mdl_cache_record_t* record,
                                                   const char*               url,
                                                   const mdl_net_req_t*      base_request,
                                                   mdl_cache_fetch_fn        fetch,
                                                   void*                     fetch_context,
                                                   char*                     buffer,
                                                   size_t                    capacity,
                                                   size_t*                   out_length,
                                                   mdl_net_resp_t*           response)
{
  mdl_net_req_t request = *base_request;
  request.if_none_match =
    ((record != nullptr) && (record->etag[0] != '\0')) ? record->etag : nullptr;
  request.if_modified_since =
    ((record != nullptr) && (record->last_modified[0] != '\0')) ? record->last_modified : nullptr;
  *out_length = 0U;
  *response   = (mdl_net_resp_t){};
  return fetch(fetch_context, url, &request, buffer, capacity, out_length, response);
}

/**
 * @struct mdl_cache_lookup_t
 * @brief Prepared host paths and verified retained-entity state.
 * @since 0.1.0
 */
typedef struct {
  mdl_cache_paths_t   paths;           /**< Derived host namespace.      */
  mdl_cache_record_t* record;          /**< Exact URL record, or NULL.   */
  size_t              retained_length; /**< Verified retained body size. */
  bool                held;            /**< Body exists and hash checks. */
} mdl_cache_lookup_t;

/**
 * @brief Load one host index and verify any retained exact-URL entity.
 * @details Authenticates the index, matches both URL hash and exact URL bytes,
 *          then accepts a body only after exact size and content-hash checks.
 * @param[in,out] cache Cache binding.
 * @param[in] url Exact URL.
 * @param[out] buffer Body destination used for retained verification.
 * @param[in] capacity Body destination capacity.
 * @param[in,out] result Cache outcome receiving load observations.
 * @param[out] lookup Prepared lookup state.
 * @return Canonical index-load status.
 * @retval k_ra8_ok Lookup state is complete; the body may still be absent.
 * @retval other Host path, index recovery, or storage inspection failed.
 * @pre Every pointer is non-NULL and @p capacity is nonzero.
 * @pre Cache storage and workspace are exclusively owned.
 * @post Success initializes @p lookup completely.
 * @post A held entity has already passed exact size/hash validation.
 * @note A missing or corrupt body is treated as absent and fetched again.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_prepare(mdl_cache_t*        cache,
                                                     const char*         url,
                                                     char*               buffer,
                                                     size_t              capacity,
                                                     mdl_cache_result_t* result,
                                                     mdl_cache_lookup_t* lookup)
{
  *lookup         = (mdl_cache_lookup_t){};
  ra8_err_t error = priv_mdl_cache_load(cache, url, &lookup->paths, &result->index_rebuilt);
  if (error != k_ra8_ok) {
    return error;
  }
  const uint64_t url_hash = mdl_hash_str(url);
  lookup->record          = internal_cache_find(cache->index, url, url_hash);
  lookup->held =
    (lookup->record != nullptr) && (priv_mdl_cache_read_body(cache->storage,
                                                             &lookup->paths,
                                                             lookup->record,
                                                             buffer,
                                                             capacity,
                                                             &lookup->retained_length) == k_ra8_ok);
  if (lookup->held) {
    result->age_seconds     = internal_cache_age(lookup->record->fetched_at);
    result->observed_status = lookup->record->response_status;
  }
  return k_ra8_ok;
}

/**
 * @brief Retry one invalid 304 without conditional headers.
 * @details Clears both conditional fields on a copy of the request so a server
 *          cannot strand an empty cache in a repeated not-modified loop.
 * @param[in] url Exact URL.
 * @param[in] base_request Caller request template.
 * @param[in] fetch Injected fetch callback.
 * @param[in,out] fetch_context Callback context.
 * @param[out] buffer Body destination.
 * @param[in] capacity Body capacity.
 * @param[out] out_length Response body extent.
 * @param[out] response Finished response metadata.
 * @return Canonical callback or protocol status.
 * @retval k_ra8_ok The unconditional retry returned a usable response.
 * @retval k_ra8_err_protocol_error The unconditional retry also returned 304.
 * @retval other The injected callback failed.
 * @pre Every required pointer is non-NULL.
 * @pre No valid cached entity is available.
 * @post A second 304 is rejected as protocol failure.
 * @post No storage mutation occurs.
 * @note This prevents a permanent 304 loop after body loss.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_retry_unconditional(const char*          url,
                                                                 const mdl_net_req_t* base_request,
                                                                 mdl_cache_fetch_fn   fetch,
                                                                 void*                fetch_context,
                                                                 char*                buffer,
                                                                 size_t               capacity,
                                                                 size_t*              out_length,
                                                                 mdl_net_resp_t*      response)
{
  mdl_net_req_t request     = *base_request;
  request.if_none_match     = nullptr;
  request.if_modified_since = nullptr;
  *out_length               = 0U;
  *response                 = (mdl_net_resp_t){};
  const ra8_err_t error =
    fetch(fetch_context, url, &request, buffer, capacity, out_length, response);
  return ((error == k_ra8_ok) && (response->status == (long)k_cache_http_not_modified))
           ? k_ra8_err_protocol_error
           : error;
}

/**
 * @brief Finish a verified HTTP 304 observation.
 * @details Rereads the immutable retained body after the network callback,
 *          updates only the observation metadata, and publishes a new index.
 * @param[in,out] cache Cache binding.
 * @param[in] paths Host paths.
 * @param[in,out] record Verified retained record.
 * @param[out] buffer Body destination.
 * @param[in] capacity Body capacity.
 * @param[out] out_length Exact retained body extent.
 * @param[in,out] result Outcome to update.
 * @return Canonical body-read or index-save status.
 * @retval k_ra8_ok The retained bytes and refreshed index are complete.
 * @retval other Body verification or index publication failed.
 * @pre Every pointer is non-NULL.
 * @pre @p record previously hash-verified.
 * @post Success returns the body and records status 304/time.
 * @post No body file is opened for writing.
 * @note The body is reread because a network backend may touch its buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_finish_304(mdl_cache_t*             cache,
                                                        const mdl_cache_paths_t* paths,
                                                        mdl_cache_record_t*      record,
                                                        char*                    buffer,
                                                        size_t                   capacity,
                                                        size_t*                  out_length,
                                                        mdl_cache_result_t*      result)
{
  ra8_err_t error =
    priv_mdl_cache_read_body(cache->storage, paths, record, buffer, capacity, out_length);
  if (error != k_ra8_ok) {
    return error;
  }
  record->fetched_at      = internal_cache_now();
  record->response_status = (uint16_t)k_cache_http_not_modified;
  error                   = priv_mdl_cache_save(cache, paths);
  if (error == k_ra8_ok) {
    result->body_reused     = true;
    result->observed_status = (uint16_t)k_cache_http_not_modified;
  }
  return error;
}

/**
 * @brief Publish and index one successful nonempty response body.
 * @details Derives an immutable content-addressed leaf, publishes the body,
 *          then replaces the host index only after the record is complete.
 * @param[in,out] cache Cache binding.
 * @param[in] paths Host paths.
 * @param[in,out] existing Exact prior record, or NULL.
 * @param[in] url Exact URL.
 * @param[in] buffer Complete response body.
 * @param[in] length Body extent.
 * @param[in] response Successful response metadata.
 * @param[in,out] result Outcome to update.
 * @return Canonical validation or publication status.
 * @retval k_ra8_ok Body and index generation were published.
 * @retval k_ra8_err_protocol_error The response status/body was unusable.
 * @retval other A bound, storage, or encoding operation failed.
 * @pre Every required pointer is non-NULL and body is nonempty.
 * @pre Response status is available.
 * @post Success publishes immutable body bytes and one index generation.
 * @post Index failure cannot invalidate the prior indexed body.
 * @note New body names include both URL and content identities.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_publish(mdl_cache_t*             cache,
                                                     const mdl_cache_paths_t* paths,
                                                     mdl_cache_record_t*      existing,
                                                     const char*              url,
                                                     const char*              buffer,
                                                     size_t                   length,
                                                     const mdl_net_resp_t*    response,
                                                     mdl_cache_result_t*      result)
{
  if ((response->status < (long)k_cache_http_success_min) ||
      (response->status > (long)k_cache_http_success_max) ||
      (response->status > (long)k_cache_http_status_max) || (length == 0U)) {
    return k_ra8_err_protocol_error;
  }
  const uint64_t url_hash     = mdl_hash_str(url);
  const uint64_t content_hash = mdl_hash_bytes(buffer, length);
  char           relative[k_mdl_relpath_max];
  ra8_err_t      error = priv_mdl_cache_publish_body(cache->storage,
                                                     paths,
                                                     url_hash,
                                                     content_hash,
                                                     buffer,
                                                     length,
                                                     relative,
                                                     sizeof(relative));
  if (error != k_ra8_ok) {
    return error;
  }
  if (!internal_cache_record_response(cache->index,
                                      existing,
                                      url,
                                      url_hash,
                                      content_hash,
                                      relative,
                                      response)) {
    return k_ra8_err_invalid_size;
  }
  error = priv_mdl_cache_save(cache, paths);
  if (error == k_ra8_ok) {
    result->observed_status = (uint16_t)response->status;
  }
  return error;
}

/**
 * @brief Fetch, resolve 304 semantics, and publish one network observation.
 * @details Runs exactly one conditional fetch, branches to verified 304 reuse
 *          or a single unconditional recovery, then publishes a new 2xx body.
 * @param[in,out] cache Cache binding.
 * @param[in] lookup Prepared retained-entity state.
 * @param[in] url Exact URL.
 * @param[in] base_request Caller request template.
 * @param[in] fetch Injected fetch callback.
 * @param[in,out] fetch_context Callback context.
 * @param[out] buffer Body destination.
 * @param[in] capacity Body capacity.
 * @param[out] out_length Returned body extent.
 * @param[out] response Finished response metadata.
 * @param[in,out] result Cache outcome.
 * @return Canonical callback, protocol, or publication status.
 * @retval k_ra8_ok A retained or new body is available.
 * @retval other The callback, 304 recovery, or publication failed.
 * @pre Every pointer is non-NULL and lookup was prepared successfully.
 * @pre Cache storage and body destination are exclusively owned.
 * @post Success returns either a verified retained or newly published body.
 * @post An invalid body-free 304 receives at most one unconditional retry.
 * @note A valid 304 never opens a body transaction for writing.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_network(mdl_cache_t*              cache,
                                                     const mdl_cache_lookup_t* lookup,
                                                     const char*               url,
                                                     const mdl_net_req_t*      base_request,
                                                     mdl_cache_fetch_fn        fetch,
                                                     void*                     fetch_context,
                                                     char*                     buffer,
                                                     size_t                    capacity,
                                                     size_t*                   out_length,
                                                     mdl_net_resp_t*           response,
                                                     mdl_cache_result_t*       result)
{
  result->revalidated = true;
  ra8_err_t error     = internal_cache_fetch(lookup->held ? lookup->record : nullptr,
                                             url,
                                             base_request,
                                             fetch,
                                             fetch_context,
                                             buffer,
                                             capacity,
                                             out_length,
                                             response);
  if (error != k_ra8_ok) {
    return error;
  }
  if (response->status == (long)k_cache_http_not_modified) {
    if (lookup->held) {
      return internal_cache_finish_304(cache,
                                       &lookup->paths,
                                       lookup->record,
                                       buffer,
                                       capacity,
                                       out_length,
                                       result);
    }
    error = internal_cache_retry_unconditional(url,
                                               base_request,
                                               fetch,
                                               fetch_context,
                                               buffer,
                                               capacity,
                                               out_length,
                                               response);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return internal_cache_publish(cache,
                                &lookup->paths,
                                lookup->record,
                                url,
                                buffer,
                                *out_length,
                                response,
                                result);
}

ra8_err_t mdl_cache_get_buf(mdl_cache_t*         cache,
                            const char*          url,
                            const mdl_net_req_t* base_request,
                            mdl_cache_fetch_fn   fetch,
                            void*                fetch_context,
                            char*                buffer,
                            size_t               capacity,
                            size_t*              out_length,
                            mdl_net_resp_t*      response,
                            mdl_cache_result_t*  result)
{
  if ((cache == nullptr) || (cache->storage == nullptr) || (cache->index == nullptr) ||
      (cache->root == nullptr) || (url == nullptr) || (base_request == nullptr) ||
      (fetch == nullptr) || (buffer == nullptr) || (capacity == 0U) || (out_length == nullptr) ||
      (response == nullptr) || (result == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *out_length = 0U;
  *response   = (mdl_net_resp_t){};
  *result     = (mdl_cache_result_t){.age_seconds = -1};
  mdl_cache_lookup_t lookup;
  ra8_err_t          error = internal_cache_prepare(cache, url, buffer, capacity, result, &lookup);
  if (error != k_ra8_ok) {
    return error;
  }
  const bool validators =
    lookup.held && ((lookup.record->etag[0] != '\0') || (lookup.record->last_modified[0] != '\0'));
  if (lookup.held && !cache->refetch && !validators) {
    *out_length         = lookup.retained_length;
    result->body_reused = true;
    return k_ra8_ok;
  }
  return internal_cache_network(cache,
                                &lookup,
                                url,
                                base_request,
                                fetch,
                                fetch_context,
                                buffer,
                                capacity,
                                out_length,
                                response,
                                result);
}
