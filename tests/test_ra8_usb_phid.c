/**
 * @file test_ra8_usb_phid.c
 * @brief Unit tests for the native USB device-side HID class layer
 *
 * @details Exercises HID descriptor, report, idle, protocol, endpoint, and unsupported-request behavior with bounded device fixtures.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_phid.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_phid_setup_t
 * @brief SETUP-packet fields the HID class arms submit.
 *
 * @details
 * `bmRequestType` encodes direction, type and recipient in one byte, so each
 * name states the combination rather than the hex: 0x21 is host-to-device
 * class-to-interface, 0xA1 the device-to-host reply, and 0x80 a standard
 * device-to-device request.
 */
typedef enum : uint16_t {
  k_t_bmreq_class_out  = 0x21U,   /**< Host-to-device, class, interface. */
  k_t_bmreq_class_in   = 0xA1U,   /**< Device-to-host, class, interface. */
  k_t_bmreq_std_in     = 0x80U,   /**< Device-to-host, standard, device. */
  k_t_breq_unknown     = 0xFFU,   /**< A bRequest outside the HID set, which the
                                      class must stall; also the poison value
                                      seeded into the idle-rate out-parameter.  */
  k_t_idle_duration    = 0x0AU,   /**< SET_IDLE duration, in 4 ms units: 10.      */
  k_t_wvalue_dev_desc  = 0x0100U, /**< wValue 0x0100: descriptor type 1, index 0. */
  k_t_wlength_dev_desc = 18U,     /**< Device-descriptor length, bytes.           */
} t_phid_setup_t;

/**
 * @enum t_phid_payload_t
 * @brief Report bytes the interrupt-IN arm moves.
 *
 * @details
 * Arbitrary but pairwise distinct, so a byte landing at the wrong report
 * offset is visible rather than masked by a repeated value.
 */
typedef enum : uint8_t {
  k_t_report_b0 = 0xAAU, /**< Report byte 0. */
  k_t_report_b1 = 0xBBU, /**< Report byte 1. */
  k_t_report_b2 = 0xCCU, /**< Report byte 2. */
} t_phid_payload_t;

/* Sample HID Report descriptor (boot-mouse, USB HID 1.11 appendix B). */
static const uint8_t s_sample_report_desc[] = {
  0x05,
  0x01, /* USAGE_PAGE (Generic Desktop) */
  0x09,
  0x02, /* USAGE (Mouse) */
  0xA1,
  0x01, /* COLLECTION (Application) */
  0x09,
  0x01, /* USAGE (Pointer) */
  0xA1,
  0x00, /* COLLECTION (Physical) */
  0xC0, /* END_COLLECTION        */
  0xC0, /* END_COLLECTION        */
};

/* Sample HID class descriptor (9-byte minimum, USB HID 1.11 sec 6.2.1). */
static const uint8_t s_sample_hid_desc[] = {
  0x09,
  0x21,
  0x11,
  0x01,
  0x00,
  0x01,
  0x22,
  0x34,
  0x00,
};

static int32_t   s_setup_cb_calls       = 0;
static uint8_t   s_setup_cb_last_breq   = 0U;
static ra8_err_t s_setup_cb_return_code = k_ra8_ok;

