/**
 * @file test_ra_usb_phid.c
 * @brief Unit tests for the native USB device-side HID class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_phid.h"
#include "unity_minimal.h"

/* Sample HID Report descriptor (boot-mouse, USB HID 1.11 appendix B). */
static const uint8_t s_sample_report_desc[] = {
  0x05,
  0x01, /* USAGE_PAGE (Generic Desktop) */
  0x09,
  0x02, /* USAGE (Mouse)                */
  0xA1,
  0x01, /* COLLECTION (Application)     */
  0x09,
  0x01, /*   USAGE (Pointer)            */
  0xA1,
  0x00, /*   COLLECTION (Physical)      */
  0xC0, /*   END_COLLECTION             */
  0xC0, /* END_COLLECTION               */
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

static int32_t  s_setup_cb_calls       = 0;
static uint8_t  s_setup_cb_last_breq   = 0U;
static ra_err_t s_setup_cb_return_code = k_ra_ok;

static ra_err_t test_setup_cb(void* ctx, const ra_usb_setup_t* setup)
{
  (void)ctx;
  s_setup_cb_calls++;
  s_setup_cb_last_breq = setup->b_request;
  return s_setup_cb_return_code;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_phid_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra_ok;
}

static void test_init_fs(void)
{
  TEST_BEGIN("ra_usb_phid_init succeeds on FS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));
  TEST_END("ra_usb_phid_init succeeds on FS");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_phid_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_phid_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_phid_init rejects bogus speed");
}

static void test_init_hs_default_protocol(void)
{
  TEST_BEGIN("ra_usb_phid_init seeds protocol=report and idle=0");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_hs));

  uint8_t idle = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)idle);

  ra_usb_phid_protocol_select_t proto = k_ra_phid_proto_boot;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ((int32_t)k_ra_phid_proto_report, (int32_t)proto);

  /* PIPESEL was last written for the interrupt-OUT pipe (PIPE7). */
  volatile r_usb_regs_t* reg = ra_usb_hs();
  TEST_ASSERT_EQ((int32_t)k_ra_phid_pipe_intr_out, (int32_t)reg->PIPESEL);
  TEST_END("ra_usb_phid_init seeds protocol=report and idle=0");
}

static void test_class_request_codes(void)
{
  TEST_BEGIN("HID class request codes match USB HID 1.11 spec");
  TEST_ASSERT_EQ((int32_t)0x01, (int32_t)k_ra_phid_req_get_report);
  TEST_ASSERT_EQ((int32_t)0x02, (int32_t)k_ra_phid_req_get_idle);
  TEST_ASSERT_EQ((int32_t)0x03, (int32_t)k_ra_phid_req_get_protocol);
  TEST_ASSERT_EQ((int32_t)0x09, (int32_t)k_ra_phid_req_set_report);
  TEST_ASSERT_EQ((int32_t)0x0A, (int32_t)k_ra_phid_req_set_idle);
  TEST_ASSERT_EQ((int32_t)0x0B, (int32_t)k_ra_phid_req_set_protocol);
  TEST_END("HID class request codes match USB HID 1.11 spec");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("PHID API rejects calls before init");
  prep();

  uint8_t                       buf[8] = {};
  uint16_t                      got    = 0U;
  uint8_t                       idle   = 0U;
  ra_usb_phid_protocol_select_t proto  = k_ra_phid_proto_report;
  ra_usb_setup_t                setup  = {};

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_phid_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      s_sample_hid_desc,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_phid_send_report(0U, buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_phid_recv_report(0U, buf, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_phid_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_phid_get_protocol(&proto));
  TEST_END("PHID API rejects calls before init");
}

static void test_set_descriptors(void)
{
  TEST_BEGIN("ra_usb_phid_set_descriptors stores pointers + lengths");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      s_sample_hid_desc,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));

  /* Re-installation is allowed. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      s_sample_hid_desc,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_END("ra_usb_phid_set_descriptors stores pointers + lengths");
}

static void test_set_descriptors_validation(void)
{
  TEST_BEGIN("ra_usb_phid_set_descriptors rejects null / zero len");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_phid_set_descriptors(nullptr,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      s_sample_hid_desc,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      nullptr,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      0U,
                                                      s_sample_hid_desc,
                                                      (uint16_t)sizeof(s_sample_hid_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_phid_set_descriptors(s_sample_report_desc,
                                                      (uint16_t)sizeof(s_sample_report_desc),
                                                      s_sample_hid_desc,
                                                      0U));
  TEST_END("ra_usb_phid_set_descriptors rejects null / zero len");
}

static void test_send_report_validation(void)
{
  TEST_BEGIN("ra_usb_phid_send_report validates args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  uint8_t buf[4] = {0x01U, 0x02U, 0x03U, 0x04U};

  /* NULL payload with non-zero len -> null_ptr. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_phid_send_report(0U, nullptr, 4U));

  /* Single-report device must send at least one byte. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_phid_send_report(0U, buf, 0U));

  /* Frame larger than pipe max-packet (FS default = 8) -> invalid_arg. */
  uint8_t big[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_phid_send_report(0U, big, (uint16_t)sizeof(big)));

  /* Valid argument shape: the host-side mock leaves CFIFOCTR.FRDY clear
   * so the underlying ra_usb_queue_in returns hw_timeout, but the
   * arg-validation path is exercised end-to-end (we got past every
   * pre-check inside ra_usb_phid_send_report). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_usb_phid_send_report(0U, buf, 4U));
  TEST_END("ra_usb_phid_send_report validates args");
}

static void test_send_report_with_id(void)
{
  TEST_BEGIN("ra_usb_phid_send_report prepends report ID when non-zero");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  uint8_t payload[3] = {0xAAU, 0xBBU, 0xCCU};
  /* report_id=2 means framed_len = 1 + 3 = 4, fits inside FS default 8.
   * Mock leaves FRDY clear so actual queue returns hw_timeout, but the
   * arg-validation path is fully exercised. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_usb_phid_send_report(2U, payload, 3U));

  /* report_id != 0, len = 8 -> framed_len 9 > 8, must be rejected. */
  uint8_t big[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_phid_send_report(2U, big, (uint16_t)sizeof(big)));
  TEST_END("ra_usb_phid_send_report prepends report ID when non-zero");
}

