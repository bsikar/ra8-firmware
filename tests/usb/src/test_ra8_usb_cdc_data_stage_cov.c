/**
 * @file test_ra8_usb_cdc_data_stage_cov.c
 * @brief White-box coverage for CDC line coding and the EP0 OUT data stage.
 * @details Compiles a private copy of the production CDC layer while replacing
 * only the DCP arm/read and status-response dependencies with deterministic
 * synchronous scripts. No USB controller or live hardware is touched.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_cdc.h"
#include "unity_minimal.h"

/** @brief Scripted DCP arm result. */
static ra8_err_t s_arm_result;
/** @brief Scripted terminal DCP read result. */
static ra8_err_t s_read_result;
/** @brief One-based read call on which the terminal result is returned. */
static uint16_t s_terminal_read_call;
/** @brief Number of DCP reads made by the production poll loop. */
static uint16_t s_read_calls;
/** @brief Scripted CDC line-coding payload copied by a successful DCP read. */
static uint8_t s_payload[7];
/** @brief Scripted control-status response result. */
static ra8_err_t s_control_result;
/** @brief Number of control-status responses requested. */
static uint16_t s_control_calls;
/** @brief Last status-stage ACK selector supplied by the CDC layer. */
static bool s_last_ack;

RA8_INTERNAL static ra8_err_t internal_mock_dcp_out_arm(ra8_usb_speed_t speed);
RA8_INTERNAL static ra8_err_t
internal_mock_dcp_out_read(ra8_usb_speed_t speed, uint8_t* buf, uint16_t cap, uint16_t* out_len);
RA8_INTERNAL static ra8_err_t internal_mock_control_response(ra8_usb_speed_t speed, bool ack);

// NOLINTBEGIN(readability-identifier-naming) -- exact external symbols form the include-time seam.
/** @brief Redirect the DCP OUT arm call to the mock so the data stage can be scripted. */
#define ra8_usb_dcp_out_arm internal_mock_dcp_out_arm
/** @brief Redirect the DCP OUT read call to the mock that returns the scripted payload. */
#define ra8_usb_dcp_out_read internal_mock_dcp_out_read
/** @brief Redirect the control-status response to the mock that records the ACK selector. */
#define ra8_usb_control_response internal_mock_control_response
/**
 * @brief Rename the CDC init entry point so this white-box unit does not
 *        clash with the shared object library.
 */
#define ra8_usb_cdc_init ra8_usb_cdc_init_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_deinit ra8_usb_cdc_deinit_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_attach ra8_usb_cdc_attach_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_send ra8_usb_cdc_send_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_recv ra8_usb_cdc_recv_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_handle_setup ra8_usb_cdc_handle_setup_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_get_line_coding ra8_usb_cdc_get_line_coding_data_stage_cov
/** @brief Renamed for the duplicate-symbol reason given on the init rename. */
#define ra8_usb_cdc_get_line_state ra8_usb_cdc_get_line_state_data_stage_cov
/** @brief Rename the test seam so the private source copy remains link-unique. */
#define ra8_usb_cdc_test_apply_line_coding ra8_usb_cdc_test_apply_line_coding_data_stage_cov
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_cdc.c" // NOLINT(bugprone-suspicious-include) -- white-box DCP seam

/** @brief Reset the deterministic DCP script and private CDC state. */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_arm_result         = k_ra8_ok;
  s_read_result        = k_ra8_err_no_data;
  s_terminal_read_call = 0U;
  s_read_calls         = 0U;
  s_payload[0]         = 0x00U;
  s_payload[1]         = 0xC2U;
  s_payload[2]         = 0x01U;
  s_payload[3]         = 0x00U;
  s_payload[4]         = 2U;
  s_payload[5]         = 3U;
  s_payload[6]         = 7U;
  s_control_result     = k_ra8_ok;
  s_control_calls      = 0U;
  s_last_ack           = false;
  s_state              = (ra8_usb_cdc_state_t){};
  s_state.speed        = k_ra8_usb_speed_fs;
  internal_default_coding(&s_state.coding);
}