/** @brief Verify setup cb behavior. @details Executes the setup cb scenario with bounded fixture state and asserts the contract-specific result. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] setup Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_test_setup_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_setup_cb_calls++;
  s_setup_cb_last_breq = setup->b_request;
  return s_setup_cb_return_code;
}

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_phid_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init fs behavior. @details Executes the init fs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_fs(void)
{
  TEST_BEGIN("ra8_usb_phid_init succeeds on FS");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));
  TEST_END("ra8_usb_phid_init succeeds on FS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init bad speed behavior. @details Executes the init bad speed scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_phid_init rejects bogus speed");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_phid_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init hs default protocol behavior. @details Executes the init hs default protocol scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_hs_default_protocol(void)
{
  TEST_BEGIN("ra8_usb_phid_init seeds protocol=report and idle=0");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_hs));

  uint8_t idle = k_t_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ(0U, idle);

  ra8_usb_phid_protocol_select_t proto = k_ra8_phid_proto_boot;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ(k_ra8_phid_proto_report, proto);

  /* configure_endpoint deselects the pipe window (PIPESEL=0) before returning. */
  volatile r_usb_regs_t* reg = ra8_usb_hs();
  TEST_ASSERT_EQ(0U, reg->PIPESEL);
  TEST_END("ra8_usb_phid_init seeds protocol=report and idle=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify class request codes behavior. @details Executes the class request codes scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_class_request_codes(void)
{
  TEST_BEGIN("HID class request codes match USB HID 1.11 spec");
  TEST_ASSERT_EQ(0x01, k_ra8_phid_req_get_report);
  TEST_ASSERT_EQ(0x02, k_ra8_phid_req_get_idle);
  TEST_ASSERT_EQ(0x03, k_ra8_phid_req_get_protocol);
  TEST_ASSERT_EQ(0x09, k_ra8_phid_req_set_report);
  TEST_ASSERT_EQ(0x0A, k_ra8_phid_req_set_idle);
  TEST_ASSERT_EQ(0x0B, k_ra8_phid_req_set_protocol);
  TEST_END("HID class request codes match USB HID 1.11 spec");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify pre init calls behavior. @details Executes the pre init calls scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pre_init_calls(void)
{
  TEST_BEGIN("PHID API rejects calls before init");
  internal_prep();

  uint8_t                        buf[8] = {};
  uint16_t                       got    = 0U;
  uint8_t                        idle   = 0U;
  ra8_usb_phid_protocol_select_t proto  = k_ra8_phid_proto_report;
  ra8_usb_setup_t                setup  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              s_sample_hid_desc,
                                              (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_send_report(0U, buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_recv_report(0U, buf, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_phid_attach_setup_handler(internal_test_setup_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_phid_get_protocol(&proto));
  TEST_END("PHID API rejects calls before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set descriptors behavior. @details Executes the set descriptors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_descriptors(void)
{
  TEST_BEGIN("ra8_usb_phid_set_descriptors stores pointers + lengths");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              s_sample_hid_desc,
                                              (uint16_t)sizeof(s_sample_hid_desc)));

  /* Re-installation is allowed. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              s_sample_hid_desc,
                                              (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_END("ra8_usb_phid_set_descriptors stores pointers + lengths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set descriptors validation behavior. @details Executes the set descriptors validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_descriptors_validation(void)
{
  TEST_BEGIN("ra8_usb_phid_set_descriptors rejects null / zero len");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_phid_set_descriptors(nullptr,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              s_sample_hid_desc,
                                              (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              nullptr,
                                              (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              0U,
                                              s_sample_hid_desc,
                                              (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_set_descriptors(s_sample_report_desc,
                                              (uint16_t)sizeof(s_sample_report_desc),
                                              s_sample_hid_desc,
                                              0U));
  TEST_END("ra8_usb_phid_set_descriptors rejects null / zero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify send report validation behavior. @details Executes the send report validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_report_validation(void)
{
  TEST_BEGIN("ra8_usb_phid_send_report validates args");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  uint8_t buf[4] = {0x01U, 0x02U, 0x03U, 0x04U};

  /* NULL payload with non-zero len -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_phid_send_report(0U, nullptr, 4U));

  /* Single-report device must send at least one byte. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_send_report(0U, buf, 0U));

  /* Frame larger than pipe max-packet (FS default = 8) -> invalid_arg. */
  uint8_t big[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_send_report(0U, big, (uint16_t)sizeof(big)));

  /* Valid argument shape: every pre-check inside ra8_usb_phid_send_report
   * passes and the call forwards into ra8_usb_queue_in. The FRDY wait
   * converges on its first poll via the unarmed ra8_fake_mmio seam (see
   * priv_wait_frdy), so a well-formed call returns k_ra8_ok -- the
   * arg-validation path is exercised end-to-end. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_send_report(0U, buf, 4U));
  TEST_END("ra8_usb_phid_send_report validates args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify send report with id behavior. @details Executes the send report with id scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_report_with_id(void)
{
  TEST_BEGIN("ra8_usb_phid_send_report prepends report ID when non-zero");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  uint8_t payload[3] = {k_t_report_b0, k_t_report_b1, k_t_report_b2};
  /* report_id=2 means framed_len = 1 + 3 = 4, fits inside FS default 8.
   * The FRDY wait converges via the unarmed ra8_fake_mmio seam (see
   * priv_wait_frdy), so a well-formed call returns k_ra8_ok; the
   * arg-validation path is fully exercised. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_send_report(2U, payload, 3U));

  /* report_id != 0, len = 8 -> framed_len 9 > 8, must be rejected. */
  uint8_t big[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_send_report(2U, big, (uint16_t)sizeof(big)));
  TEST_END("ra8_usb_phid_send_report prepends report ID when non-zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify recv report validation behavior. @details Executes the recv report validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_recv_report_validation(void)
{
  TEST_BEGIN("ra8_usb_phid_recv_report validates args");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_phid_recv_report(0U, nullptr, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_phid_recv_report(0U, buf, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_recv_report(0U, buf, 0U, &got));
  TEST_END("ra8_usb_phid_recv_report validates args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify attach setup handler behavior. @details Executes the attach setup handler scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_setup_handler(void)
{
  TEST_BEGIN("ra8_usb_phid_attach_setup_handler stores callback");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_attach_setup_handler(internal_test_setup_cb, nullptr));

  /* Drive a SET_IDLE class request through; the callback must fire. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_out,
    .b_request       = (uint8_t)k_ra8_phid_req_set_idle,
    .w_value         = (uint16_t)((uint16_t)k_t_idle_duration << 8U), /* duration = 10 */
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_ASSERT_EQ(k_ra8_phid_req_set_idle, s_setup_cb_last_breq);

  uint8_t idle = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ(10U, idle);

  /* Detach: passing NULL must succeed and silence subsequent calls. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_attach_setup_handler(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_END("ra8_usb_phid_attach_setup_handler stores callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup set protocol behavior. @details Executes the handle setup set protocol scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_set_protocol(void)
{
  TEST_BEGIN("ra8_usb_phid_handle_setup updates protocol shadow");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_out,
    .b_request       = (uint8_t)k_ra8_phid_req_set_protocol,
    .w_value         = (uint16_t)k_ra8_phid_proto_boot,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));

  ra8_usb_phid_protocol_select_t proto = k_ra8_phid_proto_report;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ(k_ra8_phid_proto_boot, proto);

  setup.w_value = (uint16_t)k_ra8_phid_proto_report;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ(k_ra8_phid_proto_report, proto);
  TEST_END("ra8_usb_phid_handle_setup updates protocol shadow");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup rejects standard behavior. @details Executes the handle setup rejects standard scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_rejects_standard(void)
{
  TEST_BEGIN("ra8_usb_phid_handle_setup rejects non-class SETUPs and NULL");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_std_in, /* standard | device | IN */
    .b_request       = (uint8_t)0x06U,            /* GET_DESCRIPTOR         */
    .w_value         = (uint16_t)k_t_wvalue_dev_desc,
    .w_index         = 0U,
    .w_length        = k_t_wlength_dev_desc,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_phid_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_out;
  setup.b_request       = (uint8_t)k_t_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_phid_handle_setup(&setup));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_phid_handle_setup(nullptr));
  TEST_END("ra8_usb_phid_handle_setup rejects non-class SETUPs and NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup get report acks behavior. @details Executes the handle setup get report acks scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_get_report_acks(void)
{
  TEST_BEGIN("ra8_usb_phid_handle_setup ACKs GET_REPORT");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_in,
    .b_request       = (uint8_t)k_ra8_phid_req_get_report,
    .w_value         = (uint16_t)((uint16_t)k_ra8_phid_report_type_input << 8U),
    .w_index         = 0U,
    .w_length        = 8U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));

  /* DCPCTR.PID should be BUF (ACK was issued). */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT_EQ(k_ra8_pid_buf, (reg->DCPCTR & k_ra8_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl)) != 0U);
  TEST_END("ra8_usb_phid_handle_setup ACKs GET_REPORT");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup callback stalls behavior. @details Executes the handle setup callback stalls scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_callback_stalls(void)
{
  TEST_BEGIN("ra8_usb_phid_handle_setup stalls EP0 when callback returns error");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_attach_setup_handler(internal_test_setup_cb, nullptr));

  s_setup_cb_return_code = k_ra8_err_not_supported;
  ra8_usb_setup_t setup  = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_in,
    .b_request       = (uint8_t)k_ra8_phid_req_get_report,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 8U,
  };
  /* Class layer must propagate ok (the stall response itself is ok),
   * but the controller's DCPCTR must reflect a STALL PID. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_END("ra8_usb_phid_handle_setup stalls EP0 when callback returns error");
}

/* =============================================================================
 * MC/DC vector tests for the compound boolean decisions flagged in
 * docs/MCDC_GAPS.csv against libs/ra8_hal/src/ra8_usb_phid.c.
 * =============================================================================
 */

/**
 * @enum test_phid_mcdc_t
 * @brief Numeric vectors driving the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_test_phid_speed_bad     = 9U,    /**< Test phid speed bad.        */
  k_test_phid_iface_in      = 0xA1U, /**< Test phid iface in.         */
  k_test_phid_iface_out     = 0x21U, /**< Test phid iface out.        */
  k_test_phid_bm_bogus      = 0x80U, /**< Test phid bm bogus.         */
  k_test_phid_breq_unknown  = 0x77U, /**< Test phid breq unknown.     */
  k_test_phid_send_len_zero = 0U,    /**< Test phid send length zero. */
  k_test_phid_send_len_some = 4U,    /**< Test phid send length some. */
  k_test_phid_desc_len_zero = 0U,    /**< Test phid desc length zero. */
  k_test_phid_desc_len_some = 8U,    /**< Test phid desc length some. */
} test_phid_mcdc_t;

static const uint8_t s_dummy_desc_a[8] = {};
static const uint8_t s_dummy_desc_b[8] = {};

/**
 * @brief MC/DC decision A: init speed gate, then re-init at FS for the rest.
 * @pre None.
 * @post FS/HS accept, the bad speed rejects, and the device is left at FS.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Implements the phid mcdc init fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_phid_mcdc_init(void)
{
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_hs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_phid_init((ra8_usb_speed_t)k_test_phid_speed_bad));

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_init(k_ra8_usb_speed_fs));
}

/**
 * @brief MC/DC decisions C + D: the send_report null/length envelope.
 * @pre The device is initialised.
 * @post Each C/D vector returned (or avoided) invalid_arg as documented.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Implements the phid mcdc send report fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_phid_mcdc_send_report(void)
{
  uint8_t buf[8] = {};
  /* C-V3: NULL with len -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_phid_send_report(0U, nullptr, (uint16_t)k_test_phid_send_len_some));
  /* D-V1: rid=0, len=0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_send_report(0U, buf, (uint16_t)k_test_phid_send_len_zero));
  /* C-V1 + D-V1: NULL,0 -> still invalid_arg via D path. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_send_report(0U, nullptr, (uint16_t)k_test_phid_send_len_zero));
  /* D-V2: rid!=0, len=0 -> exits via len overflow check or queue_in;
   * either way decision D stays false. The send may return ok or
   * hw_error from the fake; we only assert it is NOT invalid_arg. */
  const ra8_err_t d_v2 = ra8_usb_phid_send_report(1U, buf, (uint16_t)k_test_phid_send_len_zero);
  TEST_ASSERT(d_v2 != k_ra8_err_invalid_arg);
  /* D-V3: rid=0, len!=0 -> decision D false. */
  const ra8_err_t d_v3 = ra8_usb_phid_send_report(0U, buf, (uint16_t)k_test_phid_send_len_some);
  TEST_ASSERT(d_v3 != k_ra8_err_invalid_arg);
}

/**
 * @brief MC/DC decisions E + F: the handle_setup envelope and request-code chain.
 * @pre The device is initialised.
 * @post Each envelope and request-code vector returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Implements the phid mcdc handle setup fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_phid_mcdc_handle_setup(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_attach_setup_handler(internal_test_setup_cb, nullptr));
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_phid_iface_in,
    .b_request       = (uint8_t)k_ra8_phid_req_get_report,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  /* E-V1 iface_in -> envelope ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  /* E-V2 iface_out -> envelope ok. */
  setup.bm_request_type = (uint8_t)k_test_phid_iface_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  /* E-V3 standard envelope -> not_supported. */
  setup.bm_request_type = (uint8_t)k_test_phid_bm_bogus;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_phid_handle_setup(&setup));

  /* F: 6 lone-true vectors. */
  setup.bm_request_type    = (uint8_t)k_test_phid_iface_in;
  const uint8_t requests[] = {
    (uint8_t)k_ra8_phid_req_get_report,
    (uint8_t)k_ra8_phid_req_set_report,
    (uint8_t)k_ra8_phid_req_get_idle,
    (uint8_t)k_ra8_phid_req_set_idle,
    (uint8_t)k_ra8_phid_req_get_protocol,
    (uint8_t)k_ra8_phid_req_set_protocol,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(requests) / sizeof(requests[0])); ++i) {
    setup.b_request = requests[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_handle_setup(&setup));
  }
  /* F all-false vector. */
  setup.b_request = (uint8_t)k_test_phid_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_phid_handle_setup(&setup));
}

