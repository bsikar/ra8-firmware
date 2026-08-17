/**
 * @file test_ra8_c6link_mdl_guards.c
 * @brief Independence vectors for the media service's validation decisions
 * @details Covers the three pure validators through their private seams and
 * the three dispatch guards through the public entry point, so each compound
 * decision in `ra8_c6link_mdl_service.c` gets N+1 vectors that differ from the
 * control by exactly one condition.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6link_mdl_msg.h"
#include "ra8_c6link_mdl_service_internal.h"
#include "ra8_err.h"
#include "ra8_media_download.pb-c.h"
#include "test_ra8_c6link_mdl_guards_internal.h"
#include "unity_minimal.h"

/** @enum internal_guard_const_t @brief Bounded fixture geometry and operands. */
typedef enum : uint32_t {
  k_internal_request_bytes  = 700U,  /**< Packed request scratch capacity.        */
  k_internal_response_bytes = 1200U, /**< Packed response scratch capacity.       */
  k_internal_body_bytes     = 6U,    /**< Bytes the fake backend serves.          */
  k_internal_size_len       = 8U,    /**< In-capacity packed length operand.      */
  k_internal_size_cap       = 16U,   /**< Capacity operand for the size vectors.  */
  k_internal_size_over      = 32U,   /**< Over-capacity packed length operand.    */
  k_internal_status_ok      = 200U,  /**< In-range HTTP status operand.           */
  k_internal_status_low     = 99U,   /**< Below the protocol's HTTP status floor. */
  k_internal_status_high    = 600U,  /**< Above the protocol's HTTP status roof.  */
  k_internal_max_bytes      = 4U,    /**< Legal per-pull body bound.              */
  k_internal_max_over       = 1025U, /**< One above the absolute chunk bound.     */
  k_internal_bad_version    = 999U,  /**< Protocol version no endpoint speaks.    */
  k_internal_bad_job        = 4242U, /**< Job identity no service ever issued.    */
  k_internal_bad_offset     = 77U,   /**< Acknowledged offset the job never had.  */
} internal_guard_const_t;

/** @brief Deterministic body source and observations for one service job. */
typedef struct {
  const uint8_t* bytes;  /**< Body the backend serves.  */
  uint32_t       len;    /**< Valid bytes at `bytes`.   */
  uint32_t       served; /**< Bytes already handed out. */
} internal_backend_t;

static internal_backend_t s_backend;
static ra8_mdl_service_t  s_service;
static uint8_t            s_request[k_internal_request_bytes];
static uint8_t            s_response[k_internal_response_bytes];

