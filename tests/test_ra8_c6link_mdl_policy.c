/**
 * @file test_ra8_c6link_mdl_policy.c
 * @brief Request-policy and response-metadata vectors for the media service.
 * @details Drives every decoded StartRequest field operand and every terminal
 * response-metadata operand through `ra8_mdl_service_dispatch` with real
 * packed protobuf. The deterministic backend both service test units bind
 * lives here too, beside the vectors that exercise it hardest, which keeps
 * each hand-authored translation unit inside the repository size cap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_media_download.pb-c.h"
#include "test_ra8_c6link_mdl_policy_internal.h"
#include "unity_minimal.h"

/** @brief Fixed capacities owned by this vector fixture. */
typedef enum : uint16_t {
  k_t_policy_request_bytes  = 700U,  /**< Packed request scratch capacity.  */
  k_t_policy_response_bytes = 1200U, /**< Packed response scratch capacity. */
  k_t_policy_len_sentinel   = 99U,   /**< Non-zero output-length sentinel.  */
} t_policy_const_t;

static ra8_test_mdl_backend_t s_policy_backend;
static ra8_mdl_service_t      s_policy_service;
static uint8_t                s_policy_request[k_t_policy_request_bytes];
static uint8_t                s_policy_response[k_t_policy_response_bytes];