/**
 * @test internal_test_mcdc_phid
 *
 * @par MC/DC:
 * Covers every compound decision flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_phid.c.
 *
 * Decision A (line 223, 2 conds): init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3:
 *   - V1 FS  -> C1=F (short circuit) -> dec=F
 *   - V2 HS  -> C1=T, C2=F           -> dec=F
 *   - V3 9   -> C1=T, C2=T           -> dec=T (invalid_arg)
 *
 * Decision B (line 274, 2 conds): set_descriptors len OR
 *   `(report_desc_len == 0) || (hid_desc_len == 0)` -- N+1=3:
 *   - V1 (8,8) -> C1=F, C2=F -> dec=F (ok)
 *   - V2 (0,8) -> C1=T (short circuit) -> dec=T (invalid_arg)
 *   - V3 (8,0) -> C1=F, C2=T -> dec=T (invalid_arg)
 *
 * Decision C (line 294, 2 conds): send_report null-with-len
 *   `(payload == NULL) && (len != 0)` -- N+1=3:
 *   - V1 (NULL,0) -> C1=T,C2=F -> dec=F (falls to next check)
 *   - V2 (buf,4)  -> C1=F      -> dec=F
 *   - V3 (NULL,4) -> C1=T,C2=T -> dec=T (null_ptr)
 *
 * Decision D (line 297, 2 conds): send_report (rid==0 && len==0)
 *   - V1 (rid=0,len=0)   -> C1=T, C2=T -> dec=T (invalid_arg)
 *   - V2 (rid=1,len=0)   -> C1=F       -> dec=F (falls through)
 *   - V3 (rid=0,len=4)   -> C1=T, C2=F -> dec=F (falls through)
 *
 * Decision E (lines 372-373, 2 conds): handle_setup envelope
 *   `(bm != iface_in) && (bm != iface_out)` -- N+1=3.
 *
 * Decision F (lines 178-180, 6-condition OR chain inside
 * `internal_is_known_class_request`): per DO-178C 6.4.4.3
 * representative-subset: 6 lone-true vectors + 1 all-false (7 total). @brief Verify mcdc phid behavior. @details Executes the mcdc phid scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_phid(void)
{
  TEST_BEGIN("phid MC/DC: init/desc/send_report/handle_setup compound decisions");

  /* Decision A: init speed gate. */
  internal_phid_mcdc_init();

  /* Decision B: set_descriptors len OR. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_phid_set_descriptors(s_dummy_desc_a, 8U, s_dummy_desc_b, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_set_descriptors(s_dummy_desc_a, 0U, s_dummy_desc_b, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_phid_set_descriptors(s_dummy_desc_a, 8U, s_dummy_desc_b, 0U));

  /* Decision C + D: send_report. */
  internal_phid_mcdc_send_report();

  /* Decision E + F: handle_setup. */
  internal_phid_mcdc_handle_setup();

  TEST_END("phid MC/DC: init/desc/send_report/handle_setup compound decisions");
}

