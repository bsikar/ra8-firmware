/**
 * @file test_ra8_esp32_c6_mdl_service.c
 * @brief Host fault model for the concrete ESP32-C6 HTTPS media backend.
 * @details Compiles the production ESP-IDF adapter against deterministic HTTP
 * and SHA stand-ins, then drives its public CustomRpc hook with generated
 * protobuf messages. The vectors cover retained-client initialization,
 * multichunk hashing, unknown lengths, HTTP rejection, truncation, and backend
 * faults without claiming to model TLS or the ESP-IDF allocator.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "esp_idf_mdl_compat_internal.h"
#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_mdl_service.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

/** @brief Fixed fixture capacities and HTTP values. */
typedef enum : uint16_t {
  k_c6_http_request_cap     = 256U,  /**< Packed request scratch.          */
  k_c6_http_response_cap    = 4608U, /**< Packed response scratch.         */
  k_c6_http_status_ok       = 200U,  /**< Accepted HTTP status.            */
  k_c6_http_status_low      = 199U,  /**< Below the accepted status range. */
  k_c6_http_status_redirect = 300U,  /**< First rejected redirect status.  */
} c6_http_test_limits_t;

/** @brief Deterministic host model for the consumed ESP-IDF surface. */
typedef struct {
  const uint8_t* body;            /**< Borrowed response body.             */
  size_t         body_bytes;      /**< Complete body extent.               */
  size_t         cursor;          /**< Next unread body byte.              */
  int64_t        content_length;  /**< Advertised length, or negative.     */
  int            status;          /**< HTTP response status.               */
  bool           complete;        /**< ESP-IDF completeness verdict.       */
  bool           init_fail;       /**< Fail retained-client creation.      */
  bool           set_url_fail;    /**< Fail URL installation.              */
  bool           open_fail;       /**< Fail request opening.               */
  bool           read_fail;       /**< Return a negative body read.        */
  bool           read_oversize;   /**< Return one byte beyond capacity.    */
  bool           sha_start_fail;  /**< Fail SHA stream initialization.     */
  bool           sha_update_fail; /**< Fail SHA body incorporation.        */
  bool           sha_finish_fail; /**< Fail terminal digest publication.   */
  uint32_t       init_calls;      /**< Observed retained-client creations. */
  uint32_t       close_calls;     /**< Observed per-job close operations.  */
} c6_http_model_t;

/** @brief Concrete storage behind the opaque ESP-IDF handle type. */
struct esp_http_client {
  const char* url; /**< Last URL accepted by the model. */
};

static struct esp_http_client s_client;
static c6_http_model_t        s_model;
static uint8_t                s_request[k_c6_http_request_cap];
static uint8_t                s_response[k_c6_http_response_cap];

