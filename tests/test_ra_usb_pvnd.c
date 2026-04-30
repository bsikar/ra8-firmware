/**
 * @file test_ra_usb_pvnd.c
 * @brief Unit tests for the native USB device-side Vendor class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_pvnd.h"
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
  0xFF,
  0x00,
  0x00,
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
  (void)ra_usb_pvnd_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra_ok;
}

static void test_init_fs(void)
{
  TEST_BEGIN("ra_usb_pvnd_init succeeds on FS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_init(k_ra_usb_speed_fs));
  TEST_END("ra_usb_pvnd_init succeeds on FS");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_pvnd_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pvnd_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_pvnd_init rejects bogus speed");
}

static void test_class_code(void)
{
  TEST_BEGIN("Vendor class code matches USB-IF registry (0xFF)");
  TEST_ASSERT_EQ((int32_t)0xFF, (int32_t)k_ra_pvnd_class_vendor);
  TEST_ASSERT_EQ((int32_t)0xC0, (int32_t)k_ra_pvnd_bm_vendor_dev_in);
  TEST_ASSERT_EQ((int32_t)0x40, (int32_t)k_ra_pvnd_bm_vendor_dev_out);
  TEST_END("Vendor class code matches USB-IF registry (0xFF)");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("PVND API rejects calls before init");
  prep();

  uint8_t        buf[8] = {};
  uint16_t       got    = 0U;
  ra_usb_setup_t setup  = {};

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pvnd_close());
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_pvnd_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pvnd_send(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pvnd_recv(buf, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pvnd_handle_setup(&setup));
  TEST_END("PVND API rejects calls before init");
}

static void test_send_recv_validation(void)
{
  TEST_BEGIN("ra_usb_pvnd_send / recv validate args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_init(k_ra_usb_speed_fs));

  uint8_t  buf[16] = {};
  uint16_t got     = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pvnd_send(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pvnd_send(buf, 0U));
  uint8_t big[128] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_pvnd_send(big, (uint16_t)sizeof(big)));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pvnd_recv(nullptr, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pvnd_recv(buf, 8U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pvnd_recv(buf, 0U, &got));
  TEST_END("ra_usb_pvnd_send / recv validate args");
}

static void test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra_usb_pvnd_handle_setup forwards every vendor envelope to callback");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));

  /* Vendor | Device | In. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ra_pvnd_bm_vendor_dev_in,
    .b_request       = (uint8_t)0x42U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 4U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_ASSERT_EQ((int32_t)0x42, (int32_t)s_setup_cb_last_breq);

  /* Vendor | Interface | Out. */
  setup.bm_request_type = (uint8_t)k_ra_pvnd_bm_vendor_iface_out;
  setup.b_request       = (uint8_t)0x55U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)2, s_setup_cb_calls);

  /* Vendor | Endpoint | In. */
  setup.bm_request_type = (uint8_t)k_ra_pvnd_bm_vendor_ep_in;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)3, s_setup_cb_calls);
  TEST_END("ra_usb_pvnd_handle_setup forwards every vendor envelope to callback");
}

static void test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra_usb_pvnd_handle_setup rejects standard / class / NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_init(k_ra_usb_speed_fs));

  /* Standard envelope (type=0). */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_pvnd_handle_setup(&setup));

  /* Class envelope (type=1). */
  setup.bm_request_type = (uint8_t)0x21U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_pvnd_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pvnd_handle_setup(nullptr));
  TEST_END("ra_usb_pvnd_handle_setup rejects standard / class / NULL");
}

static void test_handle_setup_no_handler_stalls(void)
{
  TEST_BEGIN("ra_usb_pvnd_handle_setup stalls vendor SETUP when no callback registered");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_init(k_ra_usb_speed_fs));

  /* No setup callback installed. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ra_pvnd_bm_vendor_dev_out,
    .b_request       = (uint8_t)0x10U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  /* Class layer issues a STALL response, which itself returns ok. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)0, s_setup_cb_calls);
  TEST_END("ra_usb_pvnd_handle_setup stalls vendor SETUP when no callback registered");
}

int32_t main(void)
{
  test_init_fs();
  test_init_bad_speed();
  test_class_code();
  test_pre_init_calls();
  test_send_recv_validation();
  test_handle_setup_dispatch();
  test_handle_setup_rejects();
  test_handle_setup_no_handler_stalls();
  (void)fprintf(stderr, "[OK ] test_ra_usb_pvnd.c\n");
  return 0;
}
