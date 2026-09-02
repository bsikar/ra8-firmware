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
  k_mdl_http_timeout_ms = 15000U, /**< Default whole-request timeout. */
  k_mdl_http_status_min = 100U,   /**< Lowest valid terminal status.  */
  k_mdl_http_status_max = 599U,   /**< Highest valid terminal status. */
} mdl_http_const_t;

/**
 * @struct mdl_http_state
 * @brief Single retained ESP-IDF client and one serialized remote transfer
 * @invariant `http` and `sha` are initialised once before service dispatch.
 * @invariant `received` counts exactly the bytes supplied to the SHA context.
 */
typedef struct mdl_http_state {
  esp_http_client_handle_t http;                   /**< Retained ESP HTTP client handle.        */
  mbedtls_sha256_context   sha;                    /**< Retained streaming SHA context.         */
  char                     url[k_ra8_mdl_url_max]; /**< Bounded active HTTPS URL.               */
  char user_agent[k_ra8_mdl_user_agent_max];       /**< Active User-Agent request value.        */
  char referer[k_ra8_mdl_referer_max];             /**< Active Referer request value.           */
  char if_none_match[k_ra8_mdl_etag_max];          /**< Active If-None-Match request value.     */
  char if_modified_since[k_ra8_mdl_http_date_max]; /**< Active If-Modified-Since request value. */
  ra8_mdl_http_response_t response;                /**< Selected terminal response metadata.    */
  ra8_err_t               header_error;            /**< First response-header capture failure.  */
  uint64_t                total;                   /**< Advertised body length, or zero.        */
  uint64_t                received;                /**< Independently counted body bytes.       */
  mdl_format_t            format;                  /**< Requested artifact identity.            */
  bool                    total_known;             /**< Whether Content-Length was present.     */
  bool                    opened;                  /**< Whether response headers were accepted. */
  bool                    hashing;                 /**< Whether a SHA operation is active.      */
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
    (void)esp_http_client_close( // alloc-allow: ESP-IDF SOUP releases transport state
      state->http);
  }
  state->url[0]               = '\0';
  state->user_agent[0]        = '\0';
  state->referer[0]           = '\0';
  state->if_none_match[0]     = '\0';
  state->if_modified_since[0] = '\0';
  state->response             = (ra8_mdl_http_response_t){};
  state->header_error         = k_ra8_ok;
  state->total                = 0U;
  state->received             = 0U;
  state->format               = k_mdl_format_invalid;
  state->total_known          = false;
  state->opened               = false;
  state->hashing              = false;
}