/**
 * @brief Reset the model to one successful deterministic response.
 * @details Clears every injected fault while retaining the production service's
 *          independently owned one-time client state.
 * @param[in] body Borrowed response bytes.
 * @param[in] body_bytes Readable bytes at @p body.
 * @pre @p body is non-NULL when @p body_bytes is nonzero.
 * @pre No public handler call executes concurrently.
 * @post The next URL/open/read sequence starts at body offset zero.
 * @post Content-Length equals @p body_bytes and completeness defaults true.
 * @note File-local fixture mutation only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_model_reset(const uint8_t* body, size_t body_bytes)
{
  s_model = (c6_http_model_t){.body           = body,
                              .body_bytes     = body_bytes,
                              .content_length = (int64_t)body_bytes,
                              .status         = (int)k_c6_http_status_ok,
                              .complete       = true};
}

/**
 * @brief Pack and dispatch one generated Start request.
 * @details Uses the production synchronous hook and decodes Accepted only when
 *          the adapter reports success.
 * @param[in] url Absolute URL supplied to the service.
 * @param[out] out_job Accepted nonzero job identifier on success.
 * @return Concrete handler status.
 * @retval ESP_OK The service accepted the job and @p out_job is initialized.
 * @retval ESP_FAIL One-time component initialization failed.
 * @retval ESP_ERR_* Portable validation failures mapped into ESP-IDF's domain.
 * @pre @p url and @p out_job are non-NULL.
 * @pre The model is configured before the call.
 * @post Success consumes and frees the generated Accepted object.
 * @post Failure leaves @p out_job zero.
 * @note Uses protobuf-C's default allocator only inside the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static esp_err_t internal_start(const char* url, uint32_t* out_job)
{
  Ra8__Mdl__StartRequest request = RA8__MDL__START_REQUEST__INIT;
  request.protocol_version       = k_ra8_mdl_protocol_version;
  request.url                    = (char*)url;
  const size_t request_len       = ra8__mdl__start_request__pack(&request, s_request);
  size_t       response_len      = 0U;
  *out_job                       = 0U;
  const esp_err_t status         = esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_start,
                                                                      s_request,
                                                                      request_len,
                                                                      s_response,
                                                                      sizeof(s_response),
                                                                      &response_len);
  if (status != ESP_OK) {
    return status;
  }
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(accepted != nullptr);
  TEST_ASSERT(accepted->job_id != 0U);
  *out_job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  return status;
}

/**
 * @brief Pack and dispatch one generated Next request.
 * @details Returns the concrete handler status and decodes Chunk only on
 *          success so fault vectors never interpret an absent response.
 * @param[in] job Active job identifier.
 * @param[in] offset Acknowledged body offset.
 * @param[in] max_bytes Requested bounded chunk size.
 * @param[out] out_chunk Allocated decoded response on success.
 * @return Concrete handler status.
 * @retval ESP_OK @p out_chunk owns a decoded Chunk.
 * @retval ESP_ERR_* Backend or portable service failure mapped explicitly.
 * @pre @p out_chunk is non-NULL and the request tuple is protocol-valid.
 * @pre Any returned chunk is freed by the caller.
 * @post Failure leaves @p out_chunk NULL.
 * @post Success advances production service state exactly once.
 * @note Uses protobuf-C's default allocator only inside the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static esp_err_t
internal_next(uint32_t job, uint64_t offset, uint32_t max_bytes, Ra8__Mdl__Chunk** out_chunk)
{
  Ra8__Mdl__NextRequest request = RA8__MDL__NEXT_REQUEST__INIT;
  request.protocol_version      = k_ra8_mdl_protocol_version;
  request.job_id                = job;
  request.acknowledged_offset   = offset;
  request.max_bytes             = max_bytes;
  const size_t request_len      = ra8__mdl__next_request__pack(&request, s_request);
  size_t       response_len     = 0U;
  *out_chunk                    = nullptr;
  const esp_err_t status        = esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_next,
                                                                     s_request,
                                                                     request_len,
                                                                     s_response,
                                                                     sizeof(s_response),
                                                                     &response_len);
  if (status == ESP_OK) {
    *out_chunk = ra8__mdl__chunk__unpack(nullptr, response_len, s_response);
    TEST_ASSERT(*out_chunk != nullptr);
  }
  return status;
}

/**
 * @brief Start the configured job and assert its first pull fails as expected.
 * @details Centralizes the repeated correlation tuple while each caller
 *          configures exactly one HTTP or SHA fault.
 * @param[in] expected Expected concrete handler status.
 * @pre The model is reset and exactly one intended fault is enabled.
 * @pre No portable job is active before the call.
 * @post The failed job is inactive and a later Start may proceed.
 * @post No decoded Chunk ownership escapes.
 * @note Exercises production cancellation after backend failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_first_next_failure(esp_err_t expected)
{
  uint32_t job = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(expected, internal_next(job, 0U, 8U, &chunk));
  TEST_ASSERT(chunk == nullptr);
}

/**
 * @brief Render the digest produced by the deterministic SHA stand-in.
 * @details Mirrors the model's byte-sum plus output-index transformation.
 * @param[in] body Hashed body bytes.
 * @param[in] body_bytes Number of bytes at @p body.
 * @param[out] out Complete expected SHA-sized model digest.
 * @pre @p body and @p out span their declared readable/writable extents.
 * @pre @p out has ::k_ra8_mdl_sha256_bytes writable bytes.
 * @post Every output byte is initialized deterministically.
 * @post Input bytes are unchanged.
 * @note This verifies adapter streaming, not SHA-256 cryptography.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expected_digest(const uint8_t* body,
                                                  size_t         body_bytes,
                                                  uint8_t        out[k_ra8_mdl_sha256_bytes])
{
  uint64_t sum = 0U;
  for (size_t i = 0U; i < body_bytes; ++i) {
    sum += body[i];
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_mdl_sha256_bytes; ++i) {
    out[i] = (uint8_t)(sum + i);
  }
}

/* ESP-IDF stand-ins implement the contracts declared by the compatibility header. */
RA8_PRIV esp_err_t esp_crt_bundle_attach(void* conf)
{
  (void)conf;
  return ESP_OK;
}

