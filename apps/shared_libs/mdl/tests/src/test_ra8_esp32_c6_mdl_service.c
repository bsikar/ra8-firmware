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

#include "esp32_c6_http_model_internal.h"
#include "esp_idf_mdl_compat_internal.h"
#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_mdl_service.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

static uint8_t s_request[k_c6_http_request_cap];
static uint8_t s_response[k_c6_http_response_cap];

/**
 * @brief Pack and dispatch one generated Start request.
 * @details Uses the production synchronous hook and decodes Accepted only when
 *          the adapter reports success.
 * @param[in] url Absolute URL supplied to the service.
 * @param[in] policy Optional request policy, or null for adapter defaults.
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
RA8_INTERNAL static esp_err_t
internal_start_policy(const char* url, const ra8_mdl_http_policy_t* policy, uint32_t* out_job)
{
  Ra8__Mdl__StartRequest request = RA8__MDL__START_REQUEST__INIT;
  request.protocol_version       = k_ra8_mdl_protocol_version;
  request.url                    = (char*)url;
  request.format                 = RA8__MDL__FORMAT__FORMAT_RABOOK;
  if (policy != nullptr) {
    request.user_agent        = (char*)policy->user_agent;
    request.referer           = (char*)policy->referer;
    request.if_none_match     = (char*)policy->if_none_match;
    request.if_modified_since = (char*)policy->if_modified_since;
    request.timeout_ms        = policy->timeout_ms;
  }
  const size_t request_len  = ra8__mdl__start_request__pack(&request, s_request);
  size_t       response_len = 0U;
  *out_job                  = 0U;
  const esp_err_t status    = esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_start,
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
  TEST_ASSERT_EQ(RA8__MDL__FORMAT__FORMAT_RABOOK, accepted->format);
  *out_job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  return status;
}

/**
 * @brief Start one request with the adapter's default HTTP policy
 * @details Exercises the zero-initialized policy path while reusing the same
 * generated Start encoder and response decoder as explicit-policy tests.
 * @param[in] url Absolute HTTPS URL.
 * @param[out] out_job Accepted nonzero job identifier.
 * @return Concrete handler status.
 * @retval ESP_OK The service accepted the job.
 * @retval ESP_ERR_* Validation or backend setup failed.
 * @pre @p url and @p out_job are non-null.
 * @pre The model is configured before the call.
 * @post Success initializes @p out_job.
 * @post Failure leaves @p out_job zero.
 * @note Delegates to ::internal_start_policy with no optional fields.
 * @since 0.1.0
 */