/** @brief Return the scripted DCP-arm status. */
RA8_INTERNAL static ra8_err_t internal_mock_dcp_out_arm(ra8_usb_speed_t speed)
{
  (void)speed;
  return s_arm_result;
}

/** @brief Return no-data until the selected terminal read, then copy the payload. */
RA8_INTERNAL static ra8_err_t
internal_mock_dcp_out_read(ra8_usb_speed_t speed, uint8_t* buf, uint16_t cap, uint16_t* out_len)
{
  (void)speed;
  ++s_read_calls;
  if ((s_terminal_read_call == 0U) || (s_read_calls < s_terminal_read_call)) {
    return k_ra8_err_no_data;
  }
  if (s_read_result == k_ra8_ok) {
    TEST_ASSERT(cap >= (uint16_t)sizeof(s_payload));
    (void)memcpy(buf, s_payload, sizeof(s_payload));
    *out_len = (uint16_t)sizeof(s_payload);
  }
  return s_read_result;
}

/** @brief Record and return the scripted status-stage response. */
RA8_INTERNAL static ra8_err_t internal_mock_control_response(ra8_usb_speed_t speed, bool ack)
{
  (void)speed;
  ++s_control_calls;
  s_last_ack = ack;
  return s_control_result;
}

/**
 * @test internal_test_apply_line_coding_vectors
 * @brief Cover both reject conditions and the exact seven-byte decoder.
 * @par MC/DC:
 * For `(data == nullptr) || (len < 7)`, vectors (null,7), (data,6),
 * and (data,7) independently drive both conditions and the false outcome.
 */
RA8_INTERNAL static void internal_test_apply_line_coding_vectors(void)
{
  TEST_BEGIN("CDC line coding rejects invalid payloads and decodes valid bytes");
  internal_prep();
  internal_apply_line_coding(nullptr, k_ra8_cdc_line_coding_len);
  TEST_ASSERT_EQ(9600U, s_state.coding.dte_rate);
  internal_apply_line_coding(s_payload, k_ra8_cdc_line_coding_len - 1U);
  TEST_ASSERT_EQ(9600U, s_state.coding.dte_rate);

  internal_apply_line_coding(s_payload, k_ra8_cdc_line_coding_len);
  TEST_ASSERT_EQ(115200U, s_state.coding.dte_rate);
  TEST_ASSERT_EQ(2U, s_state.coding.char_format);
  TEST_ASSERT_EQ(3U, s_state.coding.parity_type);
  TEST_ASSERT_EQ(7U, s_state.coding.data_bits);
  TEST_END("CDC line coding rejects invalid payloads and decodes valid bytes");
}

/**
 * @test internal_test_pull_data_stage_vectors
 * @brief Cover validation, arm failure, bounded no-data, read error, and success.
 * @par MC/DC:
 * The poll's single-condition terminal-result branch executes false on no-data
 * and true on both an error and a successful landed packet.
 */
RA8_INTERNAL static void internal_test_pull_data_stage_vectors(void)
{
  TEST_BEGIN("CDC DCP data-stage pull propagates every scripted outcome");
  internal_prep();
  uint8_t  buf[sizeof(s_payload)] = {};
  uint16_t len                    = 99U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_pull_data_stage(nullptr, (uint16_t)sizeof(buf), &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, internal_pull_data_stage(buf, (uint16_t)sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_pull_data_stage(buf, 0U, &len));

  s_arm_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &len));

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_no_data, internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &len));
  TEST_ASSERT_EQ(k_ra8_cdc_data_stage_polls, s_read_calls);

  internal_prep();
  s_terminal_read_call = 1U;
  s_read_result        = k_ra8_err_invalid_size;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &len));
  TEST_ASSERT_EQ(1U, s_read_calls);

  internal_prep();
  s_terminal_read_call = 2U;
  s_read_result        = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &len));
  TEST_ASSERT_EQ(2U, s_read_calls);
  TEST_ASSERT_EQ(sizeof(s_payload), len);
  TEST_ASSERT_EQ(0, memcmp(buf, s_payload, sizeof(buf)));
  TEST_END("CDC DCP data-stage pull propagates every scripted outcome");
}

