/**
 * @file test_ra_usb_pprn.c
 * @brief Unit tests for the native USB device-side Printer class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_pprn.h"
#include "unity_minimal.h"

/* Sample minimal config descriptor blob. */
static const uint8_t s_sample_desc[] = {
  0x09,
  0x02,
  0x20,
  0x00,
  0x01,
  0x01,
  0x00,
  0x80,
  0x32,
  0x09,
  0x04,
  0x00,
  0x00,
  0x02,
  0x07,
  0x01,
  0x02,
  0x00,
};

/* Sample IEEE 1284 device-ID payload (USB Printer 1.1 sec 4.2.1). */
static const uint8_t s_sample_dev_id[] = {
  0x00, 0x1A, 'M', 'F', 'G', ':', 'R', 'e', 'n', 'e', 's', 'a',
  's',  ';',  'M', 'D', 'L', ':', 'R', 'A', '8', 'D', '2', ';',
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
  (void)ra_usb_pprn_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra_ok;
}

static void test_init_default_status(void)
{
  TEST_BEGIN("ra_usb_pprn_init seeds default port-status (online | not-error)");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_get_port_status(&status));
  /* Bit 4 (select) and bit 3 (not-error) must be set. */
  TEST_ASSERT((status & (1U << k_ra_pprn_status_bit_select)) != 0U);
  TEST_ASSERT((status & (1U << k_ra_pprn_status_bit_not_error)) != 0U);
  TEST_ASSERT((status & (1U << k_ra_pprn_status_bit_paper_empty)) == 0U);
  TEST_END("ra_usb_pprn_init seeds default port-status (online | not-error)");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_pprn_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pprn_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_pprn_init rejects bogus speed");
}

static void test_class_request_codes(void)
{
  TEST_BEGIN("Printer class request codes match USB Printer 1.1 spec");
  TEST_ASSERT_EQ((int32_t)0x00, (int32_t)k_ra_pprn_req_get_device_id);
  TEST_ASSERT_EQ((int32_t)0x01, (int32_t)k_ra_pprn_req_get_port_status);
  TEST_ASSERT_EQ((int32_t)0x02, (int32_t)k_ra_pprn_req_soft_reset);
  TEST_END("Printer class request codes match USB Printer 1.1 spec");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("PPRN API rejects calls before init");
  prep();

  uint8_t        buf[8] = {};
  uint16_t       got    = 0U;
  uint8_t        status = 0U;
  ra_usb_setup_t setup  = {};

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pprn_set_descriptors(s_sample_desc,
                                                      (uint16_t)sizeof(s_sample_desc),
                                                      s_sample_dev_id,
                                                      (uint16_t)sizeof(s_sample_dev_id)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_recv(buf, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_send(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_set_port_status(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_get_port_status(&status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pprn_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pprn_handle_setup(&setup));
  TEST_END("PPRN API rejects calls before init");
}

static void test_set_descriptors(void)
{
  TEST_BEGIN("ra_usb_pprn_set_descriptors validates pairing");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));

  /* desc NULL -> null_ptr. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_pprn_set_descriptors(nullptr, 1U, nullptr, 0U));

  /* desc_len 0 -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_pprn_set_descriptors(s_sample_desc, 0U, nullptr, 0U));

  /* device_id ptr set but len 0 -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_pprn_set_descriptors(s_sample_desc,
                                                      (uint16_t)sizeof(s_sample_desc),
                                                      s_sample_dev_id,
                                                      0U));

  /* Both NULL device-id is OK. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pprn_set_descriptors(s_sample_desc,
                                                      (uint16_t)sizeof(s_sample_desc),
                                                      nullptr,
                                                      0U));

  /* Both set is OK. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pprn_set_descriptors(s_sample_desc,
                                                      (uint16_t)sizeof(s_sample_desc),
                                                      s_sample_dev_id,
                                                      (uint16_t)sizeof(s_sample_dev_id)));
  TEST_END("ra_usb_pprn_set_descriptors validates pairing");
}

static void test_send_recv_validation(void)
{
  TEST_BEGIN("ra_usb_pprn_send / recv validate args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));

  uint8_t  buf[16] = {};
  uint16_t got     = 0U;

  /* recv: NULL buf / NULL got_len / max_len 0. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pprn_recv(nullptr, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pprn_recv(buf, 8U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pprn_recv(buf, 0U, &got));

  /* send: NULL data + len -> null_ptr; len 0 -> invalid_arg; len > 64 (FS bulk) -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pprn_send(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pprn_send(buf, 0U));

  uint8_t big[128] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_pprn_send(big, (uint16_t)sizeof(big)));
  TEST_END("ra_usb_pprn_send / recv validate args");
}

static void test_port_status_round_trip(void)
{
  TEST_BEGIN("ra_usb_pprn_set_port_status round-trips through get_port_status");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));

  /* Simulate paper-empty event. */
  const uint8_t paper_empty =
    (uint8_t)((1U << k_ra_pprn_status_bit_paper_empty) | (1U << k_ra_pprn_status_bit_select));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_set_port_status(paper_empty));

  uint8_t out = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_get_port_status(&out));
  TEST_ASSERT_EQ((int32_t)paper_empty, (int32_t)out);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pprn_get_port_status(nullptr));
  TEST_END("ra_usb_pprn_set_port_status round-trips through get_port_status");
}

static void test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra_usb_pprn_handle_setup dispatches GET_DEVICE_ID / SOFT_RESET to callback");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pprn_attach_setup_handler(test_setup_cb, nullptr));

  /* GET_DEVICE_ID. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0xA1U,
    .b_request       = (uint8_t)k_ra_pprn_req_get_device_id,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 64U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_ASSERT_EQ((int32_t)k_ra_pprn_req_get_device_id, (int32_t)s_setup_cb_last_breq);

  /* GET_PORT_STATUS. */
  setup.b_request = (uint8_t)k_ra_pprn_req_get_port_status;
  setup.w_length  = 1U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)2, s_setup_cb_calls);

  /* SOFT_RESET. */
  setup.bm_request_type = (uint8_t)0x21U;
  setup.b_request       = (uint8_t)k_ra_pprn_req_soft_reset;
  setup.w_length        = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)3, s_setup_cb_calls);
  TEST_END("ra_usb_pprn_handle_setup dispatches GET_DEVICE_ID / SOFT_RESET to callback");
}

static void test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra_usb_pprn_handle_setup rejects non-class / unknown / NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pprn_init(k_ra_usb_speed_fs));

  /* Standard envelope -> not_supported. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_pprn_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)0xA1U;
  setup.b_request       = (uint8_t)0x77U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_pprn_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pprn_handle_setup(nullptr));
  TEST_END("ra_usb_pprn_handle_setup rejects non-class / unknown / NULL");
}

int32_t main(void)
{
  test_init_default_status();
  test_init_bad_speed();
  test_class_request_codes();
  test_pre_init_calls();
  test_set_descriptors();
  test_send_recv_validation();
  test_port_status_round_trip();
  test_handle_setup_dispatch();
  test_handle_setup_rejects();
  (void)fprintf(stderr, "[OK ] test_ra_usb_pprn.c\n");
  return 0;
}