RA8_PRIV esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config)
{
  TEST_ASSERT(config != nullptr);
  ++s_model.init_calls;
  if (s_model.init_fail) {
    return nullptr;
  }
  s_client = (struct esp_http_client){};
  return &s_client;
}

RA8_PRIV esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  ++s_model.close_calls;
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char* url)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(url != nullptr);
  if (s_model.set_url_fail) {
    return ESP_FAIL;
  }
  client->url    = url;
  s_model.cursor = 0U;
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_open(esp_http_client_handle_t client, int64_t write_len)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT_EQ((int64_t)0, write_len);
  return s_model.open_fail ? ESP_FAIL : ESP_OK;
}

RA8_PRIV int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  return s_model.content_length;
}

RA8_PRIV int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  return s_model.status;
}

RA8_PRIV int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(buffer != nullptr);
  if (s_model.read_fail) {
    return -1;
  }
  if (s_model.read_oversize) {
    return len + 1;
  }
  const size_t remaining = s_model.body_bytes - s_model.cursor;
  if (remaining == 0U) {
    return 0;
  }
  size_t count = remaining;
  if (count > (size_t)len) {
    count = (size_t)len;
  }
  (void)memcpy(buffer, &s_model.body[s_model.cursor], count);
  s_model.cursor += count;
  return (int)count;
}

RA8_PRIV bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  return s_model.complete;
}

RA8_PRIV void mbedtls_sha256_init(mbedtls_sha256_context* ctx)
{
  TEST_ASSERT(ctx != nullptr);
  *ctx = (mbedtls_sha256_context){};
}

RA8_PRIV int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224)
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)is224);
  ctx->opaque[0] = 0U;
  return s_model.sha_start_fail ? -1 : 0;
}

RA8_PRIV int
mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen)
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT(input != nullptr);
  if (s_model.sha_update_fail) {
    return -1;
  }
  for (size_t i = 0U; i < ilen; ++i) {
    ctx->opaque[0] += input[i];
  }
  return 0;
}

RA8_PRIV int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32])
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT(output != nullptr);
  if (s_model.sha_finish_fail) {
    return -1;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_mdl_sha256_bytes; ++i) {
    output[i] = (uint8_t)(ctx->opaque[0] + i);
  }
  return 0;
}

