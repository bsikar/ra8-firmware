/**
 * @file mdl_service.c
 * @brief ESP-IDF HTTP backend and synchronous CustomRpc hook for media chunks
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details The portable service and caller-owned buffers are bounded. The
 * concrete ESP-IDF adapter is deliberately fenced as a remaining allocator
 * exception: esp_http_client_set_url/open and TLS reconnects can allocate from
 * ESP-IDF heaps. Keeping one client handle avoids init/cleanup churn but does
 * not make this adapter zero-heap after initialisation. Replace only this
 * backend seam with a board-proven fixed-memory HTTPS mechanism.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_idf_mdl_compat_internal.h"
#include "ra8_attributes.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_mdl_service.h"

/** @brief Fixed HTTP policy values for the concrete ESP-IDF adapter. */
typedef enum : uint16_t {
  k_mdl_http_timeout_ms         = 15000U, /**< Per-operation HTTP timeout.        */
  k_mdl_http_status_success_min = 200U,   /**< Lowest successful HTTP status.     */
  k_mdl_http_status_redirect    = 300U,   /**< First redirect/non-success status. */
} mdl_http_const_t;

/**
 * @struct mdl_http_state
 * @brief Single retained ESP-IDF client and one serialized remote transfer
 * @invariant `http` and `sha` are initialised once before service dispatch.
 * @invariant `received` counts exactly the bytes supplied to the SHA context.
 */
typedef struct mdl_http_state {
  esp_http_client_handle_t http;                   /**< Retained ESP HTTP client handle.         */
  mbedtls_sha256_context   sha;                    /**< Retained streaming SHA context.          */
  char                     url[k_ra8_mdl_url_max]; /**< Bounded active HTTPS URL.                */
  uint64_t                 total;                  /**< Advertised body length, or zero.         */
  uint64_t                 received;               /**< Independently counted body bytes.        */
  bool                     total_known;            /**< Whether Content-Length was present.      */
  bool                     opened;                 /**< Whether response headers were accepted.  */
  bool                     hashing;                /**< Whether a SHA operation is active.       */
  bool                     client_ready;           /**< Whether one-time client setup succeeded. */
} mdl_http_state_t;

static mdl_http_state_t  s_http;
static ra8_mdl_service_t s_service;
static bool              s_initialised;

/**
 * @brief Return the first-party media component ABI marker
 * @return Stable component ABI version.
 * @retval 1 First version of the concrete media service component.
 * @pre The component object is strongly linked into the image.
 * @pre The caller does not replace this symbol with the upstream weak hook.
 * @post No global or peripheral state is modified.
 * @post The return value is stable for the component build.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
typedef uint32_t (*mdl_component_abi_fn_t)(void);

[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void)
{
  return 1U;
}

/* The volatile indirection is intentional: it keeps a relocation to the ABI
 * marker through optimization and --gc-sections, making the post-link symbol
 * assertion meaningful in release images. */
static mdl_component_abi_fn_t volatile s_component_abi = ra8_mdl_service_component_abi;

/**
 * @brief Close one request while retaining the one-time client/hash objects
 * @details Clears job metadata but deliberately does not call HTTP cleanup or
 * SHA free between Start operations.
 * @param[in,out] state Retained backend state.
 * @pre @p state is non-null and has completed one-time initialisation.
 * @pre The serialized RPC task exclusively owns @p state.
 * @post No active HTTP response or SHA operation remains.
 * @post The client and SHA objects remain initialised for a later Start.
 * @note Not thread-safe; the ESP-hosted control path serializes access.
 * @warning ESP-IDF close/reconnect internals may still release/allocate heap.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_http_reset_job(mdl_http_state_t* state)
{
  if (state->http != nullptr) {
    (void)esp_http_client_close(state->http); /* alloc-allow: ESP-IDF teardown */
  }
  state->url[0]      = '\0';
  state->total       = 0U;
  state->received    = 0U;
  state->total_known = false;
  state->opened      = false;
  state->hashing     = false;
}

/**
 * @brief Create the retained ESP-IDF client and SHA context once
 * @details This is the sole direct call to esp_http_client_init in the
 * component. Upstream still allocates internally during URL changes and TLS.
 * @param[in,out] state Zero-initialised backend state.
 * @return Initialisation status.
 * @retval k_ra8_ok Retained objects are ready.
 * @retval k_ra8_err_no_mem ESP-IDF could not create the client handle.
 * @pre @p state is non-null and not yet initialised.
 * @pre The serialized RPC task exclusively owns @p state.
 * @post Success sets `client_ready` and retains the HTTP/SHA objects.
 * @post Failure leaves `client_ready` false.
 * @note Not thread-safe; called once before portable service initialisation.
 * @warning This concrete adapter remains an ESP-IDF heap exception.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_init(mdl_http_state_t* state)
{
  const esp_http_client_config_t cfg = {
    .url                   = "https://127.0.0.1/",
    .timeout_ms            = k_mdl_http_timeout_ms,
    .buffer_size           = k_ra8_mdl_chunk_data_max,
    .crt_bundle_attach     = esp_crt_bundle_attach,
    .disable_auto_redirect = true,
  };
  state->http = esp_http_client_init(&cfg); /* alloc-allow: retained client init */
  if (state->http == nullptr) {
    return k_ra8_err_no_mem;
  }
  mbedtls_sha256_init(&state->sha);
  state->client_ready = true;
  return k_ra8_ok;
}