/**
 * @brief Compare one HTTP header name without ASCII case sensitivity
 * @details Folds only the ASCII uppercase range because HTTP field names are
 * ASCII tokens and locale-dependent case conversion is forbidden here.
 * @param[in] lhs Candidate response-header name.
 * @param[in] rhs Canonical response-header name.
 * @return Whether both NUL-terminated names are equal ignoring ASCII case.
 * @retval true The names are equal.
 * @retval false The names differ or @p lhs is null.
 * @pre @p rhs is non-null and NUL-terminated.
 * @pre Non-null @p lhs is NUL-terminated.
 * @pre Header names contain only ASCII octets.
 * @post No input or service state is modified.
 * @post The result is independent of the process locale.
 * @note HTTP field names are case-insensitive.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_header_equal(const char* lhs, const char* rhs)
{
  if (lhs == nullptr) {
    return false;
  }
  size_t index = 0U;
  while ((lhs[index] != '\0') && (rhs[index] != '\0')) {
    unsigned char left  = (unsigned char)lhs[index];
    unsigned char right = (unsigned char)rhs[index];
    if ((left >= (unsigned char)'A') && (left <= (unsigned char)'Z')) {
      left = (unsigned char)(left + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if ((right >= (unsigned char)'A') && (right <= (unsigned char)'Z')) {
      right = (unsigned char)(right + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if (left != right) {
      return false;
    }
    index++;
  }
  return (lhs[index] == '\0') && (rhs[index] == '\0');
}

/**
 * @brief Copy one optional bounded HTTP field into retained storage
 * @details Canonicalizes an absent value to empty, rejects line breaks, and
 * retains one exact NUL-terminated value for the synchronous exchange.
 * @param[out] destination Fixed-capacity destination.
 * @param[in] capacity Total destination capacity including NUL.
 * @param[in] source Optional NUL-terminated field value.
 * @return Copy status.
 * @retval k_ra8_ok The value or canonical empty string was copied.
 * @retval k_ra8_err_invalid_arg The value contains CR or LF.
 * @retval k_ra8_err_invalid_size The value does not fit.
 * @pre @p destination is writable for @p capacity bytes.
 * @pre @p capacity is positive.
 * @post Success leaves @p destination NUL-terminated.
 * @post Failure leaves @p destination as an empty string.
 * @note Null and empty inputs both represent an absent field.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_copy_field(char* destination, size_t capacity, const char* source)
{
  destination[0] = '\0';
  if ((source == nullptr) || (source[0] == '\0')) {
    return k_ra8_ok;
  }
  const size_t length = strnlen(source, capacity);
  if (length >= capacity) {
    return k_ra8_err_invalid_size;
  }
  for (size_t i = 0U; i < length; ++i) {
    if ((source[i] == '\r') || (source[i] == '\n')) {
      return k_ra8_err_invalid_arg;
    }
  }
  memcpy(destination, source, length + 1U);
  return k_ra8_ok;
}

/**
 * @brief Select retained storage for one response header
 * @details Maps the four protocol-selected field names to their fixed arrays
 * while leaving all other response headers intentionally ignored.
 * @param[in,out] state Active HTTP state.
 * @param[in] key NUL-terminated response-header name.
 * @param[out] destination Selected destination, or null for an ignored header.
 * @param[out] capacity Selected destination capacity.
 * @pre All pointers are non-null and @p key is NUL-terminated.
 * @pre @p state owns initialized response storage.
 * @post Known names select exactly one bounded response field.
 * @post Unknown names produce a null destination and zero capacity.
 * @note Matching is ASCII case-insensitive.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_select_response_header(mdl_http_state_t* state,
                                                             const char*       key,
                                                             char**            destination,
                                                             size_t*           capacity)
{
  *destination = nullptr;
  *capacity    = 0U;
  if (internal_mdl_header_equal(key, "Retry-After")) {
    *destination = state->response.retry_after;
    *capacity    = sizeof(state->response.retry_after);
  } else if (internal_mdl_header_equal(key, "ETag")) {
    *destination = state->response.etag;
    *capacity    = sizeof(state->response.etag);
  } else if (internal_mdl_header_equal(key, "Last-Modified")) {
    *destination = state->response.last_modified;
    *capacity    = sizeof(state->response.last_modified);
  } else if (internal_mdl_header_equal(key, "Content-Type")) {
    *destination = state->response.content_type;
    *capacity    = sizeof(state->response.content_type);
  } else {
    /* Unselected header: destination stays null and capacity stays zero. */
  }
}

/**
 * @brief Capture selected response headers from ESP-IDF events
 * @details Filters the event stream to header events and records the first
 * bounded-copy failure so the later read path fails closed.
 * @param[in,out] event ESP-IDF event record.
 * @return ESP-IDF callback status.
 * @retval ESP_OK The event was ignored or captured safely.
 * @retval ESP_FAIL A selected header was malformed or oversized.
 * @pre @p event is non-null when called by ESP-IDF.
 * @pre `event->user_data` points to the retained backend state.
 * @post Selected valid headers are copied into bounded retained storage.
 * @post The first selected-header failure is retained for portable reporting.
 * @note Body data is consumed synchronously through ::esp_http_client_read.
 * @since 0.1.0
 */
