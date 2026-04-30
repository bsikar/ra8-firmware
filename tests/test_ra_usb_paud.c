/**
 * @file test_ra_usb_paud.c
 * @brief Unit tests for the native USB device-side Audio (UAC1) class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_paud.h"
#include "unity_minimal.h"

/* Sample minimal UAC1 descriptor blob (just header + interface stubs). */
static const uint8_t s_sample_desc[] = {
  0x09,
  0x04,
  0x00,
  0x00,
  0x00,
  0x01,
  0x01,
  0x00,
  0x00, /* AC interface */
  0x09,
  0x24,
  0x01,
  0x00,
  0x01,
  0x1E,
  0x00,
  0x01,
  0x01, /* AC header    */
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
  (void)ra_usb_paud_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra_ok;
}

static void test_init_default_format(void)
{
  TEST_BEGIN("ra_usb_paud_init seeds 48 kHz / stereo / 16-bit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));

  ra_usb_paud_format_t fmt = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_get_format(&fmt));
  TEST_ASSERT_EQ((int32_t)48000, (int32_t)fmt.sample_rate_hz);
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)fmt.channels);
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)fmt.bytes_per_sample);

  int16_t vol = 0x55;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_get_volume(&vol));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)vol);
  TEST_END("ra_usb_paud_init seeds 48 kHz / stereo / 16-bit");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_paud_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_paud_init rejects bogus speed");
}

static void test_class_request_codes(void)
{
  TEST_BEGIN("Audio class request codes match USB Audio 1.0 spec sec A.9");
  TEST_ASSERT_EQ((int32_t)0x01, (int32_t)k_ra_paud_req_set_cur);
  TEST_ASSERT_EQ((int32_t)0x81, (int32_t)k_ra_paud_req_get_cur);
  TEST_ASSERT_EQ((int32_t)0x82, (int32_t)k_ra_paud_req_get_min);
  TEST_ASSERT_EQ((int32_t)0x83, (int32_t)k_ra_paud_req_get_max);
  TEST_ASSERT_EQ((int32_t)0xFF, (int32_t)k_ra_paud_req_get_stat);
  TEST_END("Audio class request codes match USB Audio 1.0 spec sec A.9");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("PAUD API rejects calls before init");
  prep();

  uint8_t              buf[8] = {};
  uint16_t             got    = 0U;
  ra_usb_paud_format_t fmt    = {};
  int16_t              vol    = 0;
  ra_usb_setup_t       setup  = {};

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_close());
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_paud_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_send_frame(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_recv_frame(buf, 8U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_set_format(fmt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_get_format(&fmt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_set_volume(0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_get_volume(&vol));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_paud_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_paud_handle_setup(&setup));
  TEST_END("PAUD API rejects calls before init");
}

static void test_set_format_validation(void)
{
  TEST_BEGIN("ra_usb_paud_set_format validates triplet");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));

  ra_usb_paud_format_t bad = {.sample_rate_hz = 0U, .channels = 2U, .bytes_per_sample = 2U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_set_format(bad));

  bad.sample_rate_hz = 44100U;
  bad.channels       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_set_format(bad));

  bad.channels         = 2U;
  bad.bytes_per_sample = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_set_format(bad));

  bad.bytes_per_sample = 5U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_set_format(bad));

  ra_usb_paud_format_t good = {.sample_rate_hz = 96000U, .channels = 1U, .bytes_per_sample = 3U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_set_format(good));

  ra_usb_paud_format_t out = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_get_format(&out));
  TEST_ASSERT_EQ((int32_t)96000, (int32_t)out.sample_rate_hz);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)out.channels);
  TEST_ASSERT_EQ((int32_t)3U, (int32_t)out.bytes_per_sample);
  TEST_END("ra_usb_paud_set_format validates triplet");
}

static void test_send_frame_validation(void)
{
  TEST_BEGIN("ra_usb_paud_send_frame validates args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));

  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_paud_send_frame(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_send_frame(buf, 0U));
  /* FS default iso ceiling = 192 bytes, 1024 should be rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_paud_send_frame(buf, 1024U));
  TEST_END("ra_usb_paud_send_frame validates args");
}

static void test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra_usb_paud_handle_setup dispatches SET_CUR to callback");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_paud_attach_setup_handler(test_setup_cb, nullptr));

  /* SET_CUR(volume) on the feature unit. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x21U,
    .b_request       = (uint8_t)k_ra_paud_req_set_cur,
    .w_value         = (uint16_t)((uint16_t)k_ra_paud_ctl_volume << 8U),
    .w_index         = 0U,
    .w_length        = 2U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)1, s_setup_cb_calls);
  TEST_ASSERT_EQ((int32_t)k_ra_paud_req_set_cur, (int32_t)s_setup_cb_last_breq);

  /* GET_CUR(sampling-rate) on iso EP. */
  setup.bm_request_type = (uint8_t)0xA2U;
  setup.b_request       = (uint8_t)k_ra_paud_req_get_cur;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)2, s_setup_cb_calls);
  TEST_END("ra_usb_paud_handle_setup dispatches SET_CUR to callback");
}

static void test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra_usb_paud_handle_setup rejects non-class / unknown / NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));

  /* Standard envelope -> not_supported. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_paud_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)0x21U;
  setup.b_request       = (uint8_t)0x77U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_paud_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_paud_handle_setup(nullptr));
  TEST_END("ra_usb_paud_handle_setup rejects non-class / unknown / NULL");
}

static void test_volume_shadow(void)
{
  TEST_BEGIN("ra_usb_paud_set_volume / get_volume round-trip");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_set_volume((int16_t)-2048));
  int16_t out = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_paud_get_volume(&out));
  TEST_ASSERT_EQ((int32_t)-2048, (int32_t)out);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_paud_get_volume(nullptr));
  TEST_END("ra_usb_paud_set_volume / get_volume round-trip");
}

int32_t main(void)
{
  test_init_default_format();
  test_init_bad_speed();
  test_class_request_codes();
  test_pre_init_calls();
  test_set_format_validation();
  test_send_frame_validation();
  test_handle_setup_dispatch();
  test_handle_setup_rejects();
  test_volume_shadow();
  (void)fprintf(stderr, "[OK ] test_ra_usb_paud.c\n");
  return 0;
}