/**
 * @brief Implementation of `priv_test_mdl_backend_begin()`.
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_begin(void* ctx, const ra8_mdl_request_t* request)
{
  ra8_test_mdl_backend_t* fake = (ra8_test_mdl_backend_t*)ctx;
  if (strcmp(request->url, "https://example.test/book") != 0) {
    return k_ra8_err_invalid_arg;
  }
  fake->at         = 0U;
  fake->format     = request->format;
  fake->timeout_ms = request->http.timeout_ms;
  memcpy(fake->user_agent, request->http.user_agent, strlen(request->http.user_agent) + 1U);
  memcpy(fake->referer, request->http.referer, strlen(request->http.referer) + 1U);
  memcpy(fake->if_none_match,
         request->http.if_none_match,
         strlen(request->http.if_none_match) + 1U);
  memcpy(fake->if_modified_since,
         request->http.if_modified_since,
         strlen(request->http.if_modified_since) + 1U);
  fake->begins += 1U;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `priv_test_mdl_backend_read()`.
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_read(void*     ctx,
                                              uint8_t*  out,
                                              uint16_t  cap,
                                              uint16_t* got,
                                              uint64_t* total_bytes,
                                              bool*     complete,
                                              uint8_t   sha256[k_ra8_mdl_sha256_bytes],
                                              ra8_mdl_http_response_t* response)
{
  ra8_test_mdl_backend_t* fake = (ra8_test_mdl_backend_t*)ctx;
  *response                    = (ra8_mdl_http_response_t){};
  if (fake->read_fault == k_t_mdl_read_fault_oversize) {
    *got         = (uint16_t)(cap + 1U);
    *total_bytes = *got;
    *complete    = false;
    return k_ra8_ok;
  }
  if (fake->read_fault == k_t_mdl_read_fault_empty_active) {
    *got         = 0U;
    *total_bytes = fake->len;
    *complete    = false;
    return k_ra8_ok;
  }
  if (fake->read_fault == k_t_mdl_read_fault_terminal_data) {
    out[0]       = 'x';
    *got         = 1U;
    *total_bytes = 1U;
    *complete    = true;
    memset(sha256, k_t_mdl_digest_fill, k_ra8_mdl_sha256_bytes);
    response->status = 200;
    return k_ra8_ok;
  }
  const size_t left = fake->len - fake->at;
  const size_t take = (left < cap) ? left : cap;
  if (take != 0U) {
    memcpy(out, &fake->bytes[fake->at], take);
    fake->at += take;
  }
  *got         = (uint16_t)take;
  *total_bytes = fake->len;
  *complete    = (take == 0U);
  if (*complete) {
    if (fake->terminal_total_zero) {
      *total_bytes = 0U;
    }
    memset(sha256, k_t_mdl_digest_fill, k_ra8_mdl_sha256_bytes);
    if (fake->response_override) {
      *response = fake->response;
      return k_ra8_ok;
    }
    response->status = 200;
    memcpy(response->retry_after, "3", sizeof("3"));
    memcpy(response->etag, "\"fixture-etag\"", sizeof("\"fixture-etag\""));
    memcpy(response->last_modified,
           "Wed, 21 Oct 2015 07:28:00 GMT",
           sizeof("Wed, 21 Oct 2015 07:28:00 GMT"));
    memcpy(response->content_type, "application/x-rabook", sizeof("application/x-rabook"));
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `priv_test_mdl_backend_cancel()`.
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_cancel(void* ctx)
{
  ra8_test_mdl_backend_t* fake = (ra8_test_mdl_backend_t*)ctx;
  fake->cancels += 1U;
  return k_ra8_ok;
}

/**
 * @brief Rebind a fresh service and backend for one independent vector.
 * @details Every vector runs on its own job, so no injected fault or override
 * can survive into the next one.
 * @return Nothing.
 * @pre The shared backend callbacks are addressable.
 * @pre No dispatch is in progress on this service instance.
 * @post The backend serves the canonical six-byte artifact from offset zero.
 * @post The service is inactive with no job identifier issued.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_policy_reset(void)
{
  static const uint8_t bytes[] = {'a', 'b', 'c', 'd', 'e', 'f'};
  s_policy_backend             = (ra8_test_mdl_backend_t){.bytes = bytes, .len = sizeof(bytes)};
  const ra8_mdl_service_backend_t backend = {.begin  = priv_test_mdl_backend_begin,
                                             .read   = priv_test_mdl_backend_read,
                                             .cancel = priv_test_mdl_backend_cancel,
                                             .ctx    = &s_policy_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_policy_service, &backend));
}

/**
 * @brief Pull one bounded data chunk from the active vector job.
 * @details Packs a correlated NextRequest and decodes the accepted Chunk so a
 * vector can consume the artifact body before its terminal pull.
 * @param[in] job Active job identifier.
 * @param[in] offset Acknowledged body offset.
 * @param[in] max_bytes Requested bounded chunk size.
 * @return Decoded Chunk owned by the caller.
 * @retval non-NULL The accepted response, to be released by the caller.
 * @pre One job is active and the request tuple is protocol-valid.
 * @pre The response scratch buffer fits the worst-case chunk.
 * @post Service sequence and offset advance by exactly the returned bytes.
 * @post The caller owns the decoded message.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static Ra8__Mdl__Chunk*
internal_policy_next(uint32_t job, uint64_t offset, uint32_t max_bytes)
{
  Ra8__Mdl__NextRequest req = RA8__MDL__NEXT_REQUEST__INIT;
  req.protocol_version      = k_ra8_mdl_protocol_version;
  req.job_id                = job;
  req.acknowledged_offset   = offset;
  req.max_bytes             = max_bytes;
  const size_t request_len  = ra8__mdl__next_request__pack(&req, s_policy_request);
  size_t       response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_policy_service,
                                          k_ra8_mdl_rpc_next,
                                          s_policy_request,
                                          request_len,
                                          s_policy_response,
                                          sizeof(s_policy_response),
                                          &response_len));
  return ra8__mdl__chunk__unpack(nullptr, response_len, s_policy_response);
}

/**
 * @brief Dispatch one correlated Next the backend record must invalidate.
 * @details Packs an ordinary Next request so the overridden terminal record is
 * the only possible source of rejection.
 * @param[in] job Active job identifier.
 * @param[in] offset Acknowledged offset matching the service state.
 * @return Nothing.
 * @pre One job is active and the response override is configured.
 * @pre The response scratch buffer fits every canonical response.
 * @post Dispatch reports a protocol error and publishes no response bytes.
 * @post The backend is cancelled once and the job is deactivated.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_policy_expect_protocol_error(uint32_t job, uint64_t offset)
{
  Ra8__Mdl__NextRequest next = RA8__MDL__NEXT_REQUEST__INIT;
  next.protocol_version      = k_ra8_mdl_protocol_version;
  next.job_id                = job;
  next.acknowledged_offset   = offset;
  next.max_bytes             = 4U;
  const size_t request_len   = ra8__mdl__next_request__pack(&next, s_policy_request);
  size_t       response_len  = k_t_policy_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_policy_service,
                                          k_ra8_mdl_rpc_next,
                                          s_policy_request,
                                          request_len,
                                          s_policy_response,
                                          sizeof(s_policy_response),
                                          &response_len));
  TEST_ASSERT_EQ(0, response_len);
  TEST_ASSERT_EQ(1, s_policy_backend.cancels);
  TEST_ASSERT(!s_policy_service.active);
}

/**
 * @brief Fill one canonical StartRequest that the service must accept.
 * @details Every field is the truth-value control for
 * ::internal_mdl_start_valid: a supported protocol version, a bounded HTTPS
 * URL with a nonempty path, a concrete format, the maximum legal timeout, and
 * four bounded single-line request headers.
 * @param[out] request Generated request initialised in place.
 * @pre @p request is writable and not aliased by a live packed buffer.
 * @pre The referenced literals outlive the packing call.
 * @post Every validated field holds its accepted truth value.
 * @post No service or backend state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_fill_start_control(Ra8__Mdl__StartRequest* request)
{
  ra8__mdl__start_request__init(request);
  request->protocol_version  = k_ra8_mdl_protocol_version;
  request->url               = (char*)"https://example.test/book";
  request->format            = RA8__MDL__FORMAT__FORMAT_RABOOK;
  request->user_agent        = (char*)"ra8-test/3";
  request->referer           = (char*)"https://example.test/catalog";
  request->if_none_match     = (char*)"\"cached-etag\"";
  request->if_modified_since = (char*)"Tue, 20 Oct 2015 07:28:00 GMT";
  request->timeout_ms        = k_ra8_mdl_timeout_ms_max;
}

/**
 * @brief Reject one single-field variation of the canonical StartRequest.
 * @details Packs the candidate through the generated codec and proves the
 * service refuses it before any backend or service-state effect.
 * @param[in] request Candidate differing from the control in one field.
 * @pre The fixture was reset and no job is active.
 * @pre @p request packs within the fixed request scratch buffer.
 * @post Dispatch reports invalid-argument and publishes no response bytes.
 * @post The backend is never begun and the service stays inactive.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_expect_start_rejection(Ra8__Mdl__StartRequest* request)
{
  const size_t request_len = ra8__mdl__start_request__get_packed_size(request);
  TEST_ASSERT(request_len <= sizeof(s_policy_request));
  TEST_ASSERT_EQ((int64_t)request_len,
                 (int64_t)ra8__mdl__start_request__pack(request, s_policy_request));
  size_t response_len = k_t_policy_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mdl_service_dispatch(&s_policy_service,
                                          k_ra8_mdl_rpc_start,
                                          s_policy_request,
                                          request_len,
                                          s_policy_response,
                                          sizeof(s_policy_response),
                                          &response_len));
  TEST_ASSERT_EQ(0, response_len);
  TEST_ASSERT_EQ(0, s_policy_backend.begins);
  TEST_ASSERT(!s_policy_service.active);
}

/**
 * @brief Accept the canonical StartRequest and observe every forwarded field.
 * @details Establishes ::internal_mdl_start_valid's all-true control against
 * the same generated encoder every rejection vector uses.
 * @param[in] request Canonical request filled by ::internal_fill_start_control.
 * @return The accepted job identifier.
 * @retval nonzero The identifier the service issued for this job.
 * @pre The fixture was reset and no job is active.
 * @pre @p request packs within the fixed request scratch buffer.
 * @post Exactly one job is activated and its identifier is nonzero.
 * @post The backend observed every forwarded policy field verbatim.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint32_t internal_expect_start_acceptance(Ra8__Mdl__StartRequest* request)
{
  const size_t request_len = ra8__mdl__start_request__get_packed_size(request);
  TEST_ASSERT(request_len <= sizeof(s_policy_request));
  TEST_ASSERT_EQ((int64_t)request_len,
                 (int64_t)ra8__mdl__start_request__pack(request, s_policy_request));
  size_t response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_policy_service,
                                          k_ra8_mdl_rpc_start,
                                          s_policy_request,
                                          request_len,
                                          s_policy_response,
                                          sizeof(s_policy_response),
                                          &response_len));
  Ra8__Mdl__Accepted* accepted =
    ra8__mdl__accepted__unpack(nullptr, response_len, s_policy_response);
  TEST_ASSERT(accepted != nullptr);
  TEST_ASSERT(accepted->job_id != 0U);
  const uint32_t job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  TEST_ASSERT_EQ(1, s_policy_backend.begins);
  TEST_ASSERT_EQ(k_ra8_mdl_timeout_ms_max, s_policy_backend.timeout_ms);
  TEST_ASSERT(strcmp(s_policy_backend.user_agent, request->user_agent) == 0);
  TEST_ASSERT(strcmp(s_policy_backend.referer, request->referer) == 0);
  TEST_ASSERT(strcmp(s_policy_backend.if_none_match, request->if_none_match) == 0);
  TEST_ASSERT(strcmp(s_policy_backend.if_modified_since, request->if_modified_since) == 0);
  return job;
}

/**
 * @brief Reject each HTTP-policy variation of the canonical StartRequest.
 * @details Carries vectors V7 through V12 of the Start field matrix: the
 * timeout bound, one CR or LF inside each of the four conditional-request
 * headers, and a cap-length User-Agent.
 * @return Nothing.
 * @pre The fixture was reset and no job is active.
 * @pre The declared header caps fit the fixed request scratch buffer.
 * @post Every candidate is rejected before the backend is begun.
 * @post The service is still inactive on return.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_expect_start_policy_rejections(void)
{
  char capped_agent[k_ra8_mdl_user_agent_max + 1U];
  memset(capped_agent, 'a', sizeof(capped_agent));
  capped_agent[k_ra8_mdl_user_agent_max] = '\0';

  Ra8__Mdl__StartRequest candidate = RA8__MDL__START_REQUEST__INIT;
  internal_fill_start_control(&candidate);
  candidate.timeout_ms = k_ra8_mdl_timeout_ms_max + 1U;
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.user_agent = (char*)"ra8-test/3\rX-Injected: 1";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.referer = (char*)"https://example.test/catalog\nX-Injected: 1";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.if_none_match = (char*)"\"cached-etag\"\rX-Injected: 1";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.if_modified_since = (char*)"Tue, 20 Oct 2015 07:28:00 GMT\nX-Injected: 1";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.user_agent = capped_agent;
  internal_expect_start_rejection(&candidate);
}

/**
 * @test internal_test_start_request_fields_mcdc
 * @brief Prove every Start field operand independently rejects a request.
 * @par MC/DC:
 * The canonical request accepted first is the all-true control for
 * ::internal_mdl_start_valid's eleven-condition conjunction. Each vector then
 * flips exactly one conjunct while every other keeps its accepted truth value:
 * - V1 protocol_version + 1 -> only the version conjunct false.
 * - V2 an empty URL -> only `url_len != 0` false.
 * - V3 a cap-length HTTPS URL -> only `url_len < k_ra8_mdl_url_max` false.
 * - V4 an `http://` URL -> only the scheme comparison false.
 * - V5 the bare prefix `https://` -> only `url[prefix] != '\\0'` false.
 * - V6 the reserved invalid format -> only the format bound false.
 * - V7 timeout_ms = max + 1 -> only the timeout bound false.
 * - V8 a CR inside User-Agent -> only its field check false.
 * - V9 an LF inside Referer -> only its field check false.
 * - V10 a CR inside If-None-Match -> only its field check false.
 * - V11 an LF inside If-Modified-Since -> only its field check false.
 * - V12 a cap-length User-Agent -> the same conjunct false through the length
 *   bound rather than the line-break scan.
 * For the inner `(text[index] == '\\r') || (text[index] == '\\n')` decision the
 * control walks four header values whose every byte is F,F/false; V8 and V10
 * execute T,-/true while V9 and V11 execute F,T/true, so both conjuncts
 * independently decide.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_start_valid
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_request_field_valid
 * @details Packs each candidate with the generated codec so no vector depends
 * on a hand-built wire frame.
 * @pre The deterministic fixture can be reset before every vector.
 * @pre The declared header caps fit the fixed request scratch buffer.
 * @post The accepted control activates exactly one job, which is cancelled.
 * @post No rejected candidate reaches the backend.
 * @note The `request->url == nullptr` guard is unreachable from the wire: the
 * generated decoder initialises an omitted URL to the shared empty string,
 * which V2 covers instead.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_start_request_fields_mcdc(void)
{
  TEST_BEGIN("mdl start request fields MC/DC");
  Ra8__Mdl__StartRequest control = RA8__MDL__START_REQUEST__INIT;
  internal_fill_start_control(&control);
  internal_policy_reset();
  TEST_ASSERT(internal_expect_start_acceptance(&control) != 0U);

  char capped_url[k_ra8_mdl_url_max + 1U];
  memset(capped_url, 'u', sizeof(capped_url));
  memcpy(capped_url, "https://", sizeof("https://") - 1U);
  capped_url[k_ra8_mdl_url_max] = '\0';

  Ra8__Mdl__StartRequest candidate = RA8__MDL__START_REQUEST__INIT;
  internal_policy_reset();
  internal_fill_start_control(&candidate);
  candidate.protocol_version = k_ra8_mdl_protocol_version + 1U;
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.url = (char*)"";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.url = capped_url;
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.url = (char*)"http://example.test/book";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.url = (char*)"https://";
  internal_expect_start_rejection(&candidate);

  internal_fill_start_control(&candidate);
  candidate.format = RA8__MDL__FORMAT__FORMAT_INVALID;
  internal_expect_start_rejection(&candidate);

  internal_expect_start_policy_rejections();
  TEST_END("mdl start request fields MC/DC");
}

/**
 * @brief Fill the canonical terminal response metadata the service accepts.
 * @details Mirrors the fixture backend's ordinary terminal record, which is
 * ::internal_mdl_response_valid's all-true control.
 * @param[out] response Fixed response record filled in place.
 * @pre @p response is writable for its complete extent.
 * @pre Every literal fits its declared array including the terminator.
 * @post Status and all four selected headers hold accepted truth values.
 * @post No service or backend state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_fill_response_control(ra8_mdl_http_response_t* response)
{
  *response        = (ra8_mdl_http_response_t){};
  response->status = (int32_t)k_ra8_mdl_http_status_min + 100;
  memcpy(response->retry_after, "3", sizeof("3"));
  memcpy(response->etag, "\"fixture-etag\"", sizeof("\"fixture-etag\""));
  memcpy(response->last_modified,
         "Wed, 21 Oct 2015 07:28:00 GMT",
         sizeof("Wed, 21 Oct 2015 07:28:00 GMT"));
  memcpy(response->content_type, "application/x-rabook", sizeof("application/x-rabook"));
}

/**
 * @brief Reject one single-field variation of the terminal response metadata.
 * @details Consumes the complete artifact body first so the faulted pull is
 * the terminal one, then proves the service refuses to publish it.
 * @param[in] response Candidate differing from the control in one field.
 * @pre The deterministic fixture can be reset and started.
 * @pre @p response describes exactly one mutated field.
 * @post Dispatch reports a protocol error and publishes no response bytes.
 * @post The backend is cancelled once and the job is deactivated.
 * @note File-local helper; each call creates an independent job.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_expect_terminal_response_rejection(const ra8_mdl_http_response_t* response)
{
  internal_policy_reset();
  Ra8__Mdl__StartRequest control = RA8__MDL__START_REQUEST__INIT;
  internal_fill_start_control(&control);
  const uint32_t   job  = internal_expect_start_acceptance(&control);
  Ra8__Mdl__Chunk* data = internal_policy_next(job, 0U, 8U);
  TEST_ASSERT(data != nullptr);
  TEST_ASSERT_EQ(6, data->data.len);
  ra8__mdl__chunk__free_unpacked(data, nullptr);
  s_policy_backend.response_override = true;
  s_policy_backend.response          = *response;
  internal_policy_expect_protocol_error(job, 6U);
}

/**
 * @test internal_test_response_metadata_mcdc
 * @brief Prove every backend response-metadata operand rejects a pull.
 * @par MC/DC:
 * The terminal pull in `internal_test_service_multichunk_and_digest` is the
 * all-true control for ::internal_mdl_response_valid: status 200 with four
 * bounded single-line headers. Each vector below overrides exactly one field
 * of that record:
 * - V1 status 99 -> only `status >= k_ra8_mdl_http_status_min` false.
 * - V2 status 600 -> only `status <= k_ra8_mdl_http_status_max` false.
 * - V3 a CR inside Retry-After -> only its field check false.
 * - V4 an LF inside ETag -> only its field check false.
 * - V5 a CR inside Last-Modified -> only its field check false.
 * - V6 an LF inside Content-Type -> only its field check false.
 * - V7 an ETag array with no terminator -> the same conjunct false through
 *   the length bound rather than the line-break scan.
 * For the inner `(text[index] == '\\r') || (text[index] == '\\n')` decision the
 * control walks four header values whose every byte is F,F/false, V3 and V5
 * execute T,-/true, and V4 and V6 execute F,T/true.
 * The `text == nullptr` guard of ::internal_mdl_request_field_valid is
 * unreachable from here: every selected header is a fixed array inside the
 * response record, so its address is never null.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_response_valid
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_request_field_valid
 * @details Each vector runs on a fresh job whose body is fully consumed, so
 * only the terminal record can be the cause of rejection.
 * @pre The deterministic fixture can be reset and started.
 * @pre The canonical six-byte artifact remains the modelled body.
 * @post Every mutated record returns a protocol error with no response bytes.
 * @post Every rejected job is cancelled once and deactivated.
 * @note A rejected terminal record must never reach the caller's chunk.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_response_metadata_mcdc(void)
{
  TEST_BEGIN("mdl response metadata MC/DC");
  ra8_mdl_http_response_t candidate = {};
  internal_fill_response_control(&candidate);
  candidate.status = (int32_t)k_ra8_mdl_http_status_min - 1;
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  candidate.status = (int32_t)k_ra8_mdl_http_status_max + 1;
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  memcpy(candidate.retry_after, "3\rX-Injected: 1", sizeof("3\rX-Injected: 1"));
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  memcpy(candidate.etag, "\"etag\"\nX-Injected: 1", sizeof("\"etag\"\nX-Injected: 1"));
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  memcpy(candidate.last_modified,
         "Wed, 21 Oct 2015 07:28:00 GMT\r",
         sizeof("Wed, 21 Oct 2015 07:28:00 GMT\r"));
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  memcpy(candidate.content_type, "application/x-rabook\nX", sizeof("application/x-rabook\nX"));
  internal_expect_terminal_response_rejection(&candidate);

  internal_fill_response_control(&candidate);
  memset(candidate.etag, 'e', sizeof(candidate.etag));
  internal_expect_terminal_response_rejection(&candidate);
  TEST_END("mdl response metadata MC/DC");
}

/**
 * @brief Implementation of `priv_test_mdl_policy_run()`.
 */
RA8_PRIV void priv_test_mdl_policy_run(void)
{
  internal_test_start_request_fields_mcdc();
  internal_test_response_metadata_mcdc();
}
