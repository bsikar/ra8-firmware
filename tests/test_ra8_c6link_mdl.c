/**
 * @file test_ra8_c6link_mdl.c
 * @brief Generated-code and service-state tests for media download RPC.
 * @details Exercises bounded generated-code dispatch, job correlation,
 * transactional response sizing, cancellation, and malformed input rejection.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

/** @brief Fixed capacities and sentinels owned by this service fixture. */
typedef enum : uint16_t {
  k_t_mdl_request_bytes  = 700U,  /**< Packed request scratch capacity.  */
  k_t_mdl_response_bytes = 1200U, /**< Packed response scratch capacity. */
  k_t_mdl_len_sentinel   = 99U,   /**< Non-zero output-length sentinel.  */
  k_t_mdl_digest_fill    = 0xA5U, /**< Deterministic digest test octet.  */
} t_mdl_const_t;

/** @brief State for the deterministic media backend. */
typedef struct {
  const uint8_t* bytes;               /**< Modelled response body.       */
  size_t         len;                 /**< Total response body length.   */
  size_t         at;                  /**< Offset of the next body byte. */
  uint32_t       begins;              /**< Successful begin call count.  */
  uint32_t       cancels;             /**< Successful cancel call count. */
  bool           terminal_total_zero; /**< Corrupt terminal-total fault. */
} fake_backend_t;

static fake_backend_t    s_backend;
static ra8_mdl_service_t s_service;
static uint8_t           s_request[k_t_mdl_request_bytes];
static uint8_t           s_response[k_t_mdl_response_bytes];

/** @brief Legacy PR field 3 carrying the removed `format = "rabook"` value. */
static const uint8_t s_legacy_format_field[] = {0x1AU, 0x06U, 'r', 'a', 'b', 'o', 'o', 'k'};

/** @brief Unknown field 15 carrying the canonical varint value one. */
static const uint8_t s_unknown_varint_field[] = {0x78U, 0x01U};