/**
 * @brief Validate and prepare one raw HTTPS-body job
 * @details Reuses retained client/hash objects and resets only per-job fields.
 * @param[in,out] ctx Initialised ::mdl_http_state_t.
 * @param[in] url NUL-terminated HTTPS URL within the fixed URL buffer.
 * @return Begin status.
 * @retval k_ra8_ok Job state and SHA stream are ready.
 * @retval k_ra8_err_invalid_arg URL is not a non-empty HTTPS URL.
 * @retval k_ra8_err_invalid_size URL exceeds the fixed buffer.
 * @retval k_ra8_fail ESP-IDF URL setup or SHA setup failed.
 * @pre @p ctx completed ::internal_mdl_http_init.
 * @pre No read callback executes concurrently.
 * @post Success leaves a closed client ready for lazy network open.
 * @post Failure leaves no active job.
 * @note Not thread-safe; service dispatch serializes calls.
 * @warning esp_http_client_set_url may allocate internally.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_begin(void* ctx, const char* url)
{
  mdl_http_state_t* state            = (mdl_http_state_t*)ctx;
  const size_t      https_prefix_len = sizeof("https://") - 1U;
  if ((state == nullptr) || !state->client_ready || (url == nullptr) ||
      (strncmp(url, "https://", https_prefix_len) != 0) || (url[https_prefix_len] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  internal_mdl_http_reset_job(state);
  const size_t len = strnlen(url, sizeof(state->url));
  if ((len == 0U) || (len >= sizeof(state->url))) {
    return k_ra8_err_invalid_size;
  }
  memcpy(state->url, url, len + 1U);
  if (esp_http_client_set_url(state->http, state->url) != ESP_OK) { /* alloc-allow: URL state */
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  if (mbedtls_sha256_starts(&state->sha, 0) != 0) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  state->hashing = true;
  return k_ra8_ok;
}

/**
 * @brief Open lazily so Start returns without waiting for network exchange
 * @details Accepts only successful HTTP status and records bounded body metadata.
 * @param[in,out] state Prepared job state.
 * @return Open status.
 * @retval k_ra8_ok Headers describe a successful HTTP response.
 * @retval k_ra8_fail ESP-IDF failed to open the HTTPS connection.
 * @retval k_ra8_err_protocol_error HTTP status is outside 200..299.
 * @pre @p state completed ::internal_mdl_http_begin successfully.
 * @pre No concurrent request uses the retained client.
 * @post Success marks the response opened and records advertised length.
 * @post Failure does not publish body bytes.
 * @note Not thread-safe; service dispatch serializes calls.
 * @warning TLS connection/reconnection may allocate internally.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_open(mdl_http_state_t* state)
{
  if (state->opened) {
    return k_ra8_ok;
  }
  if (esp_http_client_open(state->http, 0) != ESP_OK) { /* alloc-allow: TLS state */
    return k_ra8_fail;
  }
  const int64_t content_length = esp_http_client_fetch_headers(state->http);
  const int     status         = esp_http_client_get_status_code(state->http);
  if ((status < k_mdl_http_status_success_min) || (status >= k_mdl_http_status_redirect)) {
    return k_ra8_err_protocol_error;
  }
  state->total       = (content_length >= 0) ? (uint64_t)content_length : 0U;
  state->total_known = (content_length >= 0);
  state->opened      = true;
  return k_ra8_ok;
}

