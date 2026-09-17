/**
 * @file test_ra8_usb_hcdc_pipe_cov.c
 * @brief White-box coverage for host-CDC pipe setup error propagation.
 * @details Uses a synchronous endpoint-configuration script while compiling
 * the production class control flow unchanged as a private test copy.
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
  k_t_pipe_first_call  = 1U,   /**< Bulk-IN configuration call.    */
  k_t_pipe_second_call = 2U,   /**< Bulk-OUT configuration call.   */
  k_t_pipe_never_fail  = 0xFFU /**< Sentinel selecting no failure. */
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
/** @brief Rename the copied host-CDC initializer for private linkage. */
#define ra8_usb_hcdc_init ra8_usb_hcdc_init_pipe_cov
/** @brief Rename the copied host-CDC close entry point. */
#define ra8_usb_hcdc_close ra8_usb_hcdc_close_pipe_cov
/** @brief Rename the copied host-CDC callback registration entry point. */
#define ra8_usb_hcdc_attach_callback ra8_usb_hcdc_attach_callback_pipe_cov
/** @brief Rename the copied host-CDC step entry point. */
#define ra8_usb_hcdc_step ra8_usb_hcdc_step_pipe_cov
/** @brief Rename the copied host-CDC line-coding entry point. */
#define ra8_usb_hcdc_set_line_coding ra8_usb_hcdc_set_line_coding_pipe_cov
/** @brief Rename the copied host-CDC transmit entry point. */
#define ra8_usb_hcdc_send ra8_usb_hcdc_send_pipe_cov
/** @brief Rename the copied host-CDC receive entry point. */
#define ra8_usb_hcdc_recv ra8_usb_hcdc_recv_pipe_cov
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_hcdc.c" // NOLINT(bugprone-suspicious-include) -- white-box transport seam

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
 * @brief Reset host-CDC pipe state for one scripted configuration attempt.
 * @details Initializes the production private state with valid bulk-IN,
 * bulk-OUT, and interrupt-IN endpoint numbers and selects a failing call.
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
  s_configure_call_count     = 0U;
  s_configure_fail_call      = fail_call;
  s_state.speed              = k_ra8_usb_speed_fs;
  s_state.device.bulk_in_ep  = 1U;
  s_state.device.bulk_out_ep = 2U;
  s_state.device.intr_in_ep  = 3U;
}

/**
 * @test internal_test_bulk_errors
 * @brief Verify both host-CDC bulk-pipe errors and the success path.
 * @details Selects each bulk endpoint call in turn for failure, then runs the
 * same production helper with no injected error.
 * @pre The private production copy is linked to the synchronous endpoint mock.
 * @pre The fixture provides valid bulk-IN and bulk-OUT endpoint descriptors.
 * @post Each selected failure status is returned unchanged.
 * @post The no-failure script completes both bulk configurations successfully.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_bulk_errors(void)
{
  TEST_BEGIN("host CDC propagates both bulk-pipe setup failures");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_configure_pipes());
  TEST_ASSERT_EQ(k_t_pipe_first_call, s_configure_call_count);

  internal_prep_pipe_test((uint8_t)k_t_pipe_second_call);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_configure_pipes());
  TEST_ASSERT_EQ(k_t_pipe_second_call, s_configure_call_count);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  TEST_ASSERT_EQ(k_ra8_ok, internal_configure_pipes());
  TEST_END("host CDC propagates both bulk-pipe setup failures");
}

/**
 * @test internal_test_walk_desc_propagates_pipe_error
 * @brief Verify the enumeration caller preserves pipe-setup failure semantics.
 * @details Drives WALK_DESC through failure and success; only success may mark
 * the device attached or advance the state machine.
 * @par MC/DC:
 * The single-condition error propagation branch executes both outcomes.
 */
RA8_INTERNAL static void internal_test_walk_desc_propagates_pipe_error(void)
{
  TEST_BEGIN("host CDC walk-desc propagates pipe setup status");
  internal_prep_pipe_test((uint8_t)k_t_pipe_first_call);
  s_state.initialized = true;
  s_state.attached    = false;
  s_state.step        = k_ra8_hcdc_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_step());
  TEST_ASSERT(!s_state.attached);
  TEST_ASSERT_EQ(k_ra8_hcdc_step_walk_desc, s_state.step);

  internal_prep_pipe_test((uint8_t)k_t_pipe_never_fail);
  s_state.initialized = true;
  s_state.attached    = false;
  s_state.step        = k_ra8_hcdc_step_walk_desc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  TEST_ASSERT(s_state.attached);
  TEST_ASSERT_EQ(k_ra8_hcdc_step_done, s_state.step);
  TEST_END("host CDC walk-desc propagates pipe setup status");
}

/**
 * @test internal_test_private_copy_mcdc
 * @brief Cover the speed and nullable-send decisions in the private CDC copy.
 * @details The shared driver object is covered elsewhere; this executable's
 * included copy needs its own N+1 vectors for both two-condition decisions.
 * @pre Off-target USB register fakes are active.
 * @post The private state is left detached.
 * @note No USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_private_copy_mcdc(void)
{
  TEST_BEGIN("host CDC private copy has complete MC/DC vectors");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_close());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_init((ra8_usb_speed_t)9U));

  s_state.initialized = true;
  s_state.attached    = true;
  s_state.speed       = k_ra8_usb_speed_fs;
  uint8_t data        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_send(nullptr, 4U));
  TEST_ASSERT(ra8_usb_hcdc_send(nullptr, 0U) != k_ra8_err_invalid_arg);
  TEST_ASSERT(ra8_usb_hcdc_send(&data, 1U) != k_ra8_err_invalid_arg);
  s_state.initialized = false;
  s_state.attached    = false;
  TEST_END("host CDC private copy has complete MC/DC vectors");
}

int main(void)
{
  internal_test_bulk_errors();
  internal_test_walk_desc_propagates_pipe_error();
  internal_test_private_copy_mcdc();
  return 0;
}
