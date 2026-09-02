/**
 * @file test_ra8_c6link_media_decoder.c
 * @brief Malformed C6 media-response decoder vectors.
 * @details Drives faulted generated responses through the full modelled wire
 * path while keeping the primary media/transfer test translation unit capped.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6_model.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "test_ra8_c6link_media_decoder_internal.h"
#include "unity_minimal.h"

/**
 * @brief Assert that one faulted Start response is rejected atomically.
 * @details Resets the shared model, injects one response fault, and verifies
 * the public Start call rejects it without publishing correlation state.
 * @param[in] fault Model fault applied to the next Start response.
 * @return Nothing.
 * @pre @p fault selects a malformed Accepted or outer CustomRpc response.
 * @pre The shared model can be reset and brought up.
 * @post Start returns `k_ra8_err_protocol_error`.
 * @post The caller session remains inactive and zero-correlated.
 * @note Re-brings up the model so each vector is independent.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_start_fault(ra8_c6_model_mdl_fault_t fault)
{
  priv_c6link_test_bringup();
  ra8_c6_model()->mdl_fault = fault;
  ra8_mdl_session_t session = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_start(priv_c6link_test_link(),
                                      "https://example.test/book",
                                      k_mdl_format_rabook,
                                      &session));
  TEST_ASSERT(!session.active);
  TEST_ASSERT_EQ(0, session.job_id);
}

/**
 * @test internal_test_accepted_decoder_mcdc
 * @brief Exercise every generated Accepted field-validation operand.
 * @par MC/DC:
 * The canonical Start in the main media suite supplies the all-true control,
 * and its unknown-field response test independently falsifies the first
 * operand. These vectors independently corrupt protocol version, job id,
 * nonzero chunk cap, and maximum chunk cap while every other field stays
 * canonical.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_take_accepted
 * @details Sends four otherwise-valid Accepted responses through the real
 * outer and inner generated codecs and the synchronous response extractor.
 * @pre The shared model fixture can be reset and brought up.
 * @pre The model fault seam consumes exactly one response fault.
 * @post Every malformed Accepted response returns a protocol error.
 * @post No rejected response activates the caller session.
 * @note The sibling unknown-field vector remains in the primary media suite.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_accepted_decoder_mcdc(void)
{
  TEST_BEGIN("c6link media Accepted decoder MC/DC");
  internal_assert_start_fault(k_c6m_mdl_fault_accepted_bad_version);
  internal_assert_start_fault(k_c6m_mdl_fault_accepted_zero_job);
  internal_assert_start_fault(k_c6m_mdl_fault_accepted_zero_max);
  internal_assert_start_fault(k_c6m_mdl_fault_accepted_large_max);
  internal_assert_start_fault(k_c6m_mdl_fault_accepted_wrong_format);
  TEST_END("c6link media Accepted decoder MC/DC");
}

/**
 * @test internal_test_outer_response_decoder_mcdc
 * @brief Exercise every outer CustomRpc body and payload guard.
 * @par MC/DC:
 * Canonical Start supplies both all-false controls. A missing body and a
 * present body with the wrong operation id independently select the first
 * decision's operands. Empty protobuf bytes select the payload rejection;
 * protobuf-c canonically decodes every zero-length bytes field as `{0,NULL}`,
 * so nonnull-data/zero-length is structurally infeasible rather than a missing
 * executable vector.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@internal_mdl_take_response
 * @details Sends three malformed outer responses through protobuf-c and the
 * ordinary correlated response callback.
 * @pre The shared model fixture can be reset and brought up.
 * @pre The generated protobuf-c runtime preserves its zero-length invariant.
 * @post Every malformed outer response returns a protocol error.
 * @post No rejected response activates the caller session.
 * @note No test calls the private response extractor directly.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_outer_response_decoder_mcdc(void)
{
  TEST_BEGIN("c6link media outer response decoder MC/DC");
  internal_assert_start_fault(k_c6m_mdl_fault_response_no_body);
  internal_assert_start_fault(k_c6m_mdl_fault_response_wrong_id);
  internal_assert_start_fault(k_c6m_mdl_fault_response_empty_data);
  TEST_END("c6link media outer response decoder MC/DC");
}

/**
 * @brief Run the malformed media-response decoder vectors.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The shared C6 model fixture is not in a live transaction.
 * @post Normal return means every malformed response was rejected.
 * @post Every test-created session remains inactive after rejection.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_media_decoder_run(void)
{
  internal_test_accepted_decoder_mcdc();
  internal_test_outer_response_decoder_mcdc();
}