// Kept linear so EOF, truncation, hash finalisation, and reset remain one ordered state transition.
// NOLINTBEGIN(readability-function-size)
/**
 * @brief Pull one bounded body span or verified terminal metadata
 * @details Counts and hashes every returned byte and rejects truncated bodies.
 * @param[in,out] ctx Prepared ::mdl_http_state_t.
 * @param[out] out Caller-owned body-byte buffer.
 * @param[in] cap Capacity of @p out.
 * @param[out] got Exact body bytes returned.
 * @param[out] total_bytes Advertised or independently counted total.
 * @param[out] complete Whether this response is terminal.
 * @param[out] sha256 Complete-body digest when terminal.
 * @return Read status.
 * @retval k_ra8_ok Data or one verified terminal record is available.
 * @retval k_ra8_err_protocol_error HTTP status/body completion is incoherent.
 * @retval k_ra8_err_invalid_size Received-byte count overflowed.
 * @retval k_ra8_fail ESP-IDF or SHA processing failed.
 * @pre Every output pointer is non-null and @p cap is bounded by the protocol.
 * @pre ::internal_mdl_http_begin succeeded for this job.
 * @post A data response advances `received` by exactly @p got.
 * @post Terminal success closes job state after digest finalisation.
 * @note Not thread-safe; service dispatch serializes calls.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_read(void*     ctx,
                                                     uint8_t*  out,
                                                     uint16_t  cap,
                                                     uint16_t* got,
                                                     uint64_t* total_bytes,
                                                     bool*     complete,
                                                     uint8_t   sha256[k_ra8_mdl_sha256_bytes])
{
  *got                     = 0U;
  *total_bytes             = 0U;
  *complete                = false;
  mdl_http_state_t* state  = (mdl_http_state_t*)ctx;
  const ra8_err_t   opened = internal_mdl_http_open(state);
  if (opened != k_ra8_ok) {
    internal_mdl_http_reset_job(state);
    return opened;
  }
  const int read = esp_http_client_read(state->http, (char*)out, cap);
  if ((read < 0) || ((uint32_t)read > (uint32_t)cap)) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  if (read > 0) {
    if (state->received > (UINT64_MAX - (uint64_t)read)) {
      internal_mdl_http_reset_job(state);
      return k_ra8_err_invalid_size;
    }
    const uint64_t next_received = state->received + (uint64_t)read;
    if (state->total_known && (next_received > state->total)) {
      internal_mdl_http_reset_job(state);
      return k_ra8_err_protocol_error;
    }
    if (mbedtls_sha256_update(&state->sha, out, (size_t)read) != 0) {
      internal_mdl_http_reset_job(state);
      return k_ra8_fail;
    }
    state->received = next_received;
    *got            = (uint16_t)read;
    *total_bytes    = state->total;
  } else {
    const bool body_complete  = esp_http_client_is_complete_data_received(state->http);
    const bool length_matches = !state->total_known || (state->received == state->total);
    if ((!body_complete) || (!length_matches)) {
      internal_mdl_http_reset_job(state);
      return k_ra8_err_protocol_error;
    }
    if (mbedtls_sha256_finish(&state->sha, sha256) != 0) {
      internal_mdl_http_reset_job(state);
      return k_ra8_fail;
    }
    /* A close-delimited or chunked response has no advertised length. Publish
     * the independently counted length in its final COMPLETE response. */
    if (!state->total_known) {
      state->total = state->received;
    }
    *total_bytes = state->total;
    *complete    = true;
    internal_mdl_http_reset_job(state);
  }
  return k_ra8_ok;
}
// NOLINTEND(readability-function-size)

/**
 * @brief Cancel the active HTTP job while retaining one-time objects
 * @details Closes per-job state without destroying retained service objects.
 * @param[in,out] ctx Initialised ::mdl_http_state_t.
 * @return Cancellation status.
 * @retval k_ra8_ok Local job state was reset.
 * @pre @p ctx is non-null and exclusively owned.
 * @pre One-time client initialisation succeeded.
 * @post No active response or SHA operation remains.
 * @post Retained objects are ready for a later Start.
 * @note Not thread-safe; service dispatch serializes calls.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_cancel(void* ctx)
{
  internal_mdl_http_reset_job((mdl_http_state_t*)ctx);
  return k_ra8_ok;
}

/**
 * @brief Strong implementation consumed by the pinned ESP-hosted patch.
 *
 * @details The hook runs on ESP-hosted's serialized control path. It returns
 * a fully packed inner protobuf in caller-owned storage; the patched upstream
 * handler immediately wraps those bytes in its generated CustomRpc response.
 *
 * @param[in] message_id Stable media RPC operation identifier.
 * @param[in] request Packed generated inner protobuf.
 * @param[in] request_len Valid request bytes.
 * @param[out] response Caller-owned inner response buffer.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Packed response bytes.
 * @return ESP-hosted hook status.
 * @retval ESP_OK A valid inner response was produced.
 * @retval ESP_FAIL Component or one-time backend initialisation failed.
 * @pre ESP-hosted invokes the hook on its serialized control path.
 * @pre Request/response spans are valid and do not overlap.
 * @post Success sets @p response_len within @p response_cap.
 * @post Failure does not claim a successful CustomRpc response.
 * @note Not thread-safe outside the serialized ESP-hosted control task.
 * @warning The concrete ESP-IDF HTTP/TLS adapter remains a heap exception.
 * @since 0.1.0
 */
esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t       message_id,
                                             const uint8_t* request,
                                             size_t         request_len,
                                             uint8_t*       response,
                                             size_t         response_cap,
                                             size_t*        response_len)
{
  if (s_component_abi() != 1U) {
    return ESP_FAIL;
  }
  if (!s_initialised) {
    if (internal_mdl_http_init(&s_http) != k_ra8_ok) {
      return ESP_FAIL;
    }
    const ra8_mdl_service_backend_t backend = {.begin  = internal_mdl_http_begin,
                                               .read   = internal_mdl_http_read,
                                               .cancel = internal_mdl_http_cancel,
                                               .ctx    = &s_http};
    if (ra8_mdl_service_init(&s_service, &backend) != k_ra8_ok) {
      return ESP_FAIL;
    }
    s_initialised = true;
  }
  const ra8_err_t result = ra8_mdl_service_dispatch(&s_service,
                                                    message_id,
                                                    request,
                                                    request_len,
                                                    response,
                                                    response_cap,
                                                    response_len);
  return (result == k_ra8_ok) ? ESP_OK : (esp_err_t)result;
}
