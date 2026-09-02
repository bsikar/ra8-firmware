/**
 * @file test_ra8_c6link_mdl_decode.c
 * @brief Independence vectors for the media client's response validators
 * @details Covers the decoded-response predicates in `ra8_c6link_mdl.c`
 * through their private seams and the two Start argument guards through the
 * public API, so each compound decision gets N+1 vectors that differ from the
 * control by exactly one condition.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6_model.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_internal.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "ra8_media_download.pb-c.h"
#include "test_ra8_c6link_mdl_decode_internal.h"
#include "unity_minimal.h"

/** @enum internal_decode_const_t @brief Bounded operands used by the vectors. */
typedef enum : uint32_t {
  k_internal_status_ok    = 200U,   /**< In-range HTTP status operand.             */
  k_internal_status_low   = 99U,    /**< Below the protocol's HTTP status floor.   */
  k_internal_status_high  = 600U,   /**< Above the protocol's HTTP status roof.    */
  k_internal_body_len     = 10U,    /**< Body length used by the size vectors.     */
  k_internal_total_over   = 100U,   /**< Declared total larger than the body end.  */
  k_internal_total_short  = 5U,     /**< Declared total shorter than the body end. */
  k_internal_fail_status  = 7U,     /**< Nonzero canonical failure status.         */
  k_internal_over_uint16  = 65536U, /**< One above the FAILED status ceiling.      */
  k_internal_timeout_over = 60001U, /**< One above the caller timeout ceiling.     */
  k_internal_packed_bytes = 64U,    /**< Packed acknowledgement scratch capacity.  */
  k_internal_ack_job      = 1U,     /**< Job identity the acknowledgement echoes.  */
  k_internal_bad_version  = 999U,   /**< Protocol version no endpoint speaks.      */
  k_internal_bad_job      = 4242U,  /**< Job identity no service ever issued.      */
} internal_decode_const_t;

static uint8_t s_body[k_internal_body_len];
static uint8_t s_packed[k_internal_packed_bytes];
static uint8_t s_digest[k_ra8_mdl_sha256_bytes];