RA8_INTERNAL static esp_err_t internal_mdl_http_event(esp_http_client_event_t* event)
{
  if ((event == nullptr) || (event->user_data == nullptr)) {
    return ESP_FAIL;
  }
  if (event->event_id != HTTP_EVENT_ON_HEADER) {
    return ESP_OK;
  }
  mdl_http_state_t* state = (mdl_http_state_t*)event->user_data;
  char*             destination;
  size_t            capacity;
  internal_mdl_select_response_header(state, event->header_key, &destination, &capacity);
  if (destination == nullptr) {
    return ESP_OK;
  }
  const ra8_err_t copied = internal_mdl_copy_field(destination, capacity, event->header_value);
  if (copied != k_ra8_ok) {
    state->header_error = copied;
    return ESP_FAIL;
  }
  return ESP_OK;
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
 * @post Success retains the HTTP/SHA objects for later dispatch.
 * @post Failure leaves the portable backend unregistered.
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
    .event_handler         = internal_mdl_http_event,
    .user_data             = state,
  };
  state->http =
    esp_http_client_init(&cfg); /* alloc-allow: ESP-IDF SOUP creates one retained HTTP client */
  if (state->http == nullptr) {
    return k_ra8_err_no_mem;
  }
  mbedtls_sha256_init(&state->sha);
  return k_ra8_ok;
}