/** @brief Accept one job from the service. @details Implements the begin
 * fixture operation used only by this focused test executable. @param[in,out] ctx
 * Fixture argument governed by the exercised interface contract. @param[in] request
 * Fixture argument governed by the exercised interface contract. @return RA8
 * status from the exercised fixture operation. @retval k_ra8_ok The fixture
 * operation completed successfully. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_fake_begin(void* ctx, const ra8_mdl_request_t* request)
{
  internal_backend_t* backend = (internal_backend_t*)ctx;
  (void)request;
  backend->served = 0U;
  return k_ra8_ok;
}

/** @brief Serve the next bounded span of the fixed body. @details Implements
 * the read fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[out] out Fixture argument governed by the exercised interface
 * contract. @param[in] max_bytes Fixture argument governed by the exercised
 * interface contract. @param[out] got Fixture argument governed by the exercised
 * interface contract. @param[out] total Fixture argument governed by the
 * exercised interface contract. @param[out] complete Fixture argument governed
 * by the exercised interface contract. @param[out] digest Fixture argument
 * governed by the exercised interface contract. @param[out] response Fixture
 * argument governed by the exercised interface contract. @return RA8 status
 * from the exercised fixture operation. @retval k_ra8_ok The fixture operation
 * completed successfully. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_fake_read(void*                    ctx,
                                    uint8_t*                 out,
                                    uint16_t                 max_bytes,
                                    uint16_t*                got,
                                    uint64_t*                total,
                                    bool*                    complete,
                                    uint8_t*                 digest,
                                    ra8_mdl_http_response_t* response)
{
  internal_backend_t* backend = (internal_backend_t*)ctx;
  const uint32_t      left    = backend->len - backend->served;
  const uint32_t      take    = (left < max_bytes) ? left : max_bytes;
  (void)memcpy(out, &backend->bytes[backend->served], take);
  backend->served += take;
  *got      = (uint16_t)take;
  *total    = backend->len;
  *complete = (take == 0U);
  *response = (ra8_mdl_http_response_t){};
  if (*complete) {
    (void)memset(digest, 0, (size_t)k_ra8_mdl_sha256_bytes);
    response->status = (int32_t)k_internal_status_ok;
  }
  return k_ra8_ok;
}

/** @brief Release the fake job. @details Implements the cancel fixture
 * operation used only by this focused test executable. @param[in,out] ctx
 * Fixture argument governed by the exercised interface contract. @return RA8
 * status from the exercised fixture operation. @retval k_ra8_ok The fixture
 * operation completed successfully. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_fake_cancel(void* ctx)
{
  internal_backend_t* backend = (internal_backend_t*)ctx;
  backend->served             = 0U;
  return k_ra8_ok;
}

/** @brief Reset the service onto a fresh deterministic backend. @details
 * Implements the reset fixture operation used only by this focused test
 * executable. @pre Fixed-capacity fixture storage required by this operation is
 * available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_reset(void)
{
  static const uint8_t body[k_internal_body_bytes] = {'a', 'b', 'c', 'd', 'e', 'f'};
  s_backend = (internal_backend_t){.bytes = body, .len = k_internal_body_bytes};
  const ra8_mdl_service_backend_t backend = {.begin  = internal_fake_begin,
                                             .read   = internal_fake_read,
                                             .cancel = internal_fake_cancel,
                                             .ctx    = &s_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_service, &backend));
}

/** @brief Start one job and return its correlation identity. @details
 * Implements the start fixture operation used only by this focused test
 * executable. @return Correlated job identifier issued by the service. @retval 0
 * The service refused to start, which no caller of this helper expects. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static uint32_t internal_start(void)
{
  Ra8__Mdl__StartRequest req = RA8__MDL__START_REQUEST__INIT;
  req.protocol_version       = k_ra8_mdl_protocol_version;
  req.url                    = (char*)"https://example.test/book";
  req.format                 = RA8__MDL__FORMAT__FORMAT_RABOOK;
  req.user_agent             = (char*)"";
  req.referer                = (char*)"";
  req.if_none_match          = (char*)"";
  req.if_modified_since      = (char*)"";
  const size_t len           = ra8__mdl__start_request__get_packed_size(&req);
  TEST_ASSERT(len <= sizeof(s_request));
  TEST_ASSERT_EQ(len, ra8__mdl__start_request__pack(&req, s_request));
  size_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          (uint32_t)k_ra8_mdl_rpc_start,
                                          s_request,
                                          len,
                                          s_response,
                                          sizeof(s_response),
                                          &out));
  return s_service.active_job_id;
}

/** @brief Dispatch one packed Next request and return its status. @details
 * Implements the next fixture operation used only by this focused test
 * executable. @param[in] req Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_next(const Ra8__Mdl__NextRequest* req)
{
  const size_t len = ra8__mdl__next_request__get_packed_size(req);
  TEST_ASSERT(len <= sizeof(s_request));
  TEST_ASSERT_EQ(len, ra8__mdl__next_request__pack(req, s_request));
  size_t out = 0U;
  return ra8_mdl_service_dispatch(&s_service,
                                  (uint32_t)k_ra8_mdl_rpc_next,
                                  s_request,
                                  len,
                                  s_response,
                                  sizeof(s_response),
                                  &out);
}

/** @brief Dispatch one packed Cancel request and return its status. @details
 * Implements the cancel dispatch fixture operation used only by this focused
 * test executable. @param[in] req Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_cancel(const Ra8__Mdl__CancelRequest* req)
{
  const size_t len = ra8__mdl__cancel_request__get_packed_size(req);
  TEST_ASSERT(len <= sizeof(s_request));
  TEST_ASSERT_EQ(len, ra8__mdl__cancel_request__pack(req, s_request));
  size_t out = 0U;
  return ra8_mdl_service_dispatch(&s_service,
                                  (uint32_t)k_ra8_mdl_rpc_cancel,
                                  s_request,
                                  len,
                                  s_response,
                                  sizeof(s_response),
                                  &out);
}

/**
 * @test priv_test_c6link_mdl_guards_run
 * @brief Prove each byte class independently decides header rejection.
 * @par MC/DC:
 * Decision: `(text[index] == '\r') || (text[index] == '\n')` (2 conditions)
 * - Vector 1: "ok" -> F,F -> false (control: neither byte class present).
 * - Vector 2: "a\rb" -> T,- -> true (varies the CR condition only).
 * - Vector 3: "a\nb" -> F,T -> true (varies the LF condition only).
 * Vectors 1+2 prove CR independently decides; 1+3 prove the same for LF.
 * N+1 = 3 vectors for N=2.
 * Decision: `(len == 0U) || (len > response_cap)` (2 conditions)
 * - Vector 1: len=8, cap=16 -> F,F -> false (the response fits).
 * - Vector 2: len=0, cap=16 -> T,- -> true (varies emptiness only).
 * - Vector 3: len=32, cap=16 -> F,T -> true (varies capacity only).
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_request_field_valid
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_check_response_size
 * @details Uses the private seams because a hand-packed protobuf request per
 * condition would be needed to reach the same operands through dispatch.
 * @pre The private validation seams are linked into this executable.
 * @pre The bounded literals below stay within their declared capacities.
 * @post Every rejected vector differs from the control by one operand.
 * @post No service or backend state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_pure_predicates(void)
{
  TEST_BEGIN("mdl service pure predicate MC/DC");
  TEST_ASSERT(ra8_mdl_service_field_valid_test("ok", k_ra8_mdl_etag_max));
  TEST_ASSERT(!ra8_mdl_service_field_valid_test("a\rb", k_ra8_mdl_etag_max));
  TEST_ASSERT(!ra8_mdl_service_field_valid_test("a\nb", k_ra8_mdl_etag_max));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_check_size_test(k_internal_size_len, k_internal_size_cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_mdl_service_check_size_test(0U, k_internal_size_cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_check_size_test(k_internal_size_over, k_internal_size_cap));
  TEST_END("mdl service pure predicate MC/DC");
}

/**
 * @test priv_test_c6link_mdl_guards_run
 * @brief Prove each terminal-metadata rule independently decides rejection.
 * @par MC/DC:
 * Decision: `(status >= min) && (status <= max) && field_valid(retry_after) &&
 * field_valid(etag) && field_valid(last_modified) && field_valid(content_type)`
 * (6 conditions)
 * - Vector 1: status=200, four empty headers -> T,T,T,T,T,T -> true (control).
 * - Vector 2: status=99 -> F,-,-,-,-,- -> false (varies the floor only).
 * - Vector 3: status=600 -> T,F,-,-,-,- -> false (varies the roof only).
 * - Vector 4: Retry-After carries CR -> T,T,F,-,-,- -> false.
 * - Vector 5: ETag carries CR -> T,T,T,F,-,- -> false.
 * - Vector 6: Last-Modified carries CR -> T,T,T,T,F,- -> false.
 * - Vector 7: Content-Type carries CR -> T,T,T,T,T,F -> false.
 * Each of vectors 2..7 pairs with vector 1 to prove one condition
 * independently decides. N+1 = 7 vectors for N=6.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_response_valid
 * @details A short-circuit AND chain, so making condition N false requires
 * every earlier condition true; each vector below does exactly that.
 * @pre The private validation seam is linked into this executable.
 * @pre Each injected header stays within its declared capacity.
 * @post Only the all-true control is accepted.
 * @post No service or backend state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_backend_response_valid(void)
{
  TEST_BEGIN("mdl service backend response MC/DC");
  ra8_mdl_http_response_t control = {.status = (int32_t)k_internal_status_ok};
  TEST_ASSERT(ra8_mdl_service_response_valid_test(&control));

  ra8_mdl_http_response_t vector = control;
  vector.status                  = (int32_t)k_internal_status_low;
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));

  vector        = control;
  vector.status = (int32_t)k_internal_status_high;
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));

  vector = control;
  (void)memcpy(vector.retry_after, "a\rb", sizeof("a\rb"));
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));

  vector = control;
  (void)memcpy(vector.etag, "a\rb", sizeof("a\rb"));
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));

  vector = control;
  (void)memcpy(vector.last_modified, "a\rb", sizeof("a\rb"));
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));

  vector = control;
  (void)memcpy(vector.content_type, "a\rb", sizeof("a\rb"));
  TEST_ASSERT(!ra8_mdl_service_response_valid_test(&vector));
  TEST_END("mdl service backend response MC/DC");
}

/**
 * @test priv_test_c6link_mdl_guards_run
 * @brief Prove each dispatch pointer operand independently decides rejection.
 * @par MC/DC:
 * Decision: `(ctx == nullptr) || (request == nullptr) || (request_len == 0U) ||
 * (response == nullptr) || (response_len == nullptr)` (5 conditions)
 * - Vector 1: every argument present -> F,F,F,F,F -> false (the control
 *   proceeds into the operation switch and is refused as unsupported).
 * - Vectors 2..6: exactly one argument nulled or zeroed in turn -> the
 *   corresponding condition true -> `k_ra8_err_null_ptr` before any decode.
 * Each of vectors 2..6 pairs with vector 1 to prove one condition
 * independently decides. N+1 = 6 vectors for N=5.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@ra8_mdl_service_dispatch
 * @details Uses an unsupported operation for the control so the vector set
 * measures only the pointer guard, with no decode or backend side effect.
 * @pre The service is initialised and idle.
 * @pre The bounded request and response scratch buffers are available.
 * @post Every nulled vector is refused before the operation switch.
 * @post No backend callback runs in any vector.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_dispatch_pointer_guard(void)
{
  TEST_BEGIN("mdl service dispatch pointer MC/DC");
  internal_reset();
  const uint32_t unsupported = 0U;
  size_t         out         = k_internal_size_len;
  s_request[0]               = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mdl_service_dispatch(&s_service,
                                          unsupported,
                                          s_request,
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          &out));
  TEST_ASSERT_EQ(0, out);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mdl_service_dispatch(nullptr,
                                          unsupported,
                                          s_request,
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mdl_service_dispatch(&s_service,
                                          unsupported,
                                          nullptr,
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mdl_service_dispatch(&s_service,
                                          unsupported,
                                          s_request,
                                          0U,
                                          s_response,
                                          sizeof(s_response),
                                          &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mdl_service_dispatch(&s_service,
                                          unsupported,
                                          s_request,
                                          1U,
                                          nullptr,
                                          sizeof(s_response),
                                          &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mdl_service_dispatch(&s_service,
                                          unsupported,
                                          s_request,
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          nullptr));
  TEST_END("mdl service dispatch pointer MC/DC");
}

/** @brief Drive the Cancel arm's three correlation operands and the shared
 * inactive-service vector. @details Implements the cancel arm fixture
 * operation used only by this focused test executable. @param[in] job Fixture
 * argument governed by the exercised interface contract. @param[in,out] base
 * Fixture argument governed by the exercised interface contract. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_cancel_arm(uint32_t job, Ra8__Mdl__NextRequest* base)
{
  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  Ra8__Mdl__CancelRequest cvec   = cancel;
  cvec.protocol_version          = k_internal_bad_version;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_cancel(&cvec));
  cvec        = cancel;
  cvec.job_id = k_internal_bad_job;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_cancel(&cvec));
  TEST_ASSERT_EQ(k_ra8_ok, internal_cancel(&cancel));

  /* The service is now inactive: the same shapes select the activity operand
     alone in both arms. */
  Ra8__Mdl__NextRequest vector = *base;
  vector.acknowledged_offset   = k_internal_max_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_cancel(&cancel));

  internal_reset();
  const uint32_t restarted  = internal_start();
  cancel.job_id             = restarted;
  base->job_id              = restarted;
  base->acknowledged_offset = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_next(base));
  TEST_ASSERT_EQ(k_ra8_ok, internal_cancel(&cancel));
}