/** @brief Build a decoded chunk whose non-state fields are all acceptable.
 * @details Implements the base chunk fixture operation used only by this
 * focused test executable. @param[out] msg Fixture argument governed by the
 * exercised interface contract. @param[in] state Fixture argument governed by
 * the exercised interface contract. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_base_chunk(Ra8__Mdl__Chunk* msg, Ra8__Mdl__State state)
{
  *msg                  = (Ra8__Mdl__Chunk)RA8__MDL__CHUNK__INIT;
  msg->protocol_version = k_ra8_mdl_protocol_version;
  msg->job_id           = 1U;
  msg->state            = state;
  msg->retry_after      = (char*)"";
  msg->etag             = (char*)"";
  msg->last_modified    = (char*)"";
  msg->content_type     = (char*)"";
  if (state == RA8__MDL__STATE__STATE_COMPLETE) {
    msg->http_status = (int32_t)k_internal_status_ok;
    msg->sha256      = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = s_digest};
  } else if (state == RA8__MDL__STATE__STATE_DOWNLOADING) {
    msg->data = (ProtobufCBinaryData){.len = k_internal_body_len, .data = s_body};
  } else if (state == RA8__MDL__STATE__STATE_FAILED) {
    msg->status = (int32_t)k_internal_fail_status;
  } else {
    /* CANCELLED carries neither data, digest, nor status. */
  }
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each byte class independently decides header rejection.
 * @par MC/DC:
 * Decision: `(text[index] == '\r') || (text[index] == '\n')` (2 conditions)
 * - Vector 1: "ok" -> F,F -> false (control: the header is accepted).
 * - Vector 2: "a\rb" -> T,- -> true (varies the CR condition only).
 * - Vector 3: "a\nb" -> F,T -> true (varies the LF condition only).
 * Vectors 1+2 prove CR independently decides; 1+3 prove the same for LF.
 * N+1 = 3 vectors for N=2.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_http_field_valid
 * @details Uses the private seam: reaching one byte class at a time through
 * the modelled transport needs a distinct hand-packed terminal response each.
 * @pre The private validation seams are linked into this executable.
 * @pre The literals below stay inside the declared header capacity.
 * @post Only the clean header is accepted.
 * @post No link, session, or model state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_header_bytes(void)
{
  TEST_BEGIN("mdl client header byte MC/DC");
  TEST_ASSERT(ra8_c6link_mdl_http_field_valid_test("ok", k_ra8_mdl_etag_max));
  TEST_ASSERT(!ra8_c6link_mdl_http_field_valid_test("a\rb", k_ra8_mdl_etag_max));
  TEST_ASSERT(!ra8_c6link_mdl_http_field_valid_test("a\nb", k_ra8_mdl_etag_max));
  TEST_END("mdl client header byte MC/DC");
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each metadata rule independently decides response rejection.
 * @par MC/DC:
 * Decision: `(msg->http_status == 0) && empty` (2 conditions, non-terminal arm)
 * - Vector 1: DOWNLOADING, status 0, four empty headers -> T,T -> true.
 * - Vector 2: DOWNLOADING, status 200 -> F,- -> false (varies status only).
 * - Vector 3: DOWNLOADING, status 0, non-empty ETag -> T,F -> false.
 * Decision: `(status >= min) && (status <= max) && field_valid(retry_after) &&
 * field_valid(etag) && field_valid(last_modified) && field_valid(content_type)`
 * (6 conditions, COMPLETE arm)
 * - Vector 1: status 200 and four empty headers -> all true -> true.
 * - Vectors 2..7: status 99, status 600, then one CR-bearing header at a time,
 *   each leaving every earlier condition true -> false.
 * Each rejected vector pairs with its control to prove one condition
 * independently decides. N+1 = 3 and 7 vectors for N=2 and N=6.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_http_response_valid
 * @details A non-terminal response must carry no HTTP metadata at all, which
 * the C6 service is structurally unable to emit; only this seam can present it.
 * @pre The private validation seam is linked into this executable.
 * @pre Every injected header stays within its declared capacity.
 * @post Only the two controls are accepted.
 * @post No link, session, or model state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_response_metadata(void)
{
  TEST_BEGIN("mdl client response metadata MC/DC");
  Ra8__Mdl__Chunk control = {};
  internal_base_chunk(&control, RA8__MDL__STATE__STATE_DOWNLOADING);
  TEST_ASSERT(ra8_c6link_mdl_http_response_valid_test(&control));

  Ra8__Mdl__Chunk vector = control;
  vector.http_status     = (int32_t)k_internal_status_ok;
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector      = control;
  vector.etag = (char*)"W/x";
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  internal_base_chunk(&control, RA8__MDL__STATE__STATE_COMPLETE);
  TEST_ASSERT(ra8_c6link_mdl_http_response_valid_test(&control));

  vector             = control;
  vector.http_status = (int32_t)k_internal_status_low;
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector             = control;
  vector.http_status = (int32_t)k_internal_status_high;
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector             = control;
  vector.retry_after = (char*)"a\rb";
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector      = control;
  vector.etag = (char*)"a\rb";
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector               = control;
  vector.last_modified = (char*)"a\rb";
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));

  vector              = control;
  vector.content_type = (char*)"a\rb";
  TEST_ASSERT(!ra8_c6link_mdl_http_response_valid_test(&vector));
  TEST_END("mdl client response metadata MC/DC");
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each length relationship independently decides coverage.
 * @par MC/DC:
 * Decision: `(msg->total_bytes == 0U) || (end <= msg->total_bytes)`
 * (2 conditions)
 * - Vector 1: total 0 -> T,- -> true (an unspecified total covers anything).
 * - Vector 2: total 100 with end 10 -> F,T -> true (varies the bound only).
 * - Vector 3: total 5 with end 10 -> F,F -> false (the body overruns it).
 * Vectors 1+3 prove the unspecified-total condition independently decides;
 * 2+3 prove the same for the bound. N+1 = 3 vectors for N=2.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_chunk_semantics_valid
 * @details Holds state, status, and digest constant so only the declared total
 * changes between vectors.
 * @pre The private validation seam is linked into this executable.
 * @pre The body length operand is smaller than the over-total operand.
 * @post Only the overrunning vector is rejected.
 * @post No link, session, or model state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_total_covers_data(void)
{
  TEST_BEGIN("mdl client total-covers-data MC/DC");
  Ra8__Mdl__Chunk msg = {};
  internal_base_chunk(&msg, RA8__MDL__STATE__STATE_DOWNLOADING);
  msg.total_bytes = 0U;
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&msg));
  msg.total_bytes = k_internal_total_over;
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&msg));
  msg.total_bytes = k_internal_total_short;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&msg));
  TEST_END("mdl client total-covers-data MC/DC");
}