/**
 * @brief Verify an unrelated CustomRpc operation is left to ESP-hosted
 * @details Sends an unknown identifier before one-time HTTP initialization and
 *          proves first-refusal neither creates a client nor claims a response.
 * @pre Production and model state are at process-start defaults.
 * @pre The request and response spans are valid but semantically irrelevant.
 * @post ::ESP_ERR_NOT_SUPPORTED selects the patched upstream fallback.
 * @post No HTTP client is created and response length is reset to zero.
 * @note Must execute before every media operation in this process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unknown_operation_first_refusal(void)
{
  static const uint8_t empty[] = {0U};
  internal_model_reset(empty, 0U);
  size_t response_len = sizeof(s_response);
  TEST_ASSERT_EQ(ESP_ERR_NOT_SUPPORTED,
                 esp_hosted_custom_rpc_sync_handler(UINT32_MAX,
                                                    empty,
                                                    sizeof(empty),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_model.init_calls);
  TEST_END("C6 HTTP unknown operation first refusal");
}

/**
 * @brief Verify retained initialization retries and begin-stage failures.
 * @details Proves a failed retained-client allocation is retryable and URL/SHA
 *          setup failures do not activate a portable job.
 * @pre The retained service is uninitialized and the model has no active job.
 * @pre Generated protobuf scratch has fixed sufficient capacity.
 * @post A successful retained client exists for all later vectors.
 * @post No job remains active after either begin-stage failure.
 * @note Must execute before the other fixture vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_initialization_and_begin_faults(void)
{
  static const uint8_t empty[] = {0U};
  uint32_t             job     = 0U;
  internal_model_reset(empty, 0U);
  s_model.init_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  internal_model_reset(empty, 0U);
  s_model.set_url_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  internal_model_reset(empty, 0U);
  s_model.sha_start_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));
  TEST_END("C6 HTTP initialization and begin faults");
}

/**
 * @brief Verify malformed media input maps into ESP-IDF's error domain
 * @details Dispatches a known operation with an invalid protobuf byte and
 *          checks that it is not mistaken for the upstream fallback sentinel.
 * @pre Retained service initialization succeeded in the prior vector.
 * @pre No portable media job is active.
 * @post The handler returns ::ESP_ERR_INVALID_RESPONSE with zero response bytes.
 * @post ESP-hosted will not invoke its unrelated legacy CustomRpc handler.
 * @note Pins the numeric-domain collision that previously made fallback unreachable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_known_operation_error_mapping(void)
{
  static const uint8_t malformed[]  = {0x80U};
  size_t               response_len = sizeof(s_response);
  TEST_ASSERT_EQ(ESP_ERR_INVALID_RESPONSE,
                 esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_start,
                                                    malformed,
                                                    sizeof(malformed),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_END("C6 HTTP known operation error mapping");
}

/**
 * @brief Verify known-length multichunk transfer and terminal digest.
 * @details Pulls three body bytes in two bounded chunks, then verifies the
 *          independently finalized digest and exact advertised length.
 * @pre Retained service initialization succeeded in the prior vector.
 * @pre The deterministic model body remains immutable for the transfer.
 * @post Chunk offsets and data preserve source order exactly.
 * @post Terminal response contains no data and owns a full digest.
 * @note Exercises one retained HTTP client across multiple RPC pulls.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_known_length_multichunk(void)
{
  static const uint8_t body[] = {'a', 'b', 'c'};
  internal_model_reset(body, sizeof(body));
  uint32_t job = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));

  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 2U, &chunk));
  TEST_ASSERT_EQ((int64_t)2, (int64_t)chunk->data.len);
  TEST_ASSERT_EQ(0, memcmp(body, chunk->data.data, 2U));
  TEST_ASSERT_EQ((int64_t)sizeof(body), (int64_t)chunk->total_bytes);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 2U, 2U, &chunk));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)chunk->data.len);
  TEST_ASSERT_EQ(0, memcmp(&body[2], chunk->data.data, 1U));
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 3U, 2U, &chunk));
  uint8_t expected[k_ra8_mdl_sha256_bytes];
  internal_expected_digest(body, sizeof(body), expected);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_COMPLETE, (int64_t)chunk->state);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)chunk->data.len);
  TEST_ASSERT_EQ((int64_t)sizeof(expected), (int64_t)chunk->sha256.len);
  TEST_ASSERT_EQ(0, memcmp(expected, chunk->sha256.data, sizeof(expected)));
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_END("C6 HTTP known-length multichunk");
}

/**
 * @brief Verify close-delimited transfer publishes its counted length.
 * @details Models a negative Content-Length, consumes one byte, and checks the
 *          terminal record substitutes the independently counted total.
 * @pre Retained service initialization is live and no job is active.
 * @pre The model reports a complete response at EOF.
 * @post Data response does not invent an advertised length.
 * @post Terminal response reports the exact counted body size.
 * @note Covers chunked or close-delimited response accounting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unknown_length(void)
{
  static const uint8_t body[] = {'z'};
  internal_model_reset(body, sizeof(body));
  s_model.content_length = -1;
  uint32_t job           = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 4U, &chunk));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)chunk->total_bytes);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 1U, 4U, &chunk));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)chunk->total_bytes);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_COMPLETE, (int64_t)chunk->state);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_END("C6 HTTP unknown length accounting");
}

/**
 * @brief Verify HTTP open and status failures remain fail-closed.
 * @details Exercises connection failure and both sides of the accepted 2xx
 *          range through the public RPC hook.
 * @pre Retained service initialization is live and no job is active.
 * @pre Each model reset clears the prior injected fault.
 * @post Every failed job is cancelled before the next Start.
 * @post No invalid response produces a decoded Chunk.
 * @note Redirects are intentionally rejected by policy.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_and_status_faults(void)
{
  static const uint8_t empty[] = {0U};
  internal_model_reset(empty, 0U);
  s_model.open_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  internal_model_reset(empty, 0U);
  s_model.status = (int)k_c6_http_status_low;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  internal_model_reset(empty, 0U);
  s_model.status = (int)k_c6_http_status_redirect;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);
  TEST_END("C6 HTTP open and status faults");
}

/**
 * @brief Verify body, completion, length, and SHA failures are terminal.
 * @details Enables one backend fault per fresh job and proves the public hook
 *          reports the canonical failure without publishing a Chunk.
 * @pre Retained service initialization is live and no job is active.
 * @pre Each model reset clears the prior injected fault.
 * @post Every failed backend job is inactive and restartable.
 * @post Oversize, truncation, and digest failures remain distinguishable.
 * @note The vectors cover both known-length data and EOF transitions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_and_hash_faults(void)
{
  static const uint8_t empty[] = {0U};
  static const uint8_t body[]  = {'x'};
  internal_model_reset(empty, 0U);
  s_model.read_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  internal_model_reset(empty, 0U);
  s_model.read_oversize = true;
  internal_expect_first_next_failure(ESP_FAIL);

  internal_model_reset(body, sizeof(body));
  s_model.content_length = 0;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  internal_model_reset(empty, 0U);
  s_model.content_length = 1;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  internal_model_reset(empty, 0U);
  s_model.complete = false;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  internal_model_reset(body, sizeof(body));
  s_model.sha_update_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  internal_model_reset(empty, 0U);
  s_model.sha_finish_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);
  TEST_END("C6 HTTP read and hash faults");
}

/**
 * @brief Run the concrete ESP32-C6 HTTP service host model.
 * @details Exercises unknown-operation first refusal before initialization,
 *          then success and fault vectors against one retained service.
 * @return Process test status.
 * @retval 0 Every assertion passed.
 * @pre No other thread invokes the production singleton service.
 * @pre The executable links the concrete adapter and generated codec once.
 * @post All configured transfers are terminal or cancelled.
 * @post Test-only model storage remains process-local.
 * @note Hosted integration test; no network or ESP32 hardware is accessed.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_unknown_operation_first_refusal();
  internal_test_initialization_and_begin_faults();
  internal_test_known_operation_error_mapping();
  internal_test_known_length_multichunk();
  internal_test_unknown_length();
  internal_test_open_and_status_faults();
  internal_test_read_and_hash_faults();
  return 0;
}