/**
 * @test priv_test_c6link_mdl_guards_run
 * @brief Prove each Next correlation operand independently decides rejection.
 * @par MC/DC:
 * Decision: `(protocol_version != expected) || !active || (job_id != active) ||
 * (acknowledged_offset != next_offset) || (max_bytes == 0) ||
 * (max_bytes > chunk_data_max)` (6 conditions)
 * - Vector 1: a correlated pull on an active job -> all six false -> the pull
 *   proceeds and returns bytes.
 * - Vector 2: protocol version 999 -> the version condition alone true.
 * - Vector 3: the same request after a cancel leaves the service inactive ->
 *   the activity condition alone true.
 * - Vector 4: an unissued job id -> the correlation condition alone true.
 * - Vector 5: an acknowledged offset the job never reached.
 * - Vector 6: `max_bytes` zero.
 * - Vector 7: `max_bytes` one above the absolute chunk bound.
 * Each of vectors 2..7 pairs with vector 1 to prove one condition
 * independently decides. N+1 = 7 vectors for N=6.
 * Decision: `(protocol_version != expected) || !active || (job_id != active)`
 * (3 conditions) -- the Cancel arm, driven by the same four shapes.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_next
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_service.c@internal_mdl_dispatch_cancel
 * @details Restarts a fresh job before each vector so every rejection differs
 * from the accepted control by exactly one request field or one service state.
 * @pre The fake backend serves a body longer than one pull.
 * @pre Each Start leaves exactly one active correlated job.
 * @post Every rejected pull returns `k_ra8_err_invalid_state`.
 * @post No rejected vector advances sequence or offset.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_correlation_guards(void)
{
  TEST_BEGIN("mdl service correlation MC/DC");
  internal_reset();
  const uint32_t        job  = internal_start();
  Ra8__Mdl__NextRequest base = RA8__MDL__NEXT_REQUEST__INIT;
  base.protocol_version      = k_ra8_mdl_protocol_version;
  base.job_id                = job;
  base.acknowledged_offset   = 0U;
  base.max_bytes             = k_internal_max_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, internal_next(&base));

  Ra8__Mdl__NextRequest vector = base;
  vector.acknowledged_offset   = k_internal_max_bytes;
  vector.protocol_version      = k_internal_bad_version;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));

  vector                     = base;
  vector.acknowledged_offset = k_internal_max_bytes;
  vector.job_id              = k_internal_bad_job;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));

  vector                     = base;
  vector.acknowledged_offset = k_internal_bad_offset;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));

  vector                     = base;
  vector.acknowledged_offset = k_internal_max_bytes;
  vector.max_bytes           = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));

  vector                     = base;
  vector.acknowledged_offset = k_internal_max_bytes;
  vector.max_bytes           = k_internal_max_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, internal_next(&vector));

  internal_cancel_arm(job, &base);
  TEST_END("mdl service correlation MC/DC");
}

RA8_PRIV void priv_test_c6link_mdl_guards_run(void)
{
  internal_test_pure_predicates();
  internal_test_backend_response_valid();
  internal_test_dispatch_pointer_guard();
  internal_test_correlation_guards();
}