/** @brief Drive the CANCELLED and FAILED state arms. @details Implements the
 * terminal state semantics fixture operation used only by this focused test
 * executable. @pre Fixed-capacity fixture storage required by this operation is
 * available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_terminal_state_semantics(void)
{
  Ra8__Mdl__Chunk control = {};
  Ra8__Mdl__Chunk vector  = {};
  internal_base_chunk(&control, RA8__MDL__STATE__STATE_CANCELLED);
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&control));
  vector        = control;
  vector.status = (int32_t)k_internal_fail_status;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector      = control;
  vector.data = (ProtobufCBinaryData){.len = k_internal_body_len, .data = s_body};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector        = control;
  vector.sha256 = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = s_digest};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));

  internal_base_chunk(&control, RA8__MDL__STATE__STATE_FAILED);
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&control));
  vector        = control;
  vector.status = 0;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector        = control;
  vector.status = (int32_t)k_internal_over_uint16;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector      = control;
  vector.data = (ProtobufCBinaryData){.len = k_internal_body_len, .data = s_body};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector        = control;
  vector.sha256 = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = s_digest};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each state's field rule independently decides rejection.
 * @par MC/DC:
 * Decision: DOWNLOADING `(status == 0) && (data.len != 0) && (sha256.len == 0)`
 * (3 conditions) -- all-true control, then a nonzero status, an empty body, and
 * an attached digest in turn -> 4 vectors.
 * Decision: COMPLETE `(status == 0) && (data.len == 0) && (sha256.len == 32) &&
 * (sha256.data != nullptr) && ((total == 0) || (total == end))` (6 conditions,
 * the trailing disjunction counting as two) -- all-true control, then a nonzero
 * status, attached body bytes, a short digest, a null digest pointer, and a
 * total that closes at the wrong offset -> 7 vectors.
 * Decision: CANCELLED `(status == 0) && (data.len == 0) && (sha256.len == 0)`
 * (3 conditions) -- all-true control, then a nonzero status, attached body
 * bytes, and an attached digest in turn -> 4 vectors.
 * Decision: FAILED `(status > 0) && (status <= UINT16_MAX) && (data.len == 0)
 * && (sha256.len == 0)` (4 conditions) -- all-true control, then a zero status,
 * an over-range status, attached body bytes, and an attached digest -> 5
 * vectors.
 * Every rejected vector leaves each earlier condition of its short-circuit
 * chain true, so it pairs with its own control to prove one condition
 * independently decides.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_chunk_semantics_valid
 * @details Each state has its own control; the four states are mutually
 * exclusive arms of one switch, so a vector for one cannot disturb another.
 * @pre The private validation seam is linked into this executable.
 * @pre The digest and body fixtures outlive every vector.
 * @post Exactly the four state controls are accepted.
 * @post No link, session, or model state is touched.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_state_semantics(void)
{
  TEST_BEGIN("mdl client state semantics MC/DC");
  Ra8__Mdl__Chunk control = {};
  Ra8__Mdl__Chunk vector  = {};

  internal_base_chunk(&control, RA8__MDL__STATE__STATE_DOWNLOADING);
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&control));
  vector        = control;
  vector.status = (int32_t)k_internal_fail_status;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector      = control;
  vector.data = (ProtobufCBinaryData){};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector        = control;
  vector.sha256 = (ProtobufCBinaryData){.len = k_ra8_mdl_sha256_bytes, .data = s_digest};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));

  internal_base_chunk(&control, RA8__MDL__STATE__STATE_COMPLETE);
  TEST_ASSERT(ra8_c6link_mdl_chunk_semantics_valid_test(&control));
  vector        = control;
  vector.status = (int32_t)k_internal_fail_status;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector      = control;
  vector.data = (ProtobufCBinaryData){.len = k_internal_body_len, .data = s_body};
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector            = control;
  vector.sha256.len = (size_t)k_ra8_mdl_sha256_bytes - 1U;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector             = control;
  vector.sha256.data = nullptr;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));
  vector             = control;
  vector.total_bytes = k_internal_total_over;
  TEST_ASSERT(!ra8_c6link_mdl_chunk_semantics_valid_test(&vector));

  internal_terminal_state_semantics();
  TEST_END("mdl client state semantics MC/DC");
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each Start argument independently decides rejection.
 * @par MC/DC:
 * Decisions: `(link == nullptr) || (session == nullptr)` in
 * `ra8_c6link_mdl_start_request()` (2 conditions) and
 * `(request == nullptr) || (request->url == nullptr) || (out_url_len == nullptr)`
 * in `internal_mdl_start_request_valid()` (3 conditions)
 * - Vector 1: every argument present -> F,F,F,F -> false (the request reaches
 *   the modelled service and is accepted).
 * - Vectors 2..5: exactly one argument nulled in turn -> `k_ra8_err_null_ptr`.
 * Decision: `((uint32_t)format > rabook) || (timeout_ms > max) ||
 * !field_valid(user_agent) || !field_valid(referer) ||
 * !field_valid(if_none_match) || !field_valid(if_modified_since)`
 * (6 conditions)
 * - Vector 1: a legal format, an in-range timeout, and four clean headers ->
 *   all six false -> the request proceeds.
 * - Vectors 2..7: an out-of-range format, an over-ceiling timeout, then one
 *   CR-bearing header at a time -> `k_ra8_err_invalid_arg`.
 * Each rejected vector pairs with its control to prove one condition
 * independently decides. N+1 = 5 and 7 vectors for N=4 and N=6.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_start_request_valid
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@ra8_c6link_mdl_start_request
 * @details Drives the public entry point against the shared C6 model, so the
 * accepted controls exercise the real encode and correlation path.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The rejected vectors never reach the transport.
 * @post Every rejected vector leaves the caller session untouched.
 * @post Each accepted control starts exactly one correlated job.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_start_arguments(void)
{
  TEST_BEGIN("mdl client Start argument MC/DC");
  priv_c6link_test_bringup();
  ra8_c6link_t*           link    = priv_c6link_test_link();
  ra8_mdl_session_t       session = {};
  const ra8_mdl_request_t base = {.url    = "https://example.test/book",
                                  .format = k_mdl_format_rabook,
                                  .http   = {.user_agent        = "ra8/1",
                                             .referer           = "https://example.test/",
                                             .if_none_match     = "W/x",
                                             .if_modified_since = "Thu, 01 Jan 1970 00:00:00 GMT"}};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_start_request(link, &base, &session));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(link, &session));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_mdl_start_request(nullptr, &base, &session));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_mdl_start_request(link, nullptr, &session));
  ra8_mdl_request_t vector = base;
  vector.url               = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_mdl_start_request(link, &vector, &session));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_mdl_start_request(link, &base, nullptr));

  vector        = base;
  vector.format = k_mdl_format_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  vector                 = base;
  vector.http.timeout_ms = k_internal_timeout_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  vector                 = base;
  vector.http.user_agent = "a\rb";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  vector              = base;
  vector.http.referer = "a\rb";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  vector                    = base;
  vector.http.if_none_match = "a\rb";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  vector                        = base;
  vector.http.if_modified_since = "a\rb";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_mdl_start_request(link, &vector, &session));
  TEST_END("mdl client Start argument MC/DC");
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each correlation field independently decides acknowledgement.
 * @par MC/DC:
 * Decision: `(n_unknown_fields == 0) && (protocol_version == expected) &&
 * (job_id == session->job_id) && (status == 0)` (4 conditions)
 * - Vector 1: a canonical acknowledgement -> T,T,T,T -> true (the session is
 *   deactivated).
 * - Vector 2: an unknown protobuf field appended -> F,-,-,- -> false (supplied
 *   by the modelled unknown-field response in the media suite).
 * - Vector 3: protocol version 999 -> T,F,-,- -> false.
 * - Vector 4: an unissued job identity -> T,T,F,- -> false.
 * - Vector 5: a nonzero cancellation status -> T,T,T,F -> false.
 * Each of vectors 2..5 pairs with vector 1 to prove one condition
 * independently decides. N+1 = 5 vectors for N=4.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_take_cancelled
 * @details Uses the private seam: the C6 model derives its acknowledgement
 * from live session state, so it cannot vary version, identity, and status one
 * at a time.
 * @pre The shared C6 model fixture is brought up so the link arena is live.
 * @pre The packed acknowledgement fits the bounded scratch buffer.
 * @post Only the canonical acknowledgement deactivates the session.
 * @post Every rejected vector leaves the session active.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_cancel_ack(void)
{
  TEST_BEGIN("mdl client cancel acknowledgement MC/DC");
  priv_c6link_test_bringup();
  ra8_c6link_t*       link = priv_c6link_test_link();
  Ra8__Mdl__Cancelled base = RA8__MDL__CANCELLED__INIT;
  base.protocol_version    = k_ra8_mdl_protocol_version;
  base.job_id              = k_internal_ack_job;
  base.status              = 0;

  Ra8__Mdl__Cancelled vectors[] = {base, base, base, base};
  vectors[1].protocol_version   = k_internal_bad_version;
  vectors[2].job_id             = k_internal_bad_job;
  vectors[3].status             = (int32_t)k_internal_fail_status;
  const ra8_err_t expect[]      = {k_ra8_ok,
                                   k_ra8_err_protocol_error,
                                   k_ra8_err_protocol_error,
                                   k_ra8_err_protocol_error};
  for (uint32_t index = 0U; index < (uint32_t)(sizeof(expect) / sizeof(expect[0])); index++) {
    ra8_mdl_session_t session = {.job_id = k_internal_ack_job, .active = true};
    const size_t      len     = ra8__mdl__cancelled__get_packed_size(&vectors[index]);
    TEST_ASSERT(len <= sizeof(s_packed));
    TEST_ASSERT_EQ(len, ra8__mdl__cancelled__pack(&vectors[index], s_packed));
    TEST_ASSERT_EQ(expect[index],
                   ra8_c6link_mdl_take_cancelled_test(link, &session, s_packed, len));
    TEST_ASSERT_EQ(expect[index] != k_ra8_ok, session.active);
  }
  TEST_END("mdl client cancel acknowledgement MC/DC");
}

/**
 * @test priv_test_c6link_mdl_decode_run
 * @brief Prove each empty-body shape independently decides rejection.
 * @par MC/DC:
 * Decision: `(body->data.data == nullptr) || (body->data.len == 0U)`
 * (2 conditions)
 * - Vector 1: a normal response carrying bytes -> F,F -> false (control,
 *   supplied by every other modelled exchange in this executable).
 * - Vector 2: a present but zero-length body on the wire -> T,- -> true
 *   (supplied here).
 * The F,T vector is unreachable and the decision carries the matching
 * `mcdc-deactivated` rationale in the source. This scenario is the evidence
 * for it: protobuf-c packs a non-null zero-length bytes field onto the wire
 * (only a NULL pointer counts as absent to `field_is_zeroish`), but its
 * unpack hands every zero-length field back as a NULL pointer, so the two
 * empty shapes are distinct on the wire and identical once decoded. A
 * mutation that deletes the length operand leaves this vector passing, which
 * is exactly what deactivation records.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_take_response
 * @details Keeps the fail-closed behaviour pinned even though the operand
 * behind it cannot be selected independently: a co-processor that answers with
 * an empty body must not be read as a successful start.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The injected fault is consumed by the first Start response.
 * @post The Start is rejected as a protocol error, not accepted as empty.
 * @post No session is activated.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_empty_body(void)
{
  TEST_BEGIN("mdl client empty response body MC/DC");
  priv_c6link_test_bringup();
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_response_zero_len;
  ra8_mdl_session_t session = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_start(priv_c6link_test_link(),
                                      "https://example.test/book",
                                      k_mdl_format_rabook,
                                      &session));
  TEST_ASSERT(!session.active);
  TEST_END("mdl client empty response body MC/DC");
}

RA8_PRIV void priv_test_c6link_mdl_decode_run(void)
{
  (void)memset(s_body, 'z', sizeof(s_body));
  (void)memset(s_digest, 0xA5, sizeof(s_digest));
  internal_test_header_bytes();
  internal_test_response_metadata();
  internal_test_total_covers_data();
  internal_test_state_semantics();
  internal_test_start_arguments();
  internal_test_cancel_ack();
  internal_test_empty_body();
}
