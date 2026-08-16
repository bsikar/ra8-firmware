/**
 * @file test_media_dl_net_c6link.c
 * @brief Focused coverage for the media C6 network adapter.
 * @details Replaces only the already-tested transfer coordinator with a
 * deterministic synchronous script, then drives the production network
 * adapter through its public vtable. The vectors cover bounded buffer and
 * streaming outputs, request/response propagation, HTTP classification,
 * cleanup, sink faults, transfer faults, and constructor validation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mdl_net_c6link.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"

/** @brief Exact capacities for deterministic C6 adapter vectors. */
typedef enum : uint16_t {
  k_internal_body_bytes = 6U,  /**< Scripted response body length. */
  k_internal_sink_bytes = 16U, /**< Caller-owned sink capacity.    */
} internal_limit_t;

/** @brief Transfer-coordinator script observed by the link-time stub. */
typedef struct {
  ra8_err_t result;                  /**< Injected transport result.      */
  long      status;                  /**< Terminal HTTP status.           */
  bool      probe_callbacks;         /**< Exercise rejected begin inputs. */
  bool      commit_fail;             /**< Inject commit failure.          */
  bool      saw_policy;              /**< Request policy matched fixture. */
  ra8_err_t null_begin;              /**< Null-context begin result.      */
  ra8_err_t empty_destination_begin; /**< Empty-destination begin result. */
  uint32_t  abort_count;             /**< Coordinator-requested aborts.   */
} internal_transfer_script_t;

/** @brief Caller-owned streaming sink with deterministic faults. */
typedef struct {
  uint8_t bytes[k_internal_sink_bytes]; /**< Accepted bytes.           */
  size_t  length;                       /**< Visible byte count.       */
  uint8_t reset_count;                  /**< Reset callback count.     */
  bool    fail_write;                   /**< Inject write failure.     */
  bool    short_write;                  /**< Report short success.     */
  bool    fail_second_reset;            /**< Fail coordinator cleanup. */
} internal_sink_t;

static const uint8_t              s_body[k_internal_body_bytes] = {'c', '6', '-', 'n', 'e', 't'};
static uint8_t                    s_hash_ctx; /**< Non-null caller-owned hash placeholder.   */
static ra8_c6link_t               s_link;     /**< Opaque link identity retained by adapter. */
static internal_transfer_script_t s_script;   /**< Active coordinator script.                */

