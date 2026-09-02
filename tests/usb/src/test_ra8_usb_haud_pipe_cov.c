/**
 * @file test_ra8_usb_haud_pipe_cov.c
 * @brief White-box coverage for host-audio pipe setup error propagation.
 * @details Compiles a private copy of the class driver with only the endpoint
 * configuration dependency replaced by a deterministic synchronous script.
 * The production control flow and state object remain unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_t_pipe_first_call = 1U,   /**< First endpoint-configuration call. */
  k_t_pipe_never_fail = 0xFFU /**< Sentinel selecting no failure.     */
} t_pipe_call_t;

/** @brief Number of endpoint-configuration calls in the current script. */
static uint8_t s_configure_call_count;
/** @brief One-based endpoint-configuration call selected to fail. */
static uint8_t s_configure_fail_call;

RA8_INTERNAL static ra8_err_t internal_mock_configure_endpoint(ra8_usb_speed_t   speed,
                                                               uint8_t           pipe_num,
                                                               uint8_t           ep_addr,
                                                               ra8_usb_ep_dir_t  dir,
                                                               ra8_usb_ep_type_t type,
                                                               uint16_t          max_packet);

// NOLINTBEGIN(readability-identifier-naming) -- exact external symbols form the include-time seam.
/** @brief Redirect endpoint configuration to the deterministic test script. */
#define ra8_usb_configure_endpoint internal_mock_configure_endpoint
/** @brief Rename the copied host-audio initializer for private linkage. */
#define ra8_usb_haud_init ra8_usb_haud_init_pipe_cov
/** @brief Rename the copied host-audio close entry point. */
#define ra8_usb_haud_close ra8_usb_haud_close_pipe_cov
/** @brief Rename the copied host-audio callback registration entry point. */
#define ra8_usb_haud_attach_callback ra8_usb_haud_attach_callback_pipe_cov
/** @brief Rename the copied host-audio step entry point. */
#define ra8_usb_haud_step ra8_usb_haud_step_pipe_cov
/** @brief Rename the copied host-audio format entry point. */
#define ra8_usb_haud_set_format ra8_usb_haud_set_format_pipe_cov
/** @brief Rename the copied host-audio volume entry point. */
#define ra8_usb_haud_set_volume ra8_usb_haud_set_volume_pipe_cov
/** @brief Rename the copied host-audio mute entry point. */
#define ra8_usb_haud_set_mute ra8_usb_haud_set_mute_pipe_cov
/** @brief Rename the copied host-audio transmit entry point. */
#define ra8_usb_haud_send_samples ra8_usb_haud_send_samples_pipe_cov
/** @brief Rename the copied host-audio receive entry point. */
#define ra8_usb_haud_recv_samples ra8_usb_haud_recv_samples_pipe_cov
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_haud.c" // NOLINT(bugprone-suspicious-include) -- white-box transport seam

