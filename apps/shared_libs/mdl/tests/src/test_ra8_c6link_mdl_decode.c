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
  k_internal_fail_status  = 7U,     /**< Nonzero canonical failure status.        */
  k_internal_timeout_over = 60001U, /**< One above the caller timeout ceiling.    */
  k_internal_packed_bytes = 64U,    /**< Packed acknowledgement scratch capacity. */
  k_internal_ack_job      = 1U,     /**< Job identity the acknowledgement echoes. */
  k_internal_bad_version  = 999U,   /**< Protocol version no endpoint speaks.     */
  k_internal_bad_job      = 4242U,  /**< Job identity no service ever issued.     */
} internal_decode_const_t;

static uint8_t s_packed[k_internal_packed_bytes];

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
 * libs/ra8_c6link/src/internal/mdl_request.zig@httpFieldValid
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
 * @brief Prove each Start argument independently decides rejection.
 * @par MC/DC:
 * Decisions: `(link == nullptr) || (session == nullptr)` in
 * `ra8_c6link_mdl_start_request()` (2 conditions) and
 * `(request == nullptr) || (request->url == nullptr) || (out_url_len == nullptr)`
 * in `mdl_request.zig@startRequestValid` (3 conditions)
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
 * libs/ra8_c6link/src/internal/mdl_request.zig@startRequestValid
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
  internal_test_header_bytes();
  internal_test_start_arguments();
  internal_test_cancel_ack();
  internal_test_empty_body();
}