/**
 * @brief Reset the transfer script to one successful HTTP response.
 * @details Clears every observation and selects status 200 with successful
 * transport and storage publication.
 * @pre No scripted transfer is active.
 * @pre Global test state is exclusively owned.
 * @post The next transfer returns the deterministic body with status 200.
 * @post Every observation and fault flag is clear.
 * @note Test helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_script_reset(void)
{
  s_script = (internal_transfer_script_t){.result = k_ra8_ok, .status = 200L};
}

/**
 * @brief Provide a successful no-op hash initializer.
 * @details Validates only the retained fixture context because the real
 * coordinator and digest implementation are covered in their own suite.
 * @param[in,out] ctx Bound non-null placeholder context.
 * @return Placeholder initialization status.
 * @retval k_ra8_ok The fixture context is non-null.
 * @retval k_ra8_err_null_ptr The fixture context is null.
 * @pre @p ctx is non-null.
 * @pre No concurrent vector uses the placeholder.
 * @post The context remains valid.
 * @post No external state changes.
 * @note The coordinator itself is replaced in this focused adapter test.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_hash_init(void* ctx)
{
  return (ctx != nullptr) ? k_ra8_ok : k_ra8_err_null_ptr;
}

/**
 * @brief Accept one placeholder hash update.
 * @details Validates the context and byte pointer without duplicating the real
 * digest behavior exercised by the coordinator integration tests.
 * @param[in,out] ctx Bound non-null placeholder context.
 * @param[in] data Readable transfer bytes.
 * @param[in] len Valid byte count.
 * @return Placeholder update status.
 * @retval k_ra8_ok Both fixture pointers are non-null.
 * @retval k_ra8_err_null_ptr A fixture pointer is null.
 * @pre @p ctx and @p data are non-null.
 * @pre @p data spans @p len readable bytes.
 * @post No caller-visible state changes.
 * @post The input remains unchanged.
 * @note The real digest seam is covered by the C6 transfer suite.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_hash_update(void* ctx, const uint8_t* data, uint16_t len)
{
  (void)len;
  return ((ctx != nullptr) && (data != nullptr)) ? k_ra8_ok : k_ra8_err_null_ptr;
}

/**
 * @brief Write a deterministic placeholder digest.
 * @details Validates both pointers and fills the exact digest span with zeros;
 * the link-time transfer stub does not consume the digest value.
 * @param[in,out] ctx Bound non-null placeholder context.
 * @param[out] out Exact SHA-256 output span.
 * @return Placeholder finalization status.
 * @retval k_ra8_ok The zero digest was written.
 * @retval k_ra8_err_null_ptr A fixture pointer is null.
 * @pre @p ctx and @p out are non-null.
 * @pre @p out spans ::k_ra8_mdl_sha256_bytes bytes.
 * @post The output digest contains only zero bytes.
 * @post No other state changes.
 * @note The real digest implementation is tested with the coordinator.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_hash_final(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes])
{
  if ((ctx == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  (void)memset(out, 0, k_ra8_mdl_sha256_bytes);
  return k_ra8_ok;
}

/**
 * @brief Clear the caller-owned scripted sink.
 * @details Counts reset attempts and optionally fails the second reset used
 * to clean up a failed transfer.
 * @param[in,out] ctx Bound ::internal_sink_t.
 * @return Reset status or the injected cleanup failure.
 * @retval k_ra8_ok The sink was cleared.
 * @retval k_ra8_fail The second-reset fault fired.
 * @pre @p ctx points to writable sink state.
 * @pre The sink is exclusively owned.
 * @post Success clears bytes and visible length.
 * @post Failure preserves the previously visible body.
 * @note Test helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sink_reset(void* ctx)
{
  internal_sink_t* sink = (internal_sink_t*)ctx;
  ++sink->reset_count;
  if (sink->fail_second_reset && (sink->reset_count > 1U)) {
    return k_ra8_fail;
  }
  (void)memset(sink->bytes, 0, sizeof(sink->bytes));
  sink->length = 0U;
  return k_ra8_ok;
}

/**
 * @brief Consume one scripted response fragment.
 * @details Supports exact success, an injected sink error, and a deliberately
 * short successful write for adapter progress validation.
 * @param[in,out] ctx Bound ::internal_sink_t.
 * @param[in] bytes Readable response bytes.
 * @param[in] length Valid byte count.
 * @param[out] out_written Reported accepted byte count.
 * @return Scripted sink status.
 * @retval k_ra8_ok The exact or deliberately short count was reported.
 * @retval k_ra8_fail The injected write fault fired.
 * @pre Pointer spans are valid and exclusively owned.
 * @pre The sink was reset for this attempt.
 * @post Exact success appends the complete fragment.
 * @post Fault paths leave the visible length unchanged.
 * @note Test helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_sink_write(void* ctx, const uint8_t* bytes, uint32_t length, uint32_t* out_written)
{
  internal_sink_t* sink = (internal_sink_t*)ctx;
  *out_written          = 0U;
  if (sink->fail_write) {
    return k_ra8_fail;
  }
  if (sink->short_write) {
    *out_written = length - 1U;
    return k_ra8_ok;
  }
  TEST_ASSERT(sink->length + length <= sizeof(sink->bytes));
  (void)memcpy(&sink->bytes[sink->length], bytes, length);
  sink->length += length;
  *out_written = length;
  return k_ra8_ok;
}

/**
 * @brief Replace the transfer coordinator with one bounded synchronous script.
 * @details Exercises the adapter-provided storage transaction in production
 * order, records request policy, and publishes deterministic terminal metadata.
 * @param[in,out] link Expected fixture link identity.
 * @param[in] url Expected fixture URL.
 * @param[in] destination Nonempty adapter-owned placeholder.
 * @param[in] config Adapter storage, policy, hash, and transfer bounds.
 * @param[out] result Scripted terminal result.
 * @return Scripted transport, storage, or progress status.
 * @pre All arguments are non-null and exclusively owned.
 * @pre The script was reset before the call.
 * @post Success commits exactly ::s_body and fills terminal metadata.
 * @post Failure after begin invokes adapter abort exactly once.
 * @note Link-time seam for this executable only; the coordinator has its own
 * tests over real protobuf and SHA-256.
 * @since 0.1.0
 */