/** @brief Provide the file-local fake begin test helper. @details Implements
 * the fake begin fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] url Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fake_begin(void* ctx, const char* url)
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  if (strcmp(url, "https://example.test/book") != 0) {
    return k_ra8_err_invalid_arg;
  }
  fake->at = 0U;
  fake->begins += 1U;
  return k_ra8_ok;
}

/** @brief Provide the file-local fake read test helper. @details Implements the
 * fake read fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[out] out Fixture argument governed by the exercised
 * interface contract. @param[in] cap Fixture argument governed by the exercised
 * interface contract. @param[out] got Fixture argument governed by the
 * exercised interface contract. @param[out] total_bytes Fixture argument
 * governed by the exercised interface contract. @param[out] complete Fixture
 * argument governed by the exercised interface contract. @param[in] sha256
 * Fixture argument governed by the exercised interface contract. @return RA8
 * status from the exercised fixture operation. @retval k_ra8_ok The fixture
 * operation completed successfully. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fake_read(void*     ctx,
                                                 uint8_t*  out,
                                                 uint16_t  cap,
                                                 uint16_t* got,
                                                 uint64_t* total_bytes,
                                                 bool*     complete,
                                                 uint8_t   sha256[k_ra8_mdl_sha256_bytes])
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  const size_t    left = fake->len - fake->at;
  const size_t    take = (left < cap) ? left : cap;
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
  }
  return k_ra8_ok;
}

/** @brief Provide the file-local fake cancel test helper. @details Implements
 * the fake cancel fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fake_cancel(void* ctx)
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  fake->cancels += 1U;
  return k_ra8_ok;
}

/** @brief Prepare the fixture's reset service state. @details Implements the
 * reset service fixture operation used only by this focused test executable.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_reset_service(void)
{
  static const uint8_t bytes[]            = {'a', 'b', 'c', 'd', 'e', 'f'};
  s_backend                               = (fake_backend_t){.bytes = bytes, .len = sizeof(bytes)};
  const ra8_mdl_service_backend_t backend = {.begin  = internal_fake_begin,
                                             .read   = internal_fake_read,
                                             .cancel = internal_fake_cancel,
                                             .ctx    = &s_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_service, &backend));
}

/** @brief Provide the file-local dispatch start test helper. @details
 * Implements the dispatch start fixture operation used only by this focused
 * test executable. @return The value computed by the fixture helper. @retval
 * value The computed fixture value for the supplied inputs. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL static uint32_t internal_dispatch_start(void)
{
  Ra8__Mdl__StartRequest req = RA8__MDL__START_REQUEST__INIT;
  req.protocol_version       = k_ra8_mdl_protocol_version;
  req.url                    = (char*)"https://example.test/book";
  const size_t request_len   = ra8__mdl__start_request__pack(&req, s_request);
  size_t       response_len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(accepted != nullptr);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_protocol_version, (int64_t)accepted->protocol_version);
  TEST_ASSERT(accepted->job_id != 0U);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_chunk_data_max, (int64_t)accepted->max_chunk_bytes);
  const uint32_t job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  return job;
}

RA8_INTERNAL static Ra8__Mdl__Chunk*
internal_dispatch_next(uint32_t job, uint64_t offset, uint32_t max_bytes)
{
  Ra8__Mdl__NextRequest req = RA8__MDL__NEXT_REQUEST__INIT;
  req.protocol_version      = k_ra8_mdl_protocol_version;
  req.job_id                = job;
  req.acknowledged_offset   = offset;
  req.max_bytes             = max_bytes;
  const size_t request_len  = ra8__mdl__next_request__pack(&req, s_request);
  size_t       response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  return ra8__mdl__chunk__unpack(nullptr, response_len, s_response);
}

/**
 * @par MC/DC:
 * These are the successful control vectors for the service guards. For the
 * request guard in `mdl_dispatch_next`, protocol mismatch, inactive service,
 * job mismatch, offset mismatch, zero max_bytes, and oversize max_bytes are
 * all false on each pull. For the backend-coherence decision, the two data
 * pulls execute got<=max, !complete with got>0, bounded offset/total, and a
 * nonterminal sequence; the terminal pull executes complete with got==0 and
 * end_offset==total. Thus every disjunct remains false and the response is
 * accepted. These controls pair with the stale-job and incoherent-total
 * single-fault vectors below; the three pulls also execute false/false/true
 * outcomes of the simple complete state selector and verify the digest is
 * published only on the terminal response.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_next
 * @brief Verify service multichunk and digest behavior. @details Executes the
 * service multichunk and digest scenario with bounded fixture state and asserts
 * the contract-specific result. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_service_multichunk_and_digest(void)
{
  TEST_BEGIN("mdl protobuf service multi-chunk");
  internal_reset_service();
  const uint32_t job = internal_dispatch_start();
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.begins);

  Ra8__Mdl__Chunk* first = internal_dispatch_next(job, 0U, 4U);
  TEST_ASSERT(first != nullptr);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->sequence);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->offset);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)first->data.len);
  TEST_ASSERT(memcmp(first->data.data, "abcd", 4U) == 0);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_DOWNLOADING, (int64_t)first->state);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->sha256.len);
  ra8__mdl__chunk__free_unpacked(first, nullptr);

  Ra8__Mdl__Chunk* second = internal_dispatch_next(job, 4U, 4U);
  TEST_ASSERT(second != nullptr);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)second->sequence);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)second->offset);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)second->data.len);
  TEST_ASSERT(memcmp(second->data.data, "ef", 2U) == 0);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_DOWNLOADING, (int64_t)second->state);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)second->sha256.len);
  ra8__mdl__chunk__free_unpacked(second, nullptr);

  Ra8__Mdl__Chunk* terminal = internal_dispatch_next(job, 6U, 4U);
  TEST_ASSERT(terminal != nullptr);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)terminal->sequence);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)terminal->offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)terminal->data.len);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_COMPLETE, (int64_t)terminal->state);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_sha256_bytes, (int64_t)terminal->sha256.len);
  for (size_t i = 0U; i < terminal->sha256.len; ++i) {
    TEST_ASSERT_EQ((int64_t)0xA5, (int64_t)terminal->sha256.data[i]);
  }
  ra8__mdl__chunk__free_unpacked(terminal, nullptr);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl protobuf service multi-chunk");
}

/**
 * @par MC/DC:
 * Relative to the all-false request-guard control in
 * `internal_test_service_multichunk_and_digest`, the stale NextRequest keeps
 * protocol, active state, offset, and max_bytes valid while changing only
 * `job_id != active_job_id` to true; that disjunct independently changes the
 * OR decision to rejection. The second StartRequest repeats the all-true URL
 * validity conjunction and reaches the separate active/busy decision. The
 * CancelRequest supplies the all-false control for
 * `(req == nullptr) || bad_protocol || !active || bad_job` and therefore
 * reaches the backend cancel exactly once. Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_start Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_next Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_cancel @brief
 * Verify service busy stale and cancel behavior. @details Executes the service
 * busy stale and cancel scenario with bounded fixture state and asserts the
 * contract-specific result. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_service_busy_stale_and_cancel(void)
{
  TEST_BEGIN("mdl service busy stale cancel");
  internal_reset_service();
  const uint32_t job = internal_dispatch_start();

  Ra8__Mdl__StartRequest second = RA8__MDL__START_REQUEST__INIT;
  second.protocol_version       = k_ra8_mdl_protocol_version;
  second.url                    = (char*)"https://example.test/book";
  size_t request_len            = ra8__mdl__start_request__pack(&second, s_request);
  size_t response_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));

  Ra8__Mdl__NextRequest stale = RA8__MDL__NEXT_REQUEST__INIT;
  stale.protocol_version      = k_ra8_mdl_protocol_version;
  stale.job_id                = job + 1U;
  stale.max_bytes             = 4U;
  request_len                 = ra8__mdl__next_request__pack(&stale, s_request);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));

  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  request_len                    = ra8__mdl__cancel_request__pack(&cancel, s_request);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__Cancelled* ack = ra8__mdl__cancelled__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(ack != nullptr);
  TEST_ASSERT_EQ((int64_t)job, (int64_t)ack->job_id);
  ra8__mdl__cancelled__free_unpacked(ack, nullptr);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl service busy stale cancel");
}

/** @brief Reject an undersized Start response before backend activation.
 * @details Implements the expect start capacity rejection fixture operation
 * used only by this focused test executable. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static void internal_expect_start_capacity_rejection(void)
{
  Ra8__Mdl__StartRequest start = RA8__MDL__START_REQUEST__INIT;
  start.protocol_version       = k_ra8_mdl_protocol_version;
  start.url                    = (char*)"https://example.test/book";
  const size_t request_len     = ra8__mdl__start_request__pack(&start, s_request);
  size_t       response_len    = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.begins);
  TEST_ASSERT(!s_service.active);
}

/** @brief Reject an undersized Next response without consuming backend bytes.
 * @details Implements the expect next capacity rejection fixture operation used
 * only by this focused test executable. @param[in] job Fixture argument
 * governed by the exercised interface contract. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static void internal_expect_next_capacity_rejection(uint32_t job)
{
  Ra8__Mdl__NextRequest next = RA8__MDL__NEXT_REQUEST__INIT;
  next.protocol_version      = k_ra8_mdl_protocol_version;
  next.job_id                = job;
  next.acknowledged_offset   = 0U;
  next.max_bytes             = 4U;
  const size_t request_len   = ra8__mdl__next_request__pack(&next, s_request);
  size_t       response_len  = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.at);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_sequence);
}

/** @brief Reject then accept Cancel without premature backend cancellation.
 * @details Implements the expect cancel capacity transaction fixture operation
 * used only by this focused test executable. @param[in] job Fixture argument
 * governed by the exercised interface contract. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static void internal_expect_cancel_capacity_transaction(uint32_t job)
{
  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  const size_t request_len       = ra8__mdl__cancel_request__pack(&cancel, s_request);
  size_t       response_len      = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.cancels);
  TEST_ASSERT(s_service.active);

  response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
}

/**
 * @par MC/DC:
 * The response-capacity decision `(len == 0U) || (len > response_cap)` executes
 * V1 F,T/true with a one-byte response buffer and V2 F,F/false with the normal
 * buffer. V1/V2 independently vary `len > response_cap`; `len` is the same
 * nonzero encoded response size. The pair is executed for Start, Next, and
 * Cancel. Each too-small vector precedes its adequate-capacity retry, proving
 * the backend begin/read/cancel side effect and service
 * offset/sequence/activity changes occur only on V2. Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_check_response_size
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_start
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_next
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_cancel
 * @brief Verify response capacity is transactional behavior. @details Executes
 * the response capacity is transactional scenario with bounded fixture state
 * and asserts the contract-specific result. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_response_capacity_is_transactional(void)
{
  TEST_BEGIN("mdl response capacity is transactional");
  internal_reset_service();
  internal_expect_start_capacity_rejection();

  const uint32_t job = internal_dispatch_start();
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.begins);
  internal_expect_next_capacity_rejection(job);

  Ra8__Mdl__Chunk* first = internal_dispatch_next(job, 0U, 4U);
  TEST_ASSERT(first != nullptr);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)first->data.len);
  TEST_ASSERT(memcmp(first->data.data, "abcd", 4U) == 0);
  ra8__mdl__chunk__free_unpacked(first, nullptr);
  internal_expect_cancel_capacity_transaction(job);
  TEST_END("mdl response capacity is transactional");
}

/**
 * @par MC/DC:
 * Dispatch with an unknown operation keeps every public pointer/length guard
 * valid (all OR conditions false) and reaches the default selector. The
 * non-protobuf StartRequest then reaches the independent decode-null decision.
 * For the five-condition URL-valid conjunction, the insecure request executes
 * T,T,T,F,-/false: protocol and both length bounds are valid, while only the
 * HTTPS-prefix comparison fails. The accepted control in
 * `internal_test_service_multichunk_and_digest` executes T,T,T,T,T/true, so the
 * pair independently varies the prefix condition. No rejected vector calls the
 * backend or activates the service.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@ra8_mdl_service_dispatch
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_start
 * @brief Verify rejects malformed behavior. @details Executes the rejects
 * malformed scenario with bounded fixture state and asserts the
 * contract-specific result. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_rejects_malformed(void)
{
  TEST_BEGIN("mdl rejects malformed");
  internal_reset_service();
  size_t response_len = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mdl_service_dispatch(&s_service,
                                          0xDEADBEEFU,
                                          (const uint8_t*)"x",
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          (const uint8_t*)"not protobuf",
                                          strlen("not protobuf"),
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__StartRequest insecure = RA8__MDL__START_REQUEST__INIT;
  insecure.protocol_version       = k_ra8_mdl_protocol_version;
  insecure.url                    = (char*)"http://example.test/book";
  const size_t insecure_len       = ra8__mdl__start_request__pack(&insecure, s_request);
  response_len                    = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          insecure_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.begins);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl rejects malformed");
}

/**
 * @brief Reject the removed legacy format field before backend activation.
 * @details Appends the original field-3 `format = "rabook"` wire bytes to an
 * otherwise canonical StartRequest.
 * @pre The deterministic service fixture is reset and request storage fits.
 * @pre No backend job is active.
 * @post Dispatch returns a protocol error and emits no response.
 * @post The backend remains unstarted and the service remains inactive.
 * @note Protobuf-c preserves this reserved field as an unknown field.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_legacy_format_rejection(void)
{
  Ra8__Mdl__StartRequest start = RA8__MDL__START_REQUEST__INIT;
  start.protocol_version       = k_ra8_mdl_protocol_version;
  start.url                    = (char*)"https://example.test/book";
  size_t request_len           = ra8__mdl__start_request__pack(&start, s_request);
  memcpy(&s_request[request_len], s_legacy_format_field, sizeof(s_legacy_format_field));
  request_len += sizeof(s_legacy_format_field);
  size_t response_len = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.begins);
  TEST_ASSERT(!s_service.active);
}

/**
 * @test internal_test_rejects_unknown_request_fields
 * @brief Reject unknown protobuf fields without backend or service-state effects.
 * @details Pins the removed legacy Start field and a future unknown varint on
 *          active Next and Cancel calls.
 * @pre The deterministic service fixture and request buffers are available.
 * @post Start does not begin a backend; Next does not consume bytes; Cancel does
 *       not cancel, and a subsequent canonical Cancel remains accepted.
 * @note Protobuf-c preserves unknown fields, so the application rejects them.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rejects_unknown_request_fields(void)
{
  TEST_BEGIN("mdl rejects unknown request fields");
  internal_reset_service();
  internal_expect_legacy_format_rejection();
  size_t                request_len  = 0U;
  size_t                response_len = k_t_mdl_len_sentinel;
  const uint32_t        job          = internal_dispatch_start();
  Ra8__Mdl__NextRequest next         = RA8__MDL__NEXT_REQUEST__INIT;
  next.protocol_version              = k_ra8_mdl_protocol_version;
  next.job_id                        = job;
  next.max_bytes                     = 4U;
  request_len                        = ra8__mdl__next_request__pack(&next, s_request);
  memcpy(&s_request[request_len], s_unknown_varint_field, sizeof(s_unknown_varint_field));
  request_len += sizeof(s_unknown_varint_field);
  response_len = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.at);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_sequence);

  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  request_len                    = ra8__mdl__cancel_request__pack(&cancel, s_request);
  memcpy(&s_request[request_len], s_unknown_varint_field, sizeof(s_unknown_varint_field));
  request_len += sizeof(s_unknown_varint_field);
  response_len = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.cancels);
  TEST_ASSERT(s_service.active);
  request_len = ra8__mdl__cancel_request__pack(&cancel, s_request);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl rejects unknown request fields");
}

/**
 * @test internal_test_rejects_incoherent_terminal_total
 * @brief Reject a backend that erases a known total at terminal completion.
 * @details Consumes the complete body before injecting `complete=true` with a
 *          zero total, which would otherwise produce a response the peer must
 *          reject after already transferring all bytes.
 * @pre The deterministic backend has a non-empty six-byte artifact.
 * @post Dispatch reports a protocol error, emits no response, cancels once,
 *       and deactivates the corrupt job.
 * @note Host-only fault-injection test with no network or filesystem activity.
 * @par MC/DC:
 * The prior two valid data pulls execute the backend-coherence OR with every
 * top-level fault condition false. The terminal pull keeps got<=max,
 * `(!complete && got==0)` false, `(complete && got!=0)` false, offset
 * arithmetic bounded, and sequence nonterminal, but changes `total_invalid`
 * alone to true by returning complete=true with end_offset=6 and total=0. The
 * all-false control and this single-fault vector prove `total_invalid`
 * independently decides rejection. The failure vector also proves
 * cancel/deactivation occurs before any response is published. Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_next
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation
 * is available. @post Documented outputs contain the exercised result when the
 * operation succeeds. */