/**
 * @brief Replace one optional retained request header
 * @details Clears prior client state first, then installs the new value only
 * when the portable request selected a nonempty header.
 * @param[in,out] state Retained HTTP state.
 * @param[in] name Canonical request-header name.
 * @param[in] value Bounded retained value, or empty to omit.
 * @return Header configuration status.
 * @retval k_ra8_ok The next request has the selected header policy.
 * @retval k_ra8_fail ESP-IDF rejected a nonempty header value.
 * @pre All pointers are non-null and strings are NUL-terminated.
 * @pre No request is open on the retained client.
 * @post Success installs @p value or leaves @p name absent.
 * @post Failure does not authorize the next request.
 * @note Deleting an already-absent header is intentionally idempotent.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_http_set_header(mdl_http_state_t* state, const char* name, const char* value)
{
  (void)esp_http_client_delete_header(state->http, name);
  if (value[0] == '\0') {
    return k_ra8_ok;
  }
  return (esp_http_client_set_header(state->http, name, value) == ESP_OK) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Copy and apply the complete bounded request policy
 * @details Retains every caller string, applies the resolved timeout, and
 * replaces all optional headers before the HTTP request may open.
 * @param[in,out] state Reset retained HTTP state.
 * @param[in] request Validated portable media request.
 * @return Policy configuration status.
 * @retval k_ra8_ok All strings and ESP-IDF settings were accepted.
 * @retval k_ra8_err_invalid_arg A field contains a forbidden line break.
 * @retval k_ra8_err_invalid_size A field exceeds its fixed capacity.
 * @retval k_ra8_fail ESP-IDF rejected timeout or header configuration.
 * @pre @p state owns a closed retained client.
 * @pre @p request and its URL are non-null.
 * @post Success retains every request string through the HTTP exchange.
 * @post Failure leaves no request eligible to open.
 * @note Zero timeout selects the adapter default.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_apply_request(mdl_http_state_t*        state,
                                                              const ra8_mdl_request_t* request)
{
  ra8_err_t result = internal_mdl_copy_field(state->url, sizeof(state->url), request->url);
  if (result == k_ra8_ok) {
    result = internal_mdl_copy_field(state->user_agent,
                                     sizeof(state->user_agent),
                                     request->http.user_agent);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_copy_field(state->referer, sizeof(state->referer), request->http.referer);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_copy_field(state->if_none_match,
                                     sizeof(state->if_none_match),
                                     request->http.if_none_match);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_copy_field(state->if_modified_since,
                                     sizeof(state->if_modified_since),
                                     request->http.if_modified_since);
  }
  if (result != k_ra8_ok) {
    return result;
  }
  const uint32_t timeout =
    (request->http.timeout_ms == 0U) ? k_mdl_http_timeout_ms : request->http.timeout_ms;
  if (esp_http_client_set_timeout_ms(state->http, (int)timeout) != ESP_OK) {
    result = k_ra8_fail;
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_http_set_header(state, "User-Agent", state->user_agent);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_http_set_header(state, "Referer", state->referer);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_http_set_header(state, "If-None-Match", state->if_none_match);
  }
  if (result == k_ra8_ok) {
    result = internal_mdl_http_set_header(state, "If-Modified-Since", state->if_modified_since);
  }
  return result;
}

/**
 * @brief Validate and prepare one typed HTTPS-artifact job
 * @details Reuses retained client/hash objects and resets only per-job fields.
 * @param[in,out] ctx Initialised ::mdl_http_state_t.
 * @param[in] request Complete typed HTTPS request and optional policy.
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
RA8_INTERNAL static ra8_err_t internal_mdl_http_begin(void* ctx, const ra8_mdl_request_t* request)
{
  mdl_http_state_t* state            = (mdl_http_state_t*)ctx;
  const size_t      https_prefix_len = sizeof("https://") - 1U;
  if ((uint32_t)request->format > (uint32_t)k_mdl_format_rabook) {
    return k_ra8_err_invalid_arg;
  }
  if (strncmp(request->url, "https://", https_prefix_len) != 0) {
    return k_ra8_err_invalid_arg;
  }
  if (request->url[https_prefix_len] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  internal_mdl_http_reset_job(state);
  const ra8_err_t applied = internal_mdl_http_apply_request(state, request);
  if (applied != k_ra8_ok) {
    internal_mdl_http_reset_job(state);
    return applied;
  }
  state->format = request->format;
  const esp_err_t url_status =
    esp_http_client_set_url( // alloc-allow: ESP-IDF SOUP copies URL state
      state->http,
      state->url);
  if (url_status != ESP_OK) {
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
 * @details Records the actual final status and bounded selected headers so the
 * portable downloader, rather than this transport, owns HTTP semantics.
 * @param[in,out] state Prepared job state.
 * @return Open status.
 * @retval k_ra8_ok Headers describe a syntactically valid HTTP response.
 * @retval k_ra8_fail ESP-IDF failed to open the HTTPS connection.
 * @retval k_ra8_err_protocol_error HTTP status is outside 100..599.
 * @retval k_ra8_err_invalid_size A selected response header is oversized.
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
  const esp_err_t open_status = esp_http_client_open( // alloc-allow: ESP-IDF SOUP creates TLS state
    state->http,
    0);
  if (open_status != ESP_OK) {
    return k_ra8_fail;
  }
  const int64_t content_length = esp_http_client_fetch_headers(state->http);
  const int     status         = esp_http_client_get_status_code(state->http);
  if (state->header_error != k_ra8_ok) {
    return state->header_error;
  }
  if ((status < k_mdl_http_status_min) || (status > k_mdl_http_status_max)) {
    return k_ra8_err_protocol_error;
  }
  state->response.status = status;
  state->total           = (content_length >= 0) ? (uint64_t)content_length : 0U;
  state->total_known     = (content_length >= 0);
  state->opened          = true;
  return k_ra8_ok;
}

/**
 * @brief Validate EOF and publish the terminal length and digest.
 * @details Rejects truncated or length-mismatched responses, finalizes SHA,
 *          derives an unknown total from counted bytes, and closes job state.
 * @param[in,out] state Open response and active hash state.
 * @param[out] total_bytes Advertised or independently counted final length.
 * @param[out] complete Receives true only for verified terminal success.
 * @param[out] sha256 Complete-body digest destination.
 * @param[out] response Final status and selected response headers.
 * @return Terminal response status.
 * @retval k_ra8_ok Terminal metadata and digest were published.
 * @retval k_ra8_err_protocol_error HTTP completeness or length is incoherent.
 * @retval k_ra8_fail SHA finalization failed.
 * @pre Output pointers are non-null and @p state owns an open response.
 * @pre Every returned body byte was counted and hashed exactly once.
 * @post Success closes job state after publishing terminal outputs.
 * @post Failure closes job state without claiming completion.
 * @note Close-delimited responses publish their independently counted length.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_finish(mdl_http_state_t* state,
                                                       uint64_t*         total_bytes,
                                                       bool*             complete,
                                                       uint8_t sha256[k_ra8_mdl_sha256_bytes],
                                                       ra8_mdl_http_response_t* response)
{
  if (!esp_http_client_is_complete_data_received(state->http)) {
    internal_mdl_http_reset_job(state);
    return k_ra8_err_protocol_error;
  }
  if (state->total_known) {
    if (state->received != state->total) {
      internal_mdl_http_reset_job(state);
      return k_ra8_err_protocol_error;
    }
  }
  if (mbedtls_sha256_finish(&state->sha, sha256) != 0) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  if (!state->total_known) {
    state->total = state->received;
  }
  *total_bytes = state->total;
  *complete    = true;
  *response    = state->response;
  internal_mdl_http_reset_job(state);
  return k_ra8_ok;
}

/**
 * @brief Account one successful nonzero body read into job state.
 * @details Overflow-checks the running byte count against a 64-bit ceiling,
 *          enforces it against any advertised total, folds the bytes into
 *          the running SHA-256 digest, and reports the exact transfer.
 * @param[in,out] state Prepared job state being accounted.
 * @param[in] out Body bytes just read, spanning @p read bytes.
 * @param[in] read Strictly positive byte count returned by the transport.
 * @param[out] got Exact body bytes returned.
 * @param[out] total_bytes Advertised or independently counted total.
 * @return Accounting status.
 * @retval k_ra8_ok The bytes were folded into state and reported.
 * @retval k_ra8_err_invalid_size Received-byte count overflowed.
 * @retval k_ra8_err_protocol_error The advertised total was exceeded.
 * @retval k_ra8_fail SHA digest processing failed.
 * @pre @p read is strictly positive.
 * @pre @p out has at least @p read readable bytes.
 * @post Success advances `state->received` by exactly @p read.
 * @post Failure resets the job state before returning.
 * @note Not thread-safe; service dispatch serializes calls.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_http_account_read(mdl_http_state_t* state,
                                                             const uint8_t*    out,
                                                             int               read,
                                                             uint16_t*         got,
                                                             uint64_t*         total_bytes)
{
  if (state->received > (UINT64_MAX - (uint64_t)read)) {
    internal_mdl_http_reset_job(state);
    return k_ra8_err_invalid_size;
  }
  const uint64_t next_received = state->received + (uint64_t)read;
  if (state->total_known) {
    if (next_received > state->total) {
      internal_mdl_http_reset_job(state);
      return k_ra8_err_protocol_error;
    }
  }
  if (mbedtls_sha256_update(&state->sha, out, (size_t)read) != 0) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  state->received = next_received;
  *got            = (uint16_t)read;
  *total_bytes    = state->total;
  return k_ra8_ok;
}

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
 * @param[out] response Final status and selected headers when terminal.
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
                                                     uint8_t   sha256[k_ra8_mdl_sha256_bytes],
                                                     ra8_mdl_http_response_t* response)
{
  *got                     = 0U;
  *total_bytes             = 0U;
  *complete                = false;
  *response                = (ra8_mdl_http_response_t){};
  mdl_http_state_t* state  = (mdl_http_state_t*)ctx;
  const ra8_err_t   opened = internal_mdl_http_open(state);
  if (opened != k_ra8_ok) {
    internal_mdl_http_reset_job(state);
    return opened;
  }
  const int read = esp_http_client_read(state->http, (char*)out, cap);
  if (read < 0) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  if ((uint32_t)read > (uint32_t)cap) {
    internal_mdl_http_reset_job(state);
    return k_ra8_fail;
  }
  if (read > 0) {
    return internal_mdl_http_account_read(state, out, read, got, total_bytes);
  }
  return internal_mdl_http_finish(state, total_bytes, complete, sha256, response);
}

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
 * @brief Translate a portable media-service result into ESP-IDF's error domain
 * @details Uses an explicit semantic mapping so equal numeric values in the
 *          unrelated RA8 and ESP-IDF enumerations cannot change meaning.
 * @param[in] result Portable media-service result.
 * @return Equivalent ESP-IDF status for the outer CustomRpc response.
 * @retval ESP_OK Portable dispatch produced a complete inner response.
 * @retval ESP_ERR_NO_MEM The bounded decode arena was exhausted.
 * @retval ESP_ERR_INVALID_ARG A pointer or argument was invalid.
 * @retval ESP_ERR_INVALID_STATE The media service was busy or in the wrong
 * state.
 * @retval ESP_ERR_INVALID_SIZE A request or response extent was invalid.
 * @retval ESP_ERR_NOT_FOUND A requested object was absent.
 * @retval ESP_ERR_TIMEOUT The operation exceeded its bounded work budget.
 * @retval ESP_ERR_INVALID_RESPONSE Protocol or structural validation failed.
 * @retval ESP_ERR_INVALID_CRC A digest comparison failed.
 * @retval ESP_FAIL No more specific ESP-IDF status represents @p result.
 * @pre @p result is a value returned by the portable media service.
 * @pre Unknown operation identifiers are rejected before this mapping.
 * @post No service, HTTP, SHA, or response state is modified.
 * @post ::ESP_ERR_NOT_SUPPORTED is never returned for a recognized media
 * operation.
 * @note Keeping first-refusal separate prevents backend failures from invoking
 *       ESP-hosted's unrelated legacy CustomRpc fallback.
 * @since 0.1.0
 */