ra8_err_t ra8_c6link_mdl_transfer(ra8_c6link_t*                    link,
                                  const char*                      url,
                                  const char*                      destination,
                                  const ra8_mdl_transfer_config_t* config,
                                  ra8_mdl_transfer_result_t*       result)
{
  TEST_ASSERT(link == &s_link);
  TEST_ASSERT(strcmp(url, "https://example.test/body") == 0);
  TEST_ASSERT(config->format == k_ra8_mdl_format_loose);
  TEST_ASSERT(config->storage.validate == nullptr);
  s_script.saw_policy =
    (strcmp(config->http.user_agent, "coverage-agent") == 0) &&
    (strcmp(config->http.referer, "https://example.test/index") == 0) &&
    (strcmp(config->http.if_none_match, "\"prior\"") == 0) &&
    (strcmp(config->http.if_modified_since, "Wed, 21 Oct 2015 07:28:00 GMT") == 0) &&
    (config->http.timeout_ms == 4321U);
  if (s_script.probe_callbacks) {
    s_script.null_begin              = config->storage.begin(nullptr, destination);
    s_script.empty_destination_begin = config->storage.begin(config->storage.ctx, "");
  }
  if (s_script.result != k_ra8_ok) {
    return s_script.result;
  }
  ra8_err_t status = config->storage.begin(config->storage.ctx, destination);
  if (status != k_ra8_ok) {
    return status;
  }
  uint16_t written = 0U;
  status           = config->storage.write(config->storage.ctx, s_body, sizeof(s_body), &written);
  if ((status == k_ra8_ok) && (written != sizeof(s_body))) {
    status = k_ra8_err_invalid_size;
  }
  if ((status == k_ra8_ok) && s_script.commit_fail) {
    status = k_ra8_fail;
  }
  if (status == k_ra8_ok) {
    status = config->storage.commit(config->storage.ctx);
  }
  if (status != k_ra8_ok) {
    ++s_script.abort_count;
    (void)config->storage.abort(config->storage.ctx);
    return status;
  }
  *result = (ra8_mdl_transfer_result_t){
    .bytes_stored = sizeof(s_body),
    .format       = k_ra8_mdl_format_loose,
    .response     = {.status        = s_script.status,
                     .retry_after   = "7",
                     .etag          = "\"fixture\"",
                     .last_modified = "Wed, 21 Oct 2015 07:28:00 GMT",
                     .content_type  = "application/octet-stream"},
  };
  return k_ra8_ok;
}