RA8_INTERNAL static void internal_test_rejects_incoherent_terminal_total(void)
{
  TEST_BEGIN("mdl rejects incoherent terminal total");
  internal_reset_service();
  const uint32_t   job   = internal_dispatch_start();
  Ra8__Mdl__Chunk* first = internal_dispatch_next(job, 0U, 4U);
  TEST_ASSERT(first != nullptr);
  ra8__mdl__chunk__free_unpacked(first, nullptr);
  Ra8__Mdl__Chunk* second = internal_dispatch_next(job, 4U, 4U);
  TEST_ASSERT(second != nullptr);
  ra8__mdl__chunk__free_unpacked(second, nullptr);

  s_backend.terminal_total_zero  = true;
  Ra8__Mdl__NextRequest terminal = RA8__MDL__NEXT_REQUEST__INIT;
  terminal.protocol_version      = k_ra8_mdl_protocol_version;
  terminal.job_id                = job;
  terminal.acknowledged_offset   = 6U;
  terminal.max_bytes             = 4U;
  const size_t request_len       = ra8__mdl__next_request__pack(&terminal, s_request);
  size_t       response_len      = k_t_mdl_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl rejects incoherent terminal total");
}

int32_t main(void)
{
  internal_test_service_multichunk_and_digest();
  internal_test_service_busy_stale_and_cancel();
  internal_test_response_capacity_is_transactional();
  internal_test_rejects_malformed();
  internal_test_rejects_unknown_request_fields();
  internal_test_rejects_incoherent_terminal_total();
  return 0;
}
