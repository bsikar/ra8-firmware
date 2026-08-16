/**
 * @file test_ra8_mdl_net_c6link.c
 * @brief C6link-backed media network adapter contract tests.
 * @details Drives real generated protobuf through the modelled C6 service,
 * verifies the body with the real software SHA-256 stream, and exercises both
 * downloader output shapes plus fail-closed policy and digest faults.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mdl_net_c6link.h"
#include "ra8_attributes.h"
#include "ra8_c6_model.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_rsip.h"
#include "unity_minimal.h"

/** @brief Exact capacities for the bounded adapter fixture. */
typedef enum : uint16_t {
  k_internal_chunk_bytes = 5U,  /**< Deliberately fragments the body. */
  k_internal_max_chunks  = 16U, /**< Absolute pull bound.             */
  k_internal_sink_bytes  = 64U, /**< RAM sink capacity.               */
} internal_c6_net_limit_t;

/** @brief Caller-owned body sink state. */
typedef struct {
  uint8_t bytes[k_internal_sink_bytes]; /**< Accepted body bytes.  */
  size_t  length;                       /**< Valid byte count.     */
  bool    fail_reset;                   /**< Inject reset failure. */
} internal_c6_sink_t;

/** @brief SHA context plus terminal corruption injection. */
typedef struct {
  ra8_rsip_sha256_ctx_t sha;       /**< Real streaming SHA state. */
  bool                  bad_final; /**< Corrupt finalized digest. */
} internal_c6_sha_t;

static const uint8_t     s_body[] = "portable-c6-body"; /**< Deterministic HTTPS body. */
static internal_c6_sha_t s_sha;                         /**< Adapter SHA state.        */