/**
 * @test internal_test_set_line_coding_landed_packet
 * @brief Prove SET_LINE_CODING applies a mocked landed packet before ACKing.
 * @par MC/DC:
 * The `pull == k_ra8_ok` branch executes true here and false in the bounded
 * no-data vector above; the direct decoder test covers both input guards.
 */
RA8_INTERNAL static void internal_test_set_line_coding_landed_packet(void)
{
  TEST_BEGIN("CDC SET_LINE_CODING applies a landed DCP packet");
  internal_prep();
  s_terminal_read_call        = 1U;
  s_read_result               = k_ra8_ok;
  const ra8_usb_setup_t setup = {
    .bm_request_type = 0x21U,
    .b_request       = (uint8_t)k_ra8_cdc_req_set_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_ra8_cdc_line_coding_len,
  };

  TEST_ASSERT_EQ(k_ra8_ok, internal_dispatch_class_setup(&setup));
  TEST_ASSERT_EQ(115200U, s_state.coding.dte_rate);
  TEST_ASSERT_EQ(7U, s_state.coding.data_bits);
  TEST_ASSERT_EQ(1U, s_control_calls);
  TEST_ASSERT(s_last_ack);
  TEST_END("CDC SET_LINE_CODING applies a landed DCP packet");
}

/**
 * @test internal_test_private_copy_compound_vectors
 * @brief Cover the three public compound guards in the private CDC copy.
 * @par MC/DC:
 * - Init `(speed != FS) && (speed != HS)`: FS, HS, and invalid speed.
 * - Send `(data == nullptr) && (len != 0)`: buffer/nonzero,
 *   null/zero, and null/nonzero.
 * - SETUP `(type != iface) && (type != iface-in)`: iface, iface-in,
 *   and a standard request type.
 */
RA8_INTERNAL static void internal_test_private_copy_compound_vectors(void)
{
  TEST_BEGIN("CDC private copy covers init, send, and SETUP MC/DC vectors");

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init_data_stage_cov(k_ra8_usb_speed_fs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init_data_stage_cov(k_ra8_usb_speed_hs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_cdc_init_data_stage_cov((ra8_usb_speed_t)UINT8_MAX));

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init_data_stage_cov(k_ra8_usb_speed_fs));
  uint8_t         byte              = 0U;
  const ra8_err_t send_null_zero    = ra8_usb_cdc_send_data_stage_cov(nullptr, 0U);
  const ra8_err_t send_byte         = ra8_usb_cdc_send_data_stage_cov(&byte, 1U);
  const ra8_err_t send_null_nonzero = ra8_usb_cdc_send_data_stage_cov(nullptr, 1U);
  TEST_ASSERT(send_null_zero != k_ra8_err_invalid_arg);
  TEST_ASSERT(send_byte != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, send_null_nonzero);

  ra8_usb_setup_t setup = {
    .bm_request_type = k_ra8_cdc_bm_class_recip_iface,
    .b_request       = (uint8_t)k_ra8_cdc_req_set_control_line_state,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup_data_stage_cov(&setup));
  setup.bm_request_type = k_ra8_cdc_bm_class_recip_in;
  setup.b_request       = (uint8_t)k_ra8_cdc_req_get_line_coding;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup_data_stage_cov(&setup));
  setup.bm_request_type = UINT8_MAX;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_cdc_handle_setup_data_stage_cov(&setup));

  TEST_END("CDC private copy covers init, send, and SETUP MC/DC vectors");
}

int main(void)
{
  internal_test_apply_line_coding_vectors();
  internal_test_pull_data_stage_vectors();
  internal_test_set_line_coding_landed_packet();
  internal_test_private_copy_compound_vectors();
  return 0;
}