RA8_INTERNAL static esp_err_t internal_start(const char* url, uint32_t* out_job)
{
  return internal_start_policy(url, nullptr, out_job);
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
  static const uint8_t empty[1] = {};
  priv_c6_http_model_reset(empty, 0U);
  size_t response_len = sizeof(s_response);
  TEST_ASSERT_EQ(ESP_ERR_NOT_SUPPORTED,
                 esp_hosted_custom_rpc_sync_handler(UINT32_MAX,
                                                    empty,
                                                    sizeof(empty),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ(0, response_len);
  TEST_ASSERT_EQ(0, priv_c6_http_model()->init_calls);
  TEST_END("C6 HTTP unknown operation first refusal");
}

/**
 * @brief Verify retained initialization retries and begin-stage failures.
 * @details Proves a failed retained-client allocation is retryable and URL/SHA
 *          setup failures do not activate a portable job.
 * @par MC/DC:
 * No compound decision is present in the adapter's prevalidated begin path.
 * ::internal_mdl_http_apply_request returns a failed bounded copy before the
 * timeout setter and tests the setter result separately; `set_timeout_fail`
 * exercises that failure. ::internal_mdl_http_begin consumes the portable
 * service's non-null initialized-context contract. The `init_fail` vector
 * proves the hook does not register that backend when initialization fails.
 * @pre The retained service is uninitialized and the model has no active job.
 * @pre Generated protobuf scratch has fixed sufficient capacity.
 * @post A successful retained client exists for all later vectors.
 * @post No job remains active after either begin-stage failure.
 * @note Must execute before the other fixture vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_initialization_and_begin_faults(void)
{
  static const uint8_t empty[1] = {};
  uint32_t             job      = 0U;
  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->init_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->set_url_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->sha_start_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->set_timeout_fail = true;
  TEST_ASSERT_EQ(ESP_FAIL, internal_start("https://example.test/book", &job));

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->set_header_fail = true;
  const ra8_mdl_http_policy_t policy    = {.user_agent = "ra8-test"};
  TEST_ASSERT_EQ(ESP_FAIL, internal_start_policy("https://example.test/book", &policy, &job));
  TEST_END("C6 HTTP initialization and begin faults");
}

/**
 * @brief Verify malformed media input maps into ESP-IDF's error domain
 * @details Dispatches a known operation with an invalid protobuf byte and
 *          checks every owned message identifier against the fallback sentinel.
 * @par MC/DC:
 * `port/esp32_c6/src/mdl_service.c@esp_hosted_custom_rpc_sync_handler`
 * - V1: Start -- C1 false, decision false.
 * - V2: Next -- C1 true, C2 false, decision false.
 * - V3: Cancel -- C1 true, C2 true, C3 false, decision false.
 * - V4: unknown -- C1 true, C2 true, C3 true, decision true.
 * @pre Retained service initialization succeeded in the prior vector.
 * @pre No portable media job is active.
 * @post Known malformed operations return ::ESP_ERR_INVALID_RESPONSE.
 * @post ESP-hosted will not invoke its unrelated legacy CustomRpc handler.
 * @note Pins the numeric-domain collision that previously made fallback
 * unreachable.
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
  TEST_ASSERT_EQ(ESP_ERR_INVALID_RESPONSE,
                 esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_next,
                                                    malformed,
                                                    sizeof(malformed),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ(ESP_ERR_INVALID_RESPONSE,
                 esp_hosted_custom_rpc_sync_handler(k_ra8_mdl_rpc_cancel,
                                                    malformed,
                                                    sizeof(malformed),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ(ESP_ERR_NOT_SUPPORTED,
                 esp_hosted_custom_rpc_sync_handler(UINT32_MAX,
                                                    malformed,
                                                    sizeof(malformed),
                                                    s_response,
                                                    sizeof(s_response),
                                                    &response_len));
  TEST_ASSERT_EQ(0, response_len);
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
  priv_c6_http_model_reset(body, sizeof(body));
  uint32_t job = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));

  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 2U, &chunk));
  TEST_ASSERT_EQ(2, chunk->data.len);
  TEST_ASSERT_EQ(0, memcmp(body, chunk->data.data, 2U));
  TEST_ASSERT_EQ(sizeof(body), chunk->total_bytes);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 2U, 2U, &chunk));
  TEST_ASSERT_EQ(1, chunk->data.len);
  TEST_ASSERT_EQ(0, memcmp(&body[2], chunk->data.data, 1U));
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 3U, 2U, &chunk));
  uint8_t expected[k_ra8_mdl_sha256_bytes];
  internal_expected_digest(body, sizeof(body), expected);
  TEST_ASSERT_EQ(RA8__MDL__STATE__STATE_COMPLETE, chunk->state);
  TEST_ASSERT_EQ(0, chunk->data.len);
  TEST_ASSERT_EQ(sizeof(expected), chunk->sha256.len);
  TEST_ASSERT_EQ(0, memcmp(expected, chunk->sha256.data, sizeof(expected)));
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_END("C6 HTTP known-length multichunk");
}

/**
 * @brief Verify request policy and terminal response metadata end to end
 * @details Sends all bounded conditional-request fields through generated
 * protobuf, observes the ESP-IDF setters, and captures mixed-case response
 * header events into the terminal Chunk.
 * @pre Retained service initialization is live and no job is active.
 * @pre The model emits one complete empty response.
 * @post Every request field reaches the concrete client unchanged.
 * @post A later empty-policy request clears retained headers and uses default timeout.
 * @note Redirect and conditional status interpretation remains in `mdl_fetch`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_http_policy_and_metadata(void)
{
  static const uint8_t empty[1] = {};
  priv_c6_http_model_reset(empty, 0U);
  const ra8_mdl_http_policy_t policy = {
    .user_agent        = "ra8-c6-test/3",
    .referer           = "https://example.test/catalog",
    .if_none_match     = "\"prior-etag\"",
    .if_modified_since = "Tue, 20 Oct 2015 07:28:00 GMT",
    .timeout_ms        = 4321U,
  };
  uint32_t job = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start_policy("https://example.test/book", &policy, &job));
  TEST_ASSERT(strcmp(priv_c6_http_model()->user_agent, policy.user_agent) == 0);
  TEST_ASSERT(strcmp(priv_c6_http_model()->referer, policy.referer) == 0);
  TEST_ASSERT(strcmp(priv_c6_http_model()->if_none_match, policy.if_none_match) == 0);
  TEST_ASSERT(strcmp(priv_c6_http_model()->if_modified_since, policy.if_modified_since) == 0);
  TEST_ASSERT_EQ(4321, priv_c6_http_model()->timeout_ms);

  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 8U, &chunk));
  TEST_ASSERT_EQ(200, chunk->http_status);
  TEST_ASSERT(strcmp(chunk->retry_after, "4") == 0);
  TEST_ASSERT(strcmp(chunk->etag, "\"esp-etag\"") == 0);
  TEST_ASSERT(strcmp(chunk->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_ASSERT(strcmp(chunk->content_type, "application/x-rabook") == 0);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  priv_c6_http_model_reset(empty, 0U);
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  TEST_ASSERT_EQ(0, priv_c6_http_model()->user_agent[0]);
  TEST_ASSERT_EQ(0, priv_c6_http_model()->referer[0]);
  TEST_ASSERT_EQ(0, priv_c6_http_model()->if_none_match[0]);
  TEST_ASSERT_EQ(0, priv_c6_http_model()->if_modified_since[0]);
  TEST_ASSERT_EQ(15000, priv_c6_http_model()->timeout_ms);
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 8U, &chunk));
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_END("C6 HTTP policy and response metadata");
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
  priv_c6_http_model_reset(body, sizeof(body));
  priv_c6_http_model()->content_length = -1;
  uint32_t job                         = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 4U, &chunk));
  TEST_ASSERT_EQ(0, chunk->total_bytes);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 1U, 4U, &chunk));
  TEST_ASSERT_EQ(1, chunk->total_bytes);
  TEST_ASSERT_EQ(RA8__MDL__STATE__STATE_COMPLETE, chunk->state);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
  TEST_END("C6 HTTP unknown length accounting");
}

/**
 * @brief Verify HTTP open failures and status transport semantics.
 * @details Rejects values outside the HTTP status domain while surfacing a
 * non-followed redirect intact for portable downloader interpretation.
 * @par MC/DC:
 * The status-domain decision `(status < min) || (status > max)` in
 * ::internal_mdl_http_open executes V1 status 300 -> F,F/false, V2 status 99
 * -> T,-/true, and V3 status 600 -> F,T/true, all with the same open, header
 * and body model. V1/V2 independently vary the lower bound and V1/V3 the
 * upper bound.
 * Decisions: port/esp32_c6/src/mdl_service.c@internal_mdl_http_open
 * @pre Retained service initialization is live and no job is active.
 * @pre Each model reset clears the prior injected fault.
 * @post Every malformed or failed job is cancelled before the next Start.
 * @post Redirect status is transported without being automatically followed.
 * @note `mdl_fetch` owns redirect and conditional response semantics.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_and_status_faults(void)
{
  static const uint8_t empty[1] = {};
  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->open_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->status = (int)k_c6_http_status_low;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->status = (int)k_c6_http_status_redirect;
  uint32_t job                 = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 8U, &chunk));
  TEST_ASSERT_EQ(k_c6_http_status_redirect, chunk->http_status);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->status = (int)k_c6_http_status_high;
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
  static const uint8_t empty[1] = {};
  static const uint8_t body[]   = {'x'};
  char                 oversized_etag[k_ra8_mdl_etag_max + 1U];
  (void)memset(oversized_etag, 'x', sizeof(oversized_etag) - 1U);
  oversized_etag[sizeof(oversized_etag) - 1U] = '\0';
  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->read_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->read_oversize = true;
  internal_expect_first_next_failure(ESP_FAIL);

  priv_c6_http_model_reset(body, sizeof(body));
  priv_c6_http_model()->content_length = 0;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->content_length = 1;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->complete = false;
  internal_expect_first_next_failure(ESP_ERR_INVALID_RESPONSE);

  priv_c6_http_model_reset(body, sizeof(body));
  priv_c6_http_model()->sha_update_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->sha_finish_fail = true;
  internal_expect_first_next_failure(ESP_FAIL);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag = oversized_etag;
  internal_expect_first_next_failure(ESP_ERR_INVALID_SIZE);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag = "bad\r\nheader";
  internal_expect_first_next_failure(ESP_ERR_INVALID_ARG);

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag = "bad\nheader";
  internal_expect_first_next_failure(ESP_ERR_INVALID_ARG);
  TEST_END("C6 HTTP read and hash faults");
}

/**
 * @brief Run one complete job and assert the ETag the adapter captured.
 * @details Drives the empty-body model end to end so the first pull is the
 * terminal one, then reads back the header the production selector chose to
 * retain.
 * @param[in] expected Exact ETag the terminal Chunk must carry.
 * @pre The model is configured and no portable job is active.
 * @pre Retained service initialization is live.
 * @post The job completes and its decoded response is released.
 * @post The captured ETag matches @p expected exactly.
 * @note An ignored or absent header leaves the protocol's empty string.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_captured_etag(const char* expected)
{
  uint32_t job = 0U;
  TEST_ASSERT_EQ(ESP_OK, internal_start("https://example.test/book", &job));
  Ra8__Mdl__Chunk* chunk = nullptr;
  TEST_ASSERT_EQ(ESP_OK, internal_next(job, 0U, 8U, &chunk));
  TEST_ASSERT_EQ(RA8__MDL__STATE__STATE_COMPLETE, chunk->state);
  TEST_ASSERT(strcmp(chunk->etag, expected) == 0);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);
}

/**
 * @test internal_test_header_capture_mcdc
 * @brief Prove the event guard, name matcher and field copier per condition.
 * @par MC/DC:
 * ::internal_mdl_http_event -- `(event == nullptr) || (event->user_data ==
 * nullptr)`: every modelled header event is V1 F,F/false, the null event here
 * is V2 T,-/true, and the context-less event is V3 F,T/true.
 * ::internal_mdl_header_equal, against the matching `eTaG` control: the null
 * candidate name drives its single pointer decision. The canonical right-hand
 * name is a non-null literal at every internal call site.
 * - `(lhs[index] != '\\0') && (rhs[index] != '\\0')`: the control iterates
 *   T,T/true, the short name `ETa` exits F,-/false, and the long name `ETagX`
 *   exits T,F/false.
 * - `(lhs[index] == '\\0') && (rhs[index] == '\\0')`: the control returns
 *   T,T/true, `ETagX` returns F,-/false, and `ETa` returns T,F/false.
 * - `(left >= 'A') && (left <= 'Z')` and its right-hand twin: matching
 *   `rEtRy-AfTeR` against `Retry-After` walks an upper-case letter (T,T/true),
 *   a lower-case letter (T,F/false) and the hyphen (F,-/false) on each side.
 * ::internal_mdl_copy_field:
 * - `(source == nullptr) || (source[0] == '\\0')`: a captured value is
 *   F,F/false, the valueless ETag event is T,-/true, and the empty ETag value
 *   is F,T/true.
 * - `(source[i] == '\\r') || (source[i] == '\\n')`: every accepted value byte
 *   is F,F/false, and `internal_test_read_and_hash_faults` supplies the
 *   CR-first value (T,-/true) and the LF-only value (F,T/true).
 * Decisions: port/esp32_c6/src/mdl_service.c@internal_mdl_http_event
 * Decisions: port/esp32_c6/src/mdl_service.c@internal_mdl_header_equal
 * Decisions: port/esp32_c6/src/mdl_service.c@internal_mdl_copy_field
 * @details Header-name and value variations are observed through the terminal
 * Chunk, so each assertion reads production state rather than the model's.
 * @pre Retained service initialization is live and no job is active.
 * @pre The retained client published its production event callback.
 * @post Every ignored or malformed header leaves the response field empty.
 * @post The canonical mixed-case name is still captured verbatim.
 * @note Header names are ASCII tokens, so only that range is case-folded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_header_capture_mcdc(void)
{
  static const uint8_t empty[1] = {};
  priv_c6_http_model_reset(empty, 0U);
  TEST_ASSERT(priv_c6_http_client()->event_handler != nullptr);
  TEST_ASSERT_EQ(ESP_FAIL, priv_c6_http_client()->event_handler(nullptr));
  esp_http_client_event_t orphan = {.event_id     = HTTP_EVENT_ON_HEADER,
                                    .client       = priv_c6_http_client(),
                                    .user_data    = nullptr,
                                    .header_key   = (char*)"ETag",
                                    .header_value = (char*)"\"orphan\""};
  TEST_ASSERT_EQ(ESP_FAIL, priv_c6_http_client()->event_handler(&orphan));
  esp_http_client_event_t body = {.event_id  = HTTP_EVENT_ON_DATA,
                                  .client    = priv_c6_http_client(),
                                  .user_data = priv_c6_http_client()->user_data};
  TEST_ASSERT_EQ(ESP_OK, priv_c6_http_client()->event_handler(&body));

  priv_c6_http_model_reset(empty, 0U);
  internal_expect_captured_etag("\"esp-etag\"");

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag_key = "ETagX";
  internal_expect_captured_etag("");

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag_key = "ETa";
  internal_expect_captured_etag("");

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag_key = nullptr;
  internal_expect_captured_etag("");

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->null_etag_value = true;
  internal_expect_captured_etag("");

  priv_c6_http_model_reset(empty, 0U);
  priv_c6_http_model()->etag = "";
  internal_expect_captured_etag("");
  TEST_END("C6 HTTP header capture MC/DC");
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
int main(void)
{
  internal_test_unknown_operation_first_refusal();
  internal_test_initialization_and_begin_faults();
  internal_test_known_operation_error_mapping();
  internal_test_known_length_multichunk();
  internal_test_http_policy_and_metadata();
  internal_test_unknown_length();
  internal_test_open_and_status_faults();
  internal_test_read_and_hash_faults();
  internal_test_header_capture_mcdc();
  return 0;
}