/**
 * @brief Return the scripted result for one endpoint configuration call.
 * @details Counts each production request and injects an error only at the
 * one-based call selected by ::s_configure_fail_call.
 * @param[in] speed Negotiated USB bus speed supplied by the class driver.
 * @param[in] pipe_num Host-controller pipe selected by the class driver.
 * @param[in] ep_addr Endpoint address selected from the parsed descriptor.
 * @param[in] dir Endpoint transfer direction.
 * @param[in] type Endpoint transfer type.
 * @param[in] max_packet Endpoint maximum packet size in bytes.
 * @return Scripted endpoint-configuration result.
 * @retval k_ra8_err_invalid_arg The selected call was configured to fail.
 * @retval k_ra8_ok The call was not selected to fail.
 * @pre ::s_configure_call_count and ::s_configure_fail_call describe the
 * current bounded script.
 * @pre The supplied endpoint arguments obey the production interface contract.
 * @post ::s_configure_call_count is incremented exactly once.
 * @post No production-owned buffer or hardware register is modified.
 * @note File-local synchronous test seam; it performs no hardware access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mock_configure_endpoint(ra8_usb_speed_t   speed,
                                                               uint8_t           pipe_num,
                                                               uint8_t           ep_addr,
                                                               ra8_usb_ep_dir_t  dir,
                                                               ra8_usb_ep_type_t type,
                                                               uint16_t          max_packet)
{
  (void)speed;
  (void)pipe_num;
  (void)ep_addr;
  (void)dir;
  (void)type;
  (void)max_packet;
  ++s_configure_call_count;
  return (s_configure_call_count == s_configure_fail_call) ? k_ra8_err_invalid_arg : k_ra8_ok;
}

/**
 * @brief Reset host-audio pipe state for one scripted configuration attempt.
 * @details Initializes the production private state with one valid ISO-OUT
 * endpoint and selects which configuration call, if any, should fail.
 * @param[in] fail_call One-based call number to fail, or the no-failure sentinel.
 * @pre @p fail_call is a member of ::t_pipe_call_t.
 * @pre The copied class state object is available.
 * @post The call counter is zero and the pipe state is ready for configuration.
 * @post Only file-local fixture state is mutated.
 * @note File-local fixture setup; no ownership escapes this executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_prep_pipe_test(uint8_t fail_call)
{
  s_configure_call_count            = 0U;
  s_configure_fail_call             = fail_call;
  s_state.speed                     = k_ra8_usb_speed_fs;
  s_state.device.iso_out_ep         = 1U;
  s_state.device.iso_out_max_packet = 64U;
  s_state.device.iso_in_ep          = 0U;
  s_state.device.iso_in_max_packet  = 0U;
}

/**
 * @test internal_test_iso_out_error
 * @brief Verify host-audio ISO-OUT setup error propagation and success.
 * @details Drives the first configure call to failure, then repeats with the
 * script configured for success so both branch outcomes execute.
 * @pre The private production copy is linked to the synchronous endpoint mock.
 * @pre The fixture provides a valid ISO-OUT endpoint descriptor.
 * @post The failing status is returned unchanged on the selected call.
 * @post The no-failure script completes pipe configuration successfully.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_iso_out_error(void)
{
  TEST_BEGIN("host audio propagates ISO-OUT setup failure");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_configure_pipes());
  TEST_ASSERT_EQ(k_t_pipe_first_call, s_configure_call_count);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  TEST_ASSERT_EQ(k_ra8_ok, internal_configure_pipes());
  TEST_END("host audio propagates ISO-OUT setup failure");
}

/**
 * @test internal_test_walk_desc_propagates_pipe_error
 * @brief Verify the enumeration caller preserves pipe-setup failure semantics.
 * @details Drives the public step seam at WALK_DESC with a failing and a
 * succeeding endpoint script, proving failure neither attaches nor advances.
 * @par MC/DC:
 * The single-condition error propagation branch executes both outcomes.
 */
RA8_INTERNAL static void internal_test_walk_desc_propagates_pipe_error(void)
{
  TEST_BEGIN("host audio walk-desc propagates pipe setup status");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  s_state.initialized = true;
  s_state.attached    = false;
  s_state.step        = k_ra8_haud_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_step());
  TEST_ASSERT(!s_state.attached);
  TEST_ASSERT_EQ(k_ra8_haud_step_walk_desc, s_state.step);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  s_state.initialized = true;
  s_state.attached    = false;
  s_state.step        = k_ra8_haud_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_step());
  TEST_ASSERT(s_state.attached);
  TEST_ASSERT_EQ(k_ra8_haud_step_done, s_state.step);
  TEST_END("host audio walk-desc propagates pipe setup status");
}

/**
 * @test internal_test_private_copy_mcdc
 * @brief Cover compound decisions in this test's private host-audio copy.
 * @details The normal host-audio test covers the shared production object;
 * this white-box executable includes a second copy, so it must independently
 * exercise the speed, buffer, and format-range decisions.
 * @pre Off-target USB register fakes are active.
 * @post The private driver is closed after valid initialization vectors.
 * @note No USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_private_copy_mcdc(void)
{
  TEST_BEGIN("host audio private copy has complete MC/DC vectors");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_close());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_init((ra8_usb_speed_t)9U));

  uint8_t sample = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_haud_send_samples(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_haud_send_samples(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_send_samples(&sample, 1U));

  TEST_ASSERT(internal_format_ok(2U, 16U, 48000U));
  TEST_ASSERT(!internal_format_ok(0U, 16U, 48000U));
  TEST_ASSERT(!internal_format_ok(9U, 16U, 48000U));
  TEST_ASSERT(!internal_format_ok(2U, 4U, 48000U));
  TEST_ASSERT(!internal_format_ok(2U, 64U, 48000U));
  TEST_ASSERT(!internal_format_ok(2U, 16U, 4000U));
  TEST_ASSERT(!internal_format_ok(2U, 16U, 384000U));
  TEST_END("host audio private copy has complete MC/DC vectors");
}

int main(void)
{
  internal_test_iso_out_error();
  internal_test_walk_desc_propagates_pipe_error();
  internal_test_private_copy_mcdc();
  return 0;
}