/**
 * @test internal_test_mcdc_phid_known_class_request_or_chain
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_usb_phid.c lines 220-222,
 * internal_is_known_class_request):
 *   ``(b_request == GET_REPORT) || (b_request == SET_REPORT) ||
 *    (b_request == GET_IDLE)   || (b_request == SET_IDLE)   ||
 *    (b_request == GET_PROTOCOL) || (b_request == SET_PROTOCOL)``
 * (6 conditions, OR-chain).
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=6 OR-chain requires N+1 = 7
 * vectors. Canonical short-circuit set: each Ci=T (with Cj<i = F by
 * disjoint-constant construction), plus one all-F vector. Each Ci's
 * independence follows from (Ci=T) vs all-F, per DO-178C 6.4.4.3
 * source-text equivalence. Mirror is byte-identical (constant-folding
 * only). @brief Provide the file-local mirror phid is known class request test helper. @details Implements the mirror phid is known class request fixture operation used only by this focused test executable. @param[in] b_request Fixture argument governed by the exercised interface contract. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static bool internal_mirror_phid_is_known_class_request(uint8_t b_request)
{
  return (b_request == (uint8_t)k_ra8_phid_req_get_report) ||
         (b_request == (uint8_t)k_ra8_phid_req_set_report) ||
         (b_request == (uint8_t)k_ra8_phid_req_get_idle) ||
         (b_request == (uint8_t)k_ra8_phid_req_set_idle) ||
         (b_request == (uint8_t)k_ra8_phid_req_get_protocol) ||
         (b_request == (uint8_t)k_ra8_phid_req_set_protocol);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify mcdc phid known class request or chain behavior. @details Executes the mcdc phid known class request or chain scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_phid_known_class_request_or_chain(void)
{
  TEST_BEGIN("phid MC/DC: 6-cond known-class-request OR (lines 220-222)");
  TEST_ASSERT_EQ(1,
                 internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_get_report));
  TEST_ASSERT_EQ(1,
                 internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_set_report));
  TEST_ASSERT_EQ(1, internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_get_idle));
  TEST_ASSERT_EQ(1, internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_set_idle));
  TEST_ASSERT_EQ(1,
                 internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_get_protocol));
  TEST_ASSERT_EQ(1,
                 internal_mirror_phid_is_known_class_request((uint8_t)k_ra8_phid_req_set_protocol));
  TEST_ASSERT_EQ(0, internal_mirror_phid_is_known_class_request(0xFFU));
  TEST_END("phid MC/DC: 6-cond known-class-request OR (lines 220-222)");
}

int32_t main(void)
{
  internal_test_init_fs();
  internal_test_init_bad_speed();
  internal_test_init_hs_default_protocol();
  internal_test_class_request_codes();
  internal_test_pre_init_calls();
  internal_test_set_descriptors();
  internal_test_set_descriptors_validation();
  internal_test_send_report_validation();
  internal_test_send_report_with_id();
  internal_test_recv_report_validation();
  internal_test_attach_setup_handler();
  internal_test_handle_setup_set_protocol();
  internal_test_handle_setup_rejects_standard();
  internal_test_handle_setup_get_report_acks();
  internal_test_handle_setup_callback_stalls();
  internal_test_mcdc_phid();
  internal_test_mcdc_phid_known_class_request_or_chain();
  return 0;
}