/**
 * @brief Bind one production C6 network adapter to fixture seams.
 * @details Supplies complete placeholder SHA callbacks, the opaque link
 * identity, and deliberately small nonzero pull bounds to the real factory.
 * @param[out] net Portable network interface.
 * @param[out] backend Caller-owned C6 adapter state.
 * @pre The script is reset and no request is active.
 * @pre Static fixture storage is exclusively owned.
 * @post Both objects are initialized for one request.
 * @post The adapter retains only caller-owned fixture addresses.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(mdl_net_iface_t* net, mdl_net_c6link_t* backend)
{
  const ra8_mdl_sha256_iface_t sha = {
    .init   = internal_hash_init,
    .update = internal_hash_update,
    .final  = internal_hash_final,
    .ctx    = &s_hash_ctx,
  };
  TEST_ASSERT_EQ(k_ra8_ok, mdl_net_c6link_init(net, backend, &s_link, &sha, 4U, 4U));
}

/**
 * @brief Return the shared deterministic HTTP request policy.
 * @details Constructs all supported request fields so the transfer stub can
 * prove the adapter forwards each value without rewriting it.
 * @return Complete immutable-by-value request policy.
 * @retval mdl_net_req_t Deterministic header and timeout values.
 * @pre Fixture string literals have static lifetime.
 * @pre Callers do not mutate the returned pointer targets.
 * @post Every optional request header is non-null.
 * @post The timeout is the deterministic fixture value.
 * @note Returned by value and safe for independent test vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_net_req_t internal_request(void)
{
  return (mdl_net_req_t){
    .user_agent        = "coverage-agent",
    .referer           = "https://example.test/index",
    .if_none_match     = "\"prior\"",
    .if_modified_since = "Wed, 21 Oct 2015 07:28:00 GMT",
    .timeout_ms        = 4321U,
  };
}

/**
 * @test internal_test_buffer_policy_and_status
 * @brief Buffer output preserves policy, body, and terminal metadata.
 * @details Also asks the transfer stub to probe the adapter's rejected begin
 * arguments through the injected transaction callbacks.
 * @pre Static fixture state is exclusively owned.
 * @pre The caller buffer can hold the body plus NUL.
 * @post Exact body bytes and terminal response metadata are visible.
 * @post Invalid begin inputs are rejected without mutating success output.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_buffer_policy_and_status(void)
{
  TEST_BEGIN("c6 adapter buffer and policy");
  internal_script_reset();
  s_script.probe_callbacks = true;
  mdl_net_iface_t  net;
  mdl_net_c6link_t backend;
  internal_bind(&net, &backend);
  const mdl_net_req_t request                     = internal_request();
  char                body[k_internal_sink_bytes] = "old";
  size_t              length                      = 99U;
  mdl_net_resp_t      response;
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_net_get_buf(&net,
                                 "https://example.test/body",
                                 &request,
                                 body,
                                 sizeof(body),
                                 &length,
                                 &response));
  TEST_ASSERT(s_script.saw_policy);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, s_script.null_begin);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, s_script.empty_destination_begin);
  TEST_ASSERT_EQ(sizeof(s_body), length);
  TEST_ASSERT(memcmp(body, s_body, sizeof(s_body)) == 0);
  TEST_ASSERT_EQ('\0', body[sizeof(s_body)]);
  TEST_ASSERT_EQ(200, response.status);
  TEST_ASSERT(strcmp(response.retry_after, "7") == 0);
  TEST_ASSERT(strcmp(response.etag, "\"fixture\"") == 0);
  TEST_ASSERT(strcmp(response.content_type, "application/octet-stream") == 0);
  mdl_net_destroy(&net);
  TEST_END("c6 adapter buffer and policy");
}

/**
 * @test internal_test_stream_and_sink_faults
 * @brief Streaming output accepts exact progress and rejects sink faults.
 * @details Exercises success, explicit sink failure, short-success rejection,
 * and cleanup-reset failure while retaining the first causal status.
 * @pre Static fixture state is exclusively owned.
 * @pre Each vector uses fresh adapter and sink state.
 * @post Success publishes the exact body.
 * @post Every failure attempts one coordinator abort.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stream_and_sink_faults(void)
{
  TEST_BEGIN("c6 adapter sink faults");
  const mdl_net_req_t request = internal_request();
  for (uint8_t vector = 0U; vector < 3U; ++vector) {
    internal_script_reset();
    mdl_net_iface_t  net;
    mdl_net_c6link_t backend;
    internal_bind(&net, &backend);
    internal_sink_t sink_state = {
      .fail_write        = vector == 1U,
      .short_write       = vector == 2U,
      .fail_second_reset = vector == 1U,
    };
    mdl_net_body_sink_t sink = {
      .reset = internal_sink_reset,
      .write = internal_sink_write,
      .ctx   = &sink_state,
    };
    size_t          length = 0U;
    const ra8_err_t expected =
      (vector == 0U) ? k_ra8_ok : ((vector == 1U) ? k_ra8_fail : k_ra8_err_invalid_size);
    TEST_ASSERT_EQ(
      expected,
      mdl_net_get_body(&net, "https://example.test/body", &request, &sink, &length, nullptr));
    if (vector == 0U) {
      TEST_ASSERT_EQ(sizeof(s_body), sink_state.length);
      TEST_ASSERT(memcmp(sink_state.bytes, s_body, sizeof(s_body)) == 0);
      TEST_ASSERT_EQ(0, s_script.abort_count);
    } else {
      TEST_ASSERT_EQ(1, s_script.abort_count);
    }
    mdl_net_destroy(&net);
  }
  TEST_END("c6 adapter sink faults");
}

/**
 * @test internal_test_http_fail_closed
 * @brief HTTP error classes publish metadata but no response body.
 * @details Drives the not-found, throttle, and server-failure classifications
 * after a complete transport transaction and verifies cleanup after commit.
 * @pre Static fixture state is exclusively owned.
 * @pre Each vector binds a fresh adapter.
 * @post Each vector retains its observed status and canonical error.
 * @post Every caller buffer is empty despite the completed transport body.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_http_fail_closed(void)
{
  TEST_BEGIN("c6 adapter HTTP failures clear output");
  const long          statuses[] = {404L, 429L, 500L};
  const ra8_err_t     expected[] = {k_ra8_err_not_found, k_ra8_err_busy, k_ra8_fail};
  const mdl_net_req_t request    = internal_request();
  for (size_t index = 0U; index < (sizeof(statuses) / sizeof(statuses[0])); ++index) {
    internal_script_reset();
    s_script.status = statuses[index];
    mdl_net_iface_t  net;
    mdl_net_c6link_t backend;
    internal_bind(&net, &backend);
    char           body[k_internal_sink_bytes] = "old";
    mdl_net_resp_t response;
    TEST_ASSERT_EQ(expected[index],
                   mdl_net_get_buf(&net,
                                   "https://example.test/body",
                                   &request,
                                   body,
                                   sizeof(body),
                                   nullptr,
                                   &response));
    TEST_ASSERT_EQ('\0', body[0]);
    TEST_ASSERT_EQ(statuses[index], response.status);
    mdl_net_destroy(&net);
  }
  TEST_END("c6 adapter HTTP failures clear output");
}

/**
 * @test internal_test_local_failures
 * @brief Capacity, commit, and transport failures preserve publication rules.
 * @details Exercises local output exhaustion, publication failure after a
 * complete write, and transport failure before the transaction begins.
 * @pre Static fixture state is exclusively owned.
 * @pre Each vector binds a fresh adapter.
 * @post Post-begin failures clear the caller output through abort.
 * @post A pre-begin transport failure leaves caller storage untouched.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_local_failures(void)
{
  TEST_BEGIN("c6 adapter local failures");
  const mdl_net_req_t request = internal_request();
  internal_script_reset();
  mdl_net_iface_t  net;
  mdl_net_c6link_t backend;
  internal_bind(&net, &backend);
  char small[sizeof(s_body)] = "old";
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 mdl_net_get_buf(&net,
                                 "https://example.test/body",
                                 &request,
                                 small,
                                 sizeof(small),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT_EQ('\0', small[0]);
  TEST_ASSERT_EQ(1, s_script.abort_count);
  mdl_net_destroy(&net);

  internal_script_reset();
  s_script.commit_fail = true;
  internal_bind(&net, &backend);
  char body[k_internal_sink_bytes] = "old";
  TEST_ASSERT_EQ(k_ra8_fail,
                 mdl_net_get_buf(&net,
                                 "https://example.test/body",
                                 &request,
                                 body,
                                 sizeof(body),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT_EQ('\0', body[0]);
  mdl_net_destroy(&net);

  internal_script_reset();
  s_script.result = k_ra8_err_timeout;
  internal_bind(&net, &backend);
  (void)memcpy(body, "old", 4U);
  TEST_ASSERT_EQ(k_ra8_err_timeout,
                 mdl_net_get_buf(&net,
                                 "https://example.test/body",
                                 &request,
                                 body,
                                 sizeof(body),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT(strcmp(body, "old") == 0);
  mdl_net_destroy(&net);
  TEST_END("c6 adapter local failures");
}

/**
 * @test internal_test_constructor_validation
 * @brief Constructor rejects every invalid pointer and transfer bound.
 * @details Mutates one SHA callback or bound at a time and verifies outputs
 * are cleared whenever their addresses can be validated.
 * @pre No request is active.
 * @pre Fixture objects are writable and exclusively owned.
 * @post Invalid arguments return the documented status.
 * @post The final valid bind can be destroyed safely.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_constructor_validation(void)
{
  TEST_BEGIN("c6 adapter constructor validation");
  mdl_net_iface_t        net = {.ctx = &s_hash_ctx};
  mdl_net_c6link_t       backend;
  ra8_mdl_sha256_iface_t sha = {
    .init   = internal_hash_init,
    .update = internal_hash_update,
    .final  = internal_hash_final,
    .ctx    = &s_hash_ctx,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(nullptr, &backend, &s_link, &sha, 1U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, nullptr, &s_link, &sha, 1U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, nullptr, &sha, 1U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, &s_link, nullptr, 1U, 1U));
  sha.init = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 1U));
  sha.init   = internal_hash_init;
  sha.update = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 1U));
  sha.update = internal_hash_update;
  sha.final  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 1U));
  sha.final = internal_hash_final;
  sha.ctx   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 1U));
  sha.ctx = &s_hash_ctx;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_net_c6link_init(&net, &backend, &s_link, &sha, 0U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_net_c6link_init(&net,
                                     &backend,
                                     &s_link,
                                     &sha,
                                     (uint16_t)(k_ra8_mdl_chunk_data_max + 1U),
                                     1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, mdl_net_c6link_init(&net, &backend, &s_link, &sha, 1U, 1U));
  mdl_net_destroy(&net);
  mdl_net_destroy(nullptr);
  TEST_END("c6 adapter constructor validation");
}

int main(void)
{
  internal_test_buffer_policy_and_status();
  internal_test_stream_and_sink_faults();
  internal_test_http_fail_closed();
  internal_test_local_failures();
  internal_test_constructor_validation();
  return 0;
}