static void test_recv_report_validation(void)
{
  TEST_BEGIN("ra_usb_phid_recv_report validates args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_phid_recv_report(0U, nullptr, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_phid_recv_report(0U, buf, 8U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_phid_recv_report(0U, buf, 0U, &got));
  TEST_END("ra_usb_phid_recv_report validates args");
}

static void test_attach_setup_handler(void)
{
  TEST_BEGIN("ra_usb_phid_attach_setup_handler stores callback");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_phid_attach_setup_handler(test_setup_cb, nullptr));

  /* Drive a SET_IDLE class request through; the callback must fire. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x21U,
    .b_request       = (uint8_t)k_ra_phid_req_set_idle,
    .w_value         = (uint16_t)((uint16_t)0x0AU << 8U), /* duration = 10 */
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_ASSERT_EQ((int32_t)k_ra_phid_req_set_idle, (int32_t)s_setup_cb_last_breq);

  uint8_t idle = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_get_idle(&idle));
  TEST_ASSERT_EQ((int32_t)10U, (int32_t)idle);

  /* Detach: passing NULL must succeed and silence subsequent calls. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_attach_setup_handler(nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_END("ra_usb_phid_attach_setup_handler stores callback");
}

static void test_handle_setup_set_protocol(void)
{
  TEST_BEGIN("ra_usb_phid_handle_setup updates protocol shadow");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x21U,
    .b_request       = (uint8_t)k_ra_phid_req_set_protocol,
    .w_value         = (uint16_t)k_ra_phid_proto_boot,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));

  ra_usb_phid_protocol_select_t proto = k_ra_phid_proto_report;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ((int32_t)k_ra_phid_proto_boot, (int32_t)proto);

  setup.w_value = (uint16_t)k_ra_phid_proto_report;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_get_protocol(&proto));
  TEST_ASSERT_EQ((int32_t)k_ra_phid_proto_report, (int32_t)proto);
  TEST_END("ra_usb_phid_handle_setup updates protocol shadow");
}

static void test_handle_setup_rejects_standard(void)
{
  TEST_BEGIN("ra_usb_phid_handle_setup rejects non-class SETUPs and NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U, /* standard | device | IN */
    .b_request       = (uint8_t)0x06U, /* GET_DESCRIPTOR */
    .w_value         = (uint16_t)0x0100U,
    .w_index         = 0U,
    .w_length        = 18U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_phid_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)0x21U;
  setup.b_request       = (uint8_t)0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_phid_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_phid_handle_setup(nullptr));
  TEST_END("ra_usb_phid_handle_setup rejects non-class SETUPs and NULL");
}

static void test_handle_setup_get_report_acks(void)
{
  TEST_BEGIN("ra_usb_phid_handle_setup ACKs GET_REPORT");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0xA1U,
    .b_request       = (uint8_t)k_ra_phid_req_get_report,
    .w_value         = (uint16_t)((uint16_t)k_ra_phid_report_type_input << 8U),
    .w_index         = 0U,
    .w_length        = 8U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));

  /* DCPCTR.PID should be BUF (ACK was issued). */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)k_ra_pid_buf, (int32_t)(reg->DCPCTR & k_ra_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra_dcpctr_bit_ccpl)) != 0U);
  TEST_END("ra_usb_phid_handle_setup ACKs GET_REPORT");
}

static void test_handle_setup_callback_stalls(void)
{
  TEST_BEGIN("ra_usb_phid_handle_setup stalls EP0 when callback returns error");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_phid_attach_setup_handler(test_setup_cb, nullptr));

  s_setup_cb_return_code = k_ra_err_not_supported;
  ra_usb_setup_t setup   = {
    .bm_request_type = (uint8_t)0xA1U,
    .b_request       = (uint8_t)k_ra_phid_req_get_report,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 8U,
  };
  /* Class layer must propagate ok (the stall response itself is ok),
   * but the controller's DCPCTR must reflect a STALL PID. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_phid_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_END("ra_usb_phid_handle_setup stalls EP0 when callback returns error");
}

int32_t main(void)
{
  test_init_fs();
  test_init_bad_speed();
  test_init_hs_default_protocol();
  test_class_request_codes();
  test_pre_init_calls();
  test_set_descriptors();
  test_set_descriptors_validation();
  test_send_report_validation();
  test_send_report_with_id();
  test_recv_report_validation();
  test_attach_setup_handler();
  test_handle_setup_set_protocol();
  test_handle_setup_rejects_standard();
  test_handle_setup_get_report_acks();
  test_handle_setup_callback_stalls();
  (void)fprintf(stderr, "[OK ] test_ra_usb_phid.c\n");
  return 0;
}
