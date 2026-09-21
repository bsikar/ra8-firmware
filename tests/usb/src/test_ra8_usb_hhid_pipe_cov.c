/**
 * @file test_ra8_usb_hhid_pipe_cov.c
 * @brief White-box coverage for host-HID pipe setup error propagation.
 * @details Compiles the production class flow as a private copy and replaces
 * only endpoint configuration with a deterministic synchronous script.
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
/** @brief Rename the copied host-HID initializer for private linkage. */
#define ra8_usb_hhid_init ra8_usb_hhid_init_pipe_cov
/** @brief Rename the copied host-HID close entry point. */
#define ra8_usb_hhid_close ra8_usb_hhid_close_pipe_cov
/** @brief Rename the copied host-HID callback registration entry point. */
#define ra8_usb_hhid_attach_callback ra8_usb_hhid_attach_callback_pipe_cov
/** @brief Rename the copied host-HID step entry point. */
#define ra8_usb_hhid_step ra8_usb_hhid_step_pipe_cov
/** @brief Rename the copied host-HID GET_REPORT entry point. */
#define ra8_usb_hhid_get_report ra8_usb_hhid_get_report_pipe_cov
/** @brief Rename the copied host-HID SET_REPORT entry point. */
#define ra8_usb_hhid_set_report ra8_usb_hhid_set_report_pipe_cov
/** @brief Rename the copied host-HID SET_IDLE entry point. */
#define ra8_usb_hhid_set_idle ra8_usb_hhid_set_idle_pipe_cov
/** @brief Rename the copied host-HID SET_PROTOCOL entry point. */
#define ra8_usb_hhid_set_protocol ra8_usb_hhid_set_protocol_pipe_cov
/** @brief Rename the copied host-HID input-report entry point. */
#define ra8_usb_hhid_get_input_report ra8_usb_hhid_get_input_report_pipe_cov
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_hhid.c" // NOLINT(bugprone-suspicious-include) -- white-box transport seam

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
 * @brief Reset host-HID pipe state for one scripted configuration attempt.
 * @details Initializes the production private state with one valid interrupt-IN
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
  s_configure_call_count             = 0U;
  s_configure_fail_call              = fail_call;
  s_state.speed                      = k_ra8_usb_speed_fs;
  s_state.device.intr_in_ep          = 1U;
  s_state.device.intr_in_max_packet  = 8U;
  s_state.device.intr_out_ep         = 0U;
  s_state.device.intr_out_max_packet = 0U;
}

/**
 * @test internal_test_intr_in_error
 * @brief Verify host-HID interrupt-IN setup error propagation and success.
 * @details Drives the first configure call to failure, then repeats with the
 * script configured for success so both branch outcomes execute.
 * @pre The private production copy is linked to the synchronous endpoint mock.
 * @pre The fixture provides a valid interrupt-IN endpoint descriptor.
 * @post The failing status is returned unchanged on the selected call.
 * @post The no-failure script completes pipe configuration successfully.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_intr_in_error(void)
{
  TEST_BEGIN("host HID propagates interrupt-IN setup failure");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_configure_pipes());
  TEST_ASSERT_EQ(k_t_pipe_first_call, s_configure_call_count);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  TEST_ASSERT_EQ(k_ra8_ok, internal_configure_pipes());
  TEST_END("host HID propagates interrupt-IN setup failure");
}

/**
 * @test internal_test_walk_desc_propagates_pipe_error
 * @brief Verify the enumeration caller preserves pipe-setup failure semantics.
 * @details Drives WALK_DESC through failure and success; only success advances
 * to the report-descriptor request.
 * @par MC/DC:
 * The single-condition error propagation branch executes both outcomes.
 */
RA8_INTERNAL static void internal_test_walk_desc_propagates_pipe_error(void)
{
  TEST_BEGIN("host HID walk-desc propagates pipe setup status");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  s_state.initialized = true;
  s_state.step        = k_ra8_hhid_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhid_step());
  TEST_ASSERT_EQ(k_ra8_hhid_step_walk_desc, s_state.step);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  s_state.initialized = true;
  s_state.step        = k_ra8_hhid_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_step());
  TEST_ASSERT_EQ(k_ra8_hhid_step_get_report_desc, s_state.step);
  TEST_END("host HID walk-desc propagates pipe setup status");
}

/**
 * @test internal_test_private_copy_mcdc
 * @brief Cover compound decisions in the private host-HID driver copy.
 * @details Supplies the speed, nullable-report, protocol-selector, and report
 * type N+1 vectors that otherwise cover only the shared production object.
 * @pre Off-target USB register fakes are active.
 * @post The private state is left detached.
 * @note No USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_private_copy_mcdc(void)
{
  TEST_BEGIN("host HID private copy has complete MC/DC vectors");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_close());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhid_init((ra8_usb_speed_t)9U));

  TEST_ASSERT(internal_report_type_ok(k_ra8_hhid_report_type_input));
  TEST_ASSERT(internal_report_type_ok(k_ra8_hhid_report_type_output));
  TEST_ASSERT(internal_report_type_ok(k_ra8_hhid_report_type_feature));
  TEST_ASSERT(!internal_report_type_ok((ra8_usb_hhid_report_type_t)0x77U));

  s_state.initialized = true;
  s_state.attached    = true;
  s_state.speed       = k_ra8_usb_speed_fs;
  uint8_t report      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, nullptr, 4U));
  TEST_ASSERT(ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, nullptr, 0U) !=
              k_ra8_err_null_ptr);
  TEST_ASSERT(ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, &report, 1U) !=
              k_ra8_err_null_ptr);
  TEST_ASSERT(ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot) != k_ra8_err_invalid_arg);
  TEST_ASSERT(ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_report) != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_set_protocol((ra8_usb_hhid_protocol_select_t)99U));
  s_state.initialized = false;
  s_state.attached    = false;
  TEST_END("host HID private copy has complete MC/DC vectors");
}

int main(void)
{
  internal_test_intr_in_error();
  internal_test_walk_desc_propagates_pipe_error();
  internal_test_private_copy_mcdc();
  return 0;
}