/**
 * @brief Reset the real streaming digest context.
 * @details Delegates to the production SHA-256 initializer over the fixture's
 *          caller-owned context.
 * @param[in,out] ctx Bound ::internal_c6_sha_t.
 * @return SHA initialization status.
 * @retval k_ra8_ok A fresh digest operation is active.
 * @retval k_ra8_fail The production SHA initializer rejected the context.
 * @pre @p ctx is non-null and exclusively owned.
 * @pre No prior digest operation still owns the context.
 * @post Success starts a fresh SHA-256 operation.
 * @post Failure leaves no test output published.
 * @note Test adapter; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sha_init(void* ctx)
{
  return ra8_rsip_sha256_init(&((internal_c6_sha_t*)ctx)->sha);
}

/**
 * @brief Feed one ordered C6 chunk into the real digest.
 * @details Exercises the production incremental SHA update on exactly the
 *          fragment delivered by the modelled C6 transfer.
 * @param[in,out] ctx Bound ::internal_c6_sha_t.
 * @param[in] data Chunk bytes.
 * @param[in] len Valid byte count.
 * @return SHA update status.
 * @retval k_ra8_ok The complete fragment was incorporated.
 * @retval k_ra8_fail The production SHA update rejected the fragment.
 * @pre Pointers are non-null and @p data spans @p len bytes.
 * @pre A digest operation was initialized on @p ctx.
 * @post Success incorporates exactly @p len bytes.
 * @post Failure publishes no digest or response bytes.
 * @note Test adapter; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sha_update(void* ctx, const uint8_t* data, uint16_t len)
{
  return ra8_rsip_sha256_update(&((internal_c6_sha_t*)ctx)->sha, data, len);
}

/**
 * @brief Finalize and optionally corrupt the local digest.
 * @details Finalizes the production SHA stream and flips one result bit only
 *          when the fixture requests a terminal integrity fault.
 * @param[in,out] ctx Bound ::internal_c6_sha_t.
 * @param[out] out Exact SHA-256 destination.
 * @return SHA finalization status.
 * @retval k_ra8_ok One possibly fault-injected digest was written.
 * @retval k_ra8_fail The production finalizer rejected the active stream.
 * @pre Pointers are non-null and a digest operation is active.
 * @pre @p out spans exactly ::k_ra8_mdl_sha256_bytes writable bytes.
 * @post Success writes one digest and honors the fault flag.
 * @post Failure does not deliberately mutate the digest for fault injection.
 * @note The fault proves remote digest comparison is non-vacuous.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sha_final(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes])
{
  internal_c6_sha_t* state  = (internal_c6_sha_t*)ctx;
  const ra8_err_t    result = ra8_rsip_sha256_final(&state->sha, out);
  if ((result == k_ra8_ok) && state->bad_final) {
    out[0] ^= UINT8_C(0x01);
  }
  return result;
}

/**
 * @brief Clear one caller-owned RAM body sink.
 * @details Applies the reset fault first, otherwise clears both the retained
 *          body bytes and their visible length.
 * @param[in,out] ctx Bound ::internal_c6_sink_t.
 * @return Reset status or injected failure.
 * @retval k_ra8_ok The sink is empty and reusable.
 * @retval k_ra8_fail The configured reset fault fired.
 * @pre @p ctx is non-null.
 * @pre @p ctx points to a writable ::internal_c6_sink_t.
 * @post Success clears length and bytes.
 * @post Injected failure preserves the prior sink contents.
 * @note Test adapter; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_sink_reset(void* ctx)
{
  internal_c6_sink_t* sink = (internal_c6_sink_t*)ctx;
  if (sink->fail_reset) {
    return k_ra8_fail;
  }
  memset(sink->bytes, 0, sizeof(sink->bytes));
  sink->length = 0U;
  return k_ra8_ok;
}

/**
 * @brief Append one response chunk to the RAM body sink.
 * @details Performs a bounded append into the fixture buffer and reports exact
 *          progress through the downloader sink contract.
 * @param[in,out] ctx Bound ::internal_c6_sink_t.
 * @param[in] bytes Response bytes.
 * @param[in] length Valid byte count.
 * @param[out] out_written Exact accepted byte count.
 * @return Sink status.
 * @retval k_ra8_ok Every byte fit.
 * @retval k_ra8_err_no_mem The fixed sink is full.
 * @pre Pointer spans are valid and exclusively owned.
 * @pre @p out_written is writable and distinct from the source span.
 * @post Success appends exactly @p length bytes.
 * @post Failure reports zero accepted bytes and preserves the visible length.
 * @note Test adapter; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_sink_write(void* ctx, const uint8_t* bytes, uint32_t length, uint32_t* out_written)
{
  internal_c6_sink_t* sink = (internal_c6_sink_t*)ctx;
  *out_written             = 0U;
  if ((size_t)length > (sizeof(sink->bytes) - sink->length)) {
    return k_ra8_err_no_mem;
  }
  memcpy(&sink->bytes[sink->length], bytes, length);
  sink->length += length;
  *out_written = length;
  return k_ra8_ok;
}

/**
 * @brief Rebind the model to the deterministic source and open its link.
 * @details Hashes the fixture body with production SHA-256, resets the model,
 *          and installs the exact body and digest as the next C6 response.
 * @pre Shared model state is not in use by another test.
 * @pre The source and digest storage outlive the exchange.
 * @post The test link is ready and the source offset is zero.
 * @post The model advertises the digest of exactly ::s_body without its NUL.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prepare_source(void)
{
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(s_body, sizeof(s_body) - 1U, digest));
  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6_model_mdl_source(s_body, (uint32_t)(sizeof(s_body) - 1U), digest));
}

/**
 * @brief Bind the portable network interface to the shared model link.
 * @details Resets the fixture SHA state and injects the model link, digest
 *          callbacks, chunk geometry, and hard pull bound into the backend.
 * @param[out] net Interface under test.
 * @param[out] backend Caller-owned C6 adapter state.
 * @pre ::internal_prepare_source completed.
 * @pre Static SHA state is exclusively owned.
 * @post Both outputs are initialized and ready.
 * @post The backend owns no storage outside the supplied fixture objects.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(mdl_net_iface_t* net, mdl_net_c6link_t* backend)
{
  s_sha                             = (internal_c6_sha_t){};
  const ra8_mdl_sha256_iface_t hash = {
    .init   = internal_sha_init,
    .update = internal_sha_update,
    .final  = internal_sha_final,
    .ctx    = &s_sha,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_net_c6link_init(net,
                                     backend,
                                     priv_c6link_test_link(),
                                     &hash,
                                     (uint16_t)k_internal_chunk_bytes,
                                     (uint32_t)k_internal_max_chunks));
}

/**
 * @test internal_test_buffer_and_sink
 * @brief Both network output shapes receive exact digest-verified bytes.
 * @details Runs the same deterministic C6 body through bounded string output
 *          and the reset/write streaming sink, comparing every visible byte.
 * @pre Model and static hash state are exclusively owned.
 * @pre Fixed buffers are large enough for the deterministic source.
 * @post Buffer output is NUL-terminated and sink output is byte-identical.
 * @post The network adapter is destroyed after both successful transfers.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_buffer_and_sink(void)
{
  TEST_BEGIN("c6 media net outputs");
  internal_prepare_source();
  mdl_net_iface_t  net;
  mdl_net_c6link_t backend;
  internal_bind(&net, &backend);
  const mdl_net_req_t request = {
    .user_agent        = "ra8-media-test/3",
    .referer           = "https://example.test/catalog",
    .if_none_match     = "\"prior-etag\"",
    .if_modified_since = "Tue, 20 Oct 2015 07:28:00 GMT",
    .timeout_ms        = 6789U,
  };
  mdl_net_resp_t response;
  char           buffer[k_internal_sink_bytes];
  size_t         length = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 buffer,
                                 sizeof(buffer),
                                 &length,
                                 &response));
  TEST_ASSERT_EQ(sizeof(s_body) - 1U, length);
  TEST_ASSERT_EQ(200L, response.status);
  TEST_ASSERT(strcmp(response.retry_after, "5") == 0);
  TEST_ASSERT(strcmp(response.etag, "\"c6-model-etag\"") == 0);
  TEST_ASSERT(strcmp(response.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
  TEST_ASSERT(strcmp(response.content_type, "application/octet-stream") == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_user_agent, request.user_agent) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_referer, request.referer) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_if_none_match, request.if_none_match) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_if_modified_since, request.if_modified_since) == 0);
  TEST_ASSERT_EQ(6789, ra8_c6_model()->mdl_timeout_ms);
  TEST_ASSERT(memcmp(buffer, s_body, length) == 0);
  TEST_ASSERT_EQ('\0', buffer[length]);

  internal_prepare_source();
  internal_c6_sink_t  sink_state = {};
  mdl_net_body_sink_t sink       = {
    .reset = internal_sink_reset,
    .write = internal_sink_write,
    .ctx   = &sink_state,
  };
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_net_get_body(&net, "https://example.test/book", &request, &sink, &length, &response));
  TEST_ASSERT_EQ(sizeof(s_body) - 1U, sink_state.length);
  TEST_ASSERT(memcmp(sink_state.bytes, s_body, sink_state.length) == 0);
  mdl_net_destroy(&net);
  TEST_END("c6 media net outputs");
}

/**
 * @test internal_test_http_status_is_not_synthesized
 * @brief Surface and classify a non-success HTTP response without relabeling.
 * @details Configures the C6 model to return 429 with Retry-After and proves
 * the network adapter publishes that exact metadata and shared throttle result.
 * @pre Model and static hash state are exclusively owned.
 * @pre The deterministic source digest matches the model body.
 * @post The adapter returns busy with status 429 and Retry-After intact.
 * @post Success-only output length remains untouched on the HTTP error.
 * @note Classification is required to match the curl backend contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_http_status_is_not_synthesized(void)
{
  TEST_BEGIN("c6 media net preserves HTTP status");
  internal_prepare_source();
  ra8_c6_model()->mdl_http_status = 429;
  mdl_net_iface_t  net;
  mdl_net_c6link_t backend;
  internal_bind(&net, &backend);
  const mdl_net_req_t request = {};
  mdl_net_resp_t      response;
  char                buffer[k_internal_sink_bytes];
  size_t              length = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 buffer,
                                 sizeof(buffer),
                                 &length,
                                 &response));
  TEST_ASSERT_EQ(429L, response.status);
  TEST_ASSERT(strcmp(response.retry_after, "5") == 0);
  TEST_ASSERT_EQ(0, length);

  mdl_net_destroy(&net);
  internal_prepare_source();
  internal_bind(&net, &backend);
  ra8_c6_model()->mdl_http_status = 404;
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 buffer,
                                 sizeof(buffer),
                                 &length,
                                 &response));
  TEST_ASSERT_EQ(404L, response.status);

  mdl_net_destroy(&net);
  internal_prepare_source();
  internal_bind(&net, &backend);
  ra8_c6_model()->mdl_http_status = 500;
  TEST_ASSERT_EQ(k_ra8_fail,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 buffer,
                                 sizeof(buffer),
                                 &length,
                                 &response));
  TEST_ASSERT_EQ(500L, response.status);
  mdl_net_destroy(&net);
  TEST_END("c6 media net preserves HTTP status");
}

/**
 * @test internal_test_fail_closed
 * @brief Malformed policy, capacity, and digest faults publish no bytes.
 * @details Injects a header line break, insufficient destination, and
 *          terminal digest corruption to prove each boundary fails closed.
 * @pre Model and static hash state are exclusively owned.
 * @pre The small buffer intentionally cannot hold the source and NUL.
 * @post Every failure leaves the caller buffer empty.
 * @post The adapter is destroyed after all three rejection paths are checked.
 * @note Assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fail_closed(void)
{
  TEST_BEGIN("c6 media net fail closed");
  internal_prepare_source();
  mdl_net_iface_t  net;
  mdl_net_c6link_t backend;
  internal_bind(&net, &backend);
  char                buffer[4] = "old";
  const mdl_net_req_t malformed = {.user_agent = "ra8-test\r\nInjected: value"};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &malformed,
                                 buffer,
                                 sizeof(buffer),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT_EQ('\0', buffer[0]);

  const mdl_net_req_t request = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 buffer,
                                 sizeof(buffer),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT_EQ('\0', buffer[0]);

  mdl_net_destroy(&net);
  internal_prepare_source();
  internal_bind(&net, &backend);
  s_sha.bad_final                  = true;
  char full[k_internal_sink_bytes] = "old";
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 mdl_net_get_buf(&net,
                                 "https://example.test/book",
                                 &request,
                                 full,
                                 sizeof(full),
                                 nullptr,
                                 nullptr));
  TEST_ASSERT_EQ('\0', full[0]);
  mdl_net_destroy(&net);
  TEST_END("c6 media net fail closed");
}

int main(void)
{
  internal_test_buffer_and_sink();
  internal_test_http_status_is_not_synthesized();
  internal_test_fail_closed();
  return 0;
}
