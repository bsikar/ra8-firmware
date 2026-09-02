/**
 * @file test_ra8_c6link_media_http.c
 * @brief Protocol-v3 request-policy and response-metadata vectors.
 * @details Drives every conditional-request header, the request timeout, and
 * every terminal and nonterminal response-metadata operand through the full
 * modelled wire path, while keeping the primary media test translation unit
 * inside its size cap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6_model.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "test_ra8_c6link_media_http_internal.h"
#include "unity_minimal.h"

/**
 * @brief Implementation of `priv_test_c6link_media_before_terminal()`.
 */
RA8_PRIV
void priv_test_c6link_media_before_terminal(ra8_mdl_session_t* session)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_mdl_start(priv_c6link_test_link(),
                                      "https://example.test/book",
                                      k_mdl_format_rabook,
                                      session));
  ra8_mdl_chunk_t data = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_next(priv_c6link_test_link(), session, 6U, &data));
  TEST_ASSERT_EQ(6, data.data_len);
  TEST_ASSERT_EQ(k_ra8_mdl_state_downloading, data.state);
  TEST_ASSERT(session->active);
  TEST_ASSERT_EQ(6, session->next_offset);
}

/**
 * @brief Reject one single-field HTTP-policy variation of an accepted request.
 * @details Sends a request that differs from the accepted control in exactly
 * one policy operand and proves the client rejects it before transport.
 * @param[in] request Malformed candidate derived from the accepted control.
 * @pre The shared model fixture is brought up and no job is active.
 * @pre @p request differs from the accepted control in exactly one operand.
 * @post The rejection happens before any remote job is started.
 * @post The modelled service observes no additional Start.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_mdl_expect_policy_rejection(const ra8_mdl_request_t* request)
{
  const uint16_t    cancels = ra8_c6_model()->mdl_cancels;
  ra8_mdl_session_t session = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_c6link_mdl_start_request(priv_c6link_test_link(), request, &session));
  TEST_ASSERT(!session.active);
  TEST_ASSERT_EQ(cancels, ra8_c6_model()->mdl_cancels);
}

/**
 * @brief Reject each single-operand HTTP-policy variation of the control.
 * @details Carries vectors V1 through V6 of the Start policy matrix: the
 * timeout bound, one CR or LF inside each of the four conditional-request
 * headers, and a cap-length User-Agent.
 * @param[in] control Accepted all-false control request.
 * @pre The shared model fixture is brought up and no job is active.
 * @pre @p control is the request the client has already accepted.
 * @post Every candidate is rejected before transport.
 * @post The modelled service observes no additional Start.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_mdl_expect_policy_rejections(const ra8_mdl_request_t* control)
{
  ra8_mdl_request_t candidate = *control;
  candidate.http.timeout_ms   = (uint32_t)k_ra8_mdl_timeout_ms_max + 1U;
  internal_mdl_expect_policy_rejection(&candidate);

  candidate                 = *control;
  candidate.http.user_agent = "ra8-media/3\rX-Injected: 1";
  internal_mdl_expect_policy_rejection(&candidate);

  candidate              = *control;
  candidate.http.referer = "https://example.test/catalog\nX-Injected: 1";
  internal_mdl_expect_policy_rejection(&candidate);

  candidate                    = *control;
  candidate.http.if_none_match = "\"cached-etag\"\rX-Injected: 1";
  internal_mdl_expect_policy_rejection(&candidate);

  candidate                        = *control;
  candidate.http.if_modified_since = "Tue, 20 Oct 2015 07:28:00 GMT\nX-Injected: 1";
  internal_mdl_expect_policy_rejection(&candidate);

  char overlong_agent[k_ra8_mdl_user_agent_max + 1U];
  (void)memset(overlong_agent, 'u', sizeof(overlong_agent));
  overlong_agent[k_ra8_mdl_user_agent_max] = '\0';
  candidate                                = *control;
  candidate.http.user_agent                = overlong_agent;
  internal_mdl_expect_policy_rejection(&candidate);
}

/**
 * @test internal_test_media_start_request_policy_mcdc
 * @brief Exercise every protocol-v3 HTTP-policy operand of Start.
 * @par MC/DC:
 * ::ra8_c6link_mdl_start always supplies a zero-initialised policy, so these
 * operands are reachable only through ::ra8_c6link_mdl_start_request. The
 * complete policy accepted first is the all-false control for
 * `(format > rabook) || (timeout_ms > max) || !field_valid(user_agent) ||
 * !field_valid(referer) || !field_valid(if_none_match) ||
 * !field_valid(if_modified_since)`: it carries a concrete format, the maximum
 * legal timeout, and four bounded single-line headers. Each vector below then
 * flips exactly one conjunct against that control:
 * - V1 timeout_ms = max + 1 -> only the timeout conjunct true.
 * - V2 a CR inside User-Agent -> only its field check false.
 * - V3 an LF inside Referer -> only its field check false.
 * - V4 a CR inside If-None-Match -> only its field check false.
 * - V5 an LF inside If-Modified-Since -> only its field check false.
 * - V6 a User-Agent whose length equals its cap -> the same conjunct false
 *   through the length bound rather than the line-break scan.
 * The reserved invalid format flips the first conjunct in
 * `internal_test_media_start_argument_mcdc`.
 * For the inner `(text[index] == '\\r') || (text[index] == '\\n')` decision the
 * accepted control walks four header values whose every byte is F,F/false;
 * V2 and V4 execute T,-/true and V3 and V5 execute F,T/true, so both
 * conjuncts independently decide. The final absent-policy request leaves all
 * four pointers null, executing the `text == nullptr` early return that makes
 * an omitted header valid.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_start_request_valid
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@ra8_c6link_mdl_start_request
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_http_field_valid
 * @details Drives the request-taking Start entry point against the real
 * modelled link and asserts the C6 service observed each accepted field.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The declared header caps fit the fixture's local arrays.
 * @post Every accepted control job is cancelled and inactive.
 * @post No rejected policy reaches the modelled service.
 * @note The absent-policy request proves null and empty both mean "omitted".
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_media_start_request_policy_mcdc(void)
{
  TEST_BEGIN("c6link media Start request policy MC/DC");
  priv_c6link_test_bringup();
  ra8_c6link_t*           link    = priv_c6link_test_link();
  ra8_mdl_session_t       session = {};
  const ra8_mdl_request_t control = {
    .url    = "https://example.test/book",
    .format = k_mdl_format_rabook,
    .http   = {.user_agent        = "ra8-media/3",
               .referer           = "https://example.test/catalog",
               .if_none_match     = "\"cached-etag\"",
               .if_modified_since = "Tue, 20 Oct 2015 07:28:00 GMT",
               .timeout_ms        = (uint32_t)k_ra8_mdl_timeout_ms_max},
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_start_request(link, &control, &session));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ(k_ra8_mdl_timeout_ms_max, ra8_c6_model()->mdl_timeout_ms);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_user_agent, control.http.user_agent) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_referer, control.http.referer) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_if_none_match, control.http.if_none_match) == 0);
  TEST_ASSERT(strcmp(ra8_c6_model()->mdl_if_modified_since, control.http.if_modified_since) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(link, &session));

  internal_mdl_expect_policy_rejections(&control);

  const ra8_mdl_request_t absent = {.url    = "https://example.test/book",
                                    .format = k_mdl_format_rabook};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_start_request(link, &absent, &session));
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_timeout_ms);
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_user_agent[0]);
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_referer[0]);
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_if_none_match[0]);
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_if_modified_since[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(link, &session));
  TEST_ASSERT(!session.active);
  TEST_END("c6link media Start request policy MC/DC");
}

/**
 * @brief Reject one single-field response-metadata mutation of a valid chunk.
 * @details Brings the fixture up, optionally consumes the one data frame so
 * the faulted response is terminal, then proves the mutated response is
 * rejected without advancing the caller's correlation state.
 * @param[in] fault Metadata mutation applied to the next modelled response.
 * @param[in] terminal Whether the faulted response must be the terminal one.
 * @pre The shared model fixture can be reset and brought up.
 * @pre @p fault mutates exactly one response-metadata field.
 * @post The rejected pull leaves the session active at its prior offset.
 * @post No caller chunk receives the mutated metadata.
 * @note File-local helper; each call creates an independent modelled job.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_mdl_expect_metadata_rejection(ra8_c6_model_mdl_fault_t fault, bool terminal)
{
  priv_c6link_test_bringup();
  ra8_mdl_session_t session = {};
  if (terminal) {
    priv_test_c6link_media_before_terminal(&session);
  } else {
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_c6link_mdl_start(priv_c6link_test_link(),
                                        "https://example.test/book",
                                        k_mdl_format_rabook,
                                        &session));
  }
  const uint64_t  offset    = session.next_offset;
  const uint32_t  sequence  = session.next_sequence;
  ra8_mdl_chunk_t chunk     = {};
  ra8_c6_model()->mdl_fault = fault;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ(offset, session.next_offset);
  TEST_ASSERT_EQ(sequence, session.next_sequence);
  TEST_ASSERT_EQ(0, chunk.response.status);
}

/**
 * @test internal_test_media_response_metadata_mcdc
 * @brief Prove every response-metadata operand independently rejects a chunk.
 * @par MC/DC:
 * The all-true controls are in `internal_test_media_download_roundtrip`: its
 * terminal chunk carries status 200 with four bounded single-line headers,
 * and its data chunks carry zero status with four empty headers. Each vector
 * below repacks exactly one field of an otherwise canonical response.
 * Nonterminal branch `(http_status == 0) && empty`:
 * - V1 status 100 on a DOWNLOADING chunk -> F,-/false, varying http_status.
 * - V2..V5 make exactly one of Retry-After, ETag, Last-Modified and
 *   Content-Type nonempty on a DOWNLOADING chunk -> T,F/false, each varying
 *   one `field[0] == '\\0'` conjunct of `empty` while every earlier conjunct
 *   stays true.
 * COMPLETE branch:
 * - V6 status 99 -> only `http_status >= min` false.
 * - V7 status 600 -> only `http_status <= max` false.
 * - V8..V11 inject one CR or LF into exactly one selected header -> only that
 *   header's ::internal_mdl_http_field_valid result false. V8 and V10 carry
 *   CR and V9 and V11 carry LF, so the inner
 *   `(text[index] == '\\r') || (text[index] == '\\n')` decision executes F,F
 *   over every valid byte, T,-/true on V8 and V10, and F,T/true on V9 and
 *   V11.
 * The four `field != nullptr` conjuncts of `empty` cannot be driven false:
 * the generated decoder initialises every optional string field to the shared
 * empty string and never yields null, so they are structurally infeasible
 * defensive guards rather than missing vectors.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_http_response_valid
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_http_field_valid
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_chunk_semantics_valid
 * @details Repacks each modelled response through the same generated codec the
 * production decoder uses, so no rejection depends on a hand-built frame.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The modelled artifact is the deterministic six-byte body.
 * @post Every mutated response is rejected with a protocol error.
 * @post No rejected response advances the caller's session.
 * @note Each vector runs on its own job so faults cannot interact.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_media_response_metadata_mcdc(void)
{
  TEST_BEGIN("c6link media response metadata MC/DC");
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_data_http_status, false);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_data_retry_after, false);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_data_etag, false);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_data_last_modified, false);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_data_content_type, false);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_low_status, true);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_high_status, true);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_split_retry, true);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_split_etag, true);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_split_date, true);
  internal_mdl_expect_metadata_rejection(k_c6m_mdl_fault_complete_split_type, true);
  TEST_END("c6link media response metadata MC/DC");
}

/**
 * @brief Implementation of `priv_test_c6link_media_http_run()`.
 */
RA8_PRIV void priv_test_c6link_media_http_run(void)
{
  internal_test_media_start_request_policy_mcdc();
  internal_test_media_response_metadata_mcdc();
}