RA8_INTERNAL static esp_err_t internal_mdl_to_esp_status(ra8_err_t result)
{
  switch (result) {
    case k_ra8_ok:
      return ESP_OK;
    case k_ra8_err_no_mem:
      return ESP_ERR_NO_MEM;
    case k_ra8_err_null_ptr:
    case k_ra8_err_invalid_arg:
      return ESP_ERR_INVALID_ARG;
    case k_ra8_err_busy:
    case k_ra8_err_invalid_state:
      return ESP_ERR_INVALID_STATE;
    case k_ra8_err_invalid_size:
      return ESP_ERR_INVALID_SIZE;
    case k_ra8_err_not_found:
      return ESP_ERR_NOT_FOUND;
    case k_ra8_err_timeout:
      return ESP_ERR_TIMEOUT;
    case k_ra8_err_protocol_error:
    case k_ra8_err_validation_failed:
      return ESP_ERR_INVALID_RESPONSE;
    case k_ra8_err_checksum_mismatch:
      return ESP_ERR_INVALID_CRC;
    default:
      return ESP_FAIL;
  }
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
 * @retval ESP_ERR_NOT_SUPPORTED The operation is not owned by the media
 * service.
 * @retval ESP_ERR_INVALID_ARG A request argument or pointer was invalid.
 * @retval ESP_ERR_INVALID_STATE The media service is busy or inactive.
 * @retval ESP_ERR_INVALID_SIZE A bounded request or response extent was
 * rejected.
 * @retval ESP_ERR_INVALID_RESPONSE The inner protobuf or response was invalid.
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
  if (response_len == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  *response_len = 0U;
  if ((message_id != k_ra8_mdl_rpc_start) && (message_id != k_ra8_mdl_rpc_next) &&
      (message_id != k_ra8_mdl_rpc_cancel)) {
    return ESP_ERR_NOT_SUPPORTED;
  }
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
  return internal_mdl_to_esp_status(result);
}
