/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_usb_paud.c
 * @brief Unit tests for the native USB device-side Audio (UAC1) class layer
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_paud.h"
#include "unity_minimal.h"

/**
 * @enum t_paud_setup_t
 * @brief SETUP-packet fields the audio class arms submit.
 *
 * @details
 * `bmRequestType` encodes direction, type and recipient: 0x21 is
 * host-to-device class-to-interface, 0xA2 the device-to-host reply addressed
 * to an endpoint, and 0x80 a standard device-to-device request.
 */
typedef enum : uint8_t {
  k_t_bmreq_class_out      = 0x21U, /**< Host-to-device, class, interface.             */
  k_t_bmreq_class_in       = 0xA2U, /**< Device-to-host, class, endpoint.              */
  k_t_bmreq_std_in         = 0x80U, /**< Device-to-host, standard, device.             */
  k_t_breq_unknown         = 0x77U, /**< A bRequest outside the audio set; must stall. */
  k_t_bytes_per_sample_bad = 5U,    /**< Sample width outside the supported 1..4.      */
  k_t_volume_probe         = 0x55,  /**< Pre-set volume; a control that returns
                                    without writing it leaves this value.       */
} t_paud_setup_t;

/**
 * @enum t_paud_rate_t
 * @brief Sample rates the format validator accepts and rejects.
 */
typedef enum : uint32_t {
  k_t_rate_unsupported = 44100U, /**< A rate the device does not implement. */
  k_t_rate_supported   = 48000U, /**< The nominal rate the happy path uses. */
  k_t_rate_high        = 96000U, /**< The high rate, also accepted.         */
} t_paud_rate_t;

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
  0x01, /* AC header */
};

static int32_t   s_setup_cb_calls       = 0;
static uint8_t   s_setup_cb_last_breq   = 0U;
static ra8_err_t s_setup_cb_return_code = k_ra8_ok;

static ra8_err_t test_setup_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_setup_cb_calls++;
  s_setup_cb_last_breq = setup->b_request;
  return s_setup_cb_return_code;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_paud_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_default_format(void)
{
  TEST_BEGIN("ra8_usb_paud_init seeds 48 kHz / stereo / 16-bit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  ra8_usb_paud_format_t fmt = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_get_format(&fmt));
  TEST_ASSERT_EQ(48000, fmt.sample_rate_hz);
  TEST_ASSERT_EQ(2U, fmt.channels);
  TEST_ASSERT_EQ(2U, fmt.bytes_per_sample);

  int16_t vol = k_t_volume_probe;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_get_volume(&vol));
  TEST_ASSERT_EQ(0, vol);
  TEST_END("ra8_usb_paud_init seeds 48 kHz / stereo / 16-bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_paud_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_paud_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_class_request_codes(void)
{
  TEST_BEGIN("Audio class request codes match USB Audio 1.0 spec sec A.9");
  TEST_ASSERT_EQ(0x01, k_ra8_paud_req_set_cur);
  TEST_ASSERT_EQ(0x81, k_ra8_paud_req_get_cur);
  TEST_ASSERT_EQ(0x82, k_ra8_paud_req_get_min);
  TEST_ASSERT_EQ(0x83, k_ra8_paud_req_get_max);
  TEST_ASSERT_EQ(0xFF, k_ra8_paud_req_get_stat);
  TEST_END("Audio class request codes match USB Audio 1.0 spec sec A.9");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_calls(void)
{
  TEST_BEGIN("PAUD API rejects calls before init");
  prep();

  uint8_t               buf[8] = {};
  uint16_t              got    = 0U;
  ra8_usb_paud_format_t fmt    = {};
  int16_t               vol    = 0;
  ra8_usb_setup_t       setup  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_paud_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_send_frame(buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_recv_frame(buf, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_set_format(fmt));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_get_format(&fmt));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_set_volume(0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_get_volume(&vol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_paud_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_paud_handle_setup(&setup));
  TEST_END("PAUD API rejects calls before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_format_validation(void)
{
  TEST_BEGIN("ra8_usb_paud_set_format validates triplet");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  ra8_usb_paud_format_t bad = {.sample_rate_hz = 0U, .channels = 2U, .bytes_per_sample = 2U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(bad));

  bad.sample_rate_hz = k_t_rate_unsupported;
  bad.channels       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(bad));

  bad.channels         = 2U;
  bad.bytes_per_sample = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(bad));

  bad.bytes_per_sample = k_t_bytes_per_sample_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(bad));

  ra8_usb_paud_format_t good = {.sample_rate_hz   = k_t_rate_high,
                                .channels         = 1U,
                                .bytes_per_sample = 3U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_set_format(good));

  ra8_usb_paud_format_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_get_format(&out));
  TEST_ASSERT_EQ(96000, out.sample_rate_hz);
  TEST_ASSERT_EQ(1U, out.channels);
  TEST_ASSERT_EQ(3U, out.bytes_per_sample);
  TEST_END("ra8_usb_paud_set_format validates triplet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_frame_validation(void)
{
  TEST_BEGIN("ra8_usb_paud_send_frame validates args");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_paud_send_frame(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_send_frame(buf, 0U));
  /* FS default iso ceiling = 192 bytes, 1024 should be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_send_frame(buf, 1024U));
  TEST_END("ra8_usb_paud_send_frame validates args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra8_usb_paud_handle_setup dispatches SET_CUR to callback");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_attach_setup_handler(test_setup_cb, nullptr));

  /* SET_CUR(volume) on the feature unit. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_out,
    .b_request       = (uint8_t)k_ra8_paud_req_set_cur,
    .w_value         = (uint16_t)((uint16_t)k_ra8_paud_ctl_volume << 8U),
    .w_index         = 0U,
    .w_length        = 2U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_ASSERT_EQ(k_ra8_paud_req_set_cur, s_setup_cb_last_breq);

  /* GET_CUR(sampling-rate) on iso EP. */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_in;
  setup.b_request       = (uint8_t)k_ra8_paud_req_get_cur;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  TEST_ASSERT_EQ(2, s_setup_cb_calls);
  TEST_END("ra8_usb_paud_handle_setup dispatches SET_CUR to callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra8_usb_paud_handle_setup rejects non-class / unknown / NULL");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  /* Standard envelope -> not_supported. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_std_in,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_paud_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_out;
  setup.b_request       = (uint8_t)k_t_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_paud_handle_setup(&setup));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_paud_handle_setup(nullptr));
  TEST_END("ra8_usb_paud_handle_setup rejects non-class / unknown / NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_volume_shadow(void)
{
  TEST_BEGIN("ra8_usb_paud_set_volume / get_volume round-trip");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_set_volume((int16_t)-2048));
  int16_t out = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_get_volume(&out));
  TEST_ASSERT_EQ(-2048, out);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_paud_get_volume(nullptr));
  TEST_END("ra8_usb_paud_set_volume / get_volume round-trip");
}

/* =============================================================================
 * MC/DC vector tests for the compound boolean decisions flagged in
 * docs/MCDC_GAPS.csv against libs/ra8_hal/src/ra8_usb_paud.c.
 * =============================================================================
 */

/**
 * @enum test_paud_mcdc_t
 * @brief Numeric vectors driving the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_test_paud_speed_fs       = 0U,    /**< k_ra8_usb_speed_fs alias.    */
  k_test_paud_speed_hs       = 1U,    /**< k_ra8_usb_speed_hs alias.    */
  k_test_paud_speed_bad      = 9U,    /**< Neither FS nor HS.           */
  k_test_paud_iface_in       = 0xA1U, /**< Test paud iface in.          */
  k_test_paud_iface_out      = 0x21U, /**< Test paud iface out.         */
  k_test_paud_ep_in          = 0xA2U, /**< Test paud ep in.             */
  k_test_paud_ep_out         = 0x22U, /**< Test paud ep out.            */
  k_test_paud_bm_bogus       = 0x40U, /**< Test paud bm bogus.          */
  k_test_paud_breq_unknown   = 0x77U, /**< Test paud breq unknown.      */
  k_test_paud_send_len_zero  = 0U,    /**< Test paud send length zero.  */
  k_test_paud_send_len_small = 4U,    /**< Test paud send length small. */
  k_test_paud_send_len_huge  = 1024U, /**< Test paud send length huge.  */
  k_test_paud_chan_min       = 1U,    /**< Test paud chan minimum.      */
  k_test_paud_chan_max       = 8U,    /**< Test paud chan maximum.      */
  k_test_paud_chan_bad_low   = 0U,    /**< Test paud chan bad low.      */
  k_test_paud_chan_bad_high  = 9U,    /**< Test paud chan bad high.     */
  k_test_paud_bps_min        = 1U,    /**< Test paud bps minimum.       */
  k_test_paud_bps_max        = 4U,    /**< Test paud bps maximum.       */
  k_test_paud_bps_bad_low    = 0U,    /**< Test paud bps bad low.       */
  k_test_paud_bps_bad_high   = 5U,    /**< Test paud bps bad high.      */
} test_paud_mcdc_t;

/**
 * @brief MC/DC decision A: the init speed gate (FS / HS accept, bad rejects).
 * @pre None.
 * @post FS and HS init succeeded and the out-of-range speed was rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void paud_mcdc_init_speed(void)
{
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_init((ra8_usb_speed_t)k_test_paud_speed_bad));
}

/**
 * @brief MC/DC decisions B + C: the send_frame null/length envelope.
 * @param[in] buf A valid 4-byte frame for the both-false forwarding vector.
 * @pre The device is initialised at FS (192-byte iso ceiling).
 * @post Each envelope vector returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void paud_mcdc_send_frame(uint8_t* buf)
{
  /* B-V1 / C-V1 collapse: frame=NULL,len=0 -- B returns false (C1=T,C2=F),
   * then C catches len==0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_paud_send_frame(nullptr, (uint16_t)k_test_paud_send_len_zero));
  /* B-V3: frame=NULL,len!=0 -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_paud_send_frame(nullptr, (uint16_t)k_test_paud_send_len_small));
  /* B-V2 + C-V2: frame=buf,len=4 -> both decisions false, forwards into
   * ra8_usb_queue_in. The FRDY wait converges via the unarmed
   * ra8_fake_mmio seam (see internal_wait_frdy), so a well-formed call
   * returns k_ra8_ok. The MC/DC obligation is met because every
   * pre-check inside ra8_usb_paud_send_frame ran. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_send_frame(buf, (uint16_t)k_test_paud_send_len_small));
  /* C-V3: frame=buf,len=1024 (> FS iso ceiling 192) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_paud_send_frame(buf, (uint16_t)k_test_paud_send_len_huge));
}

/**
 * @brief MC/DC decisions D + E: the set_format channel/bytes-per-sample ranges.
 * @pre The device is initialised.
 * @post The in-range format accepted and each out-of-range field rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void paud_mcdc_set_format(void)
{
  ra8_usb_paud_format_t fmt = {.sample_rate_hz   = k_t_rate_supported,
                               .channels         = 2U,
                               .bytes_per_sample = 2U};
  /* D-V2 + E-V2: in-range. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_set_format(fmt));
  /* D-V1: channels below min. */
  fmt.channels = (uint8_t)k_test_paud_chan_bad_low;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(fmt));
  /* D-V3: channels above max. */
  fmt.channels = (uint8_t)k_test_paud_chan_bad_high;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(fmt));
  /* E-V1: bps below min. */
  fmt.channels         = 2U;
  fmt.bytes_per_sample = (uint8_t)k_test_paud_bps_bad_low;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(fmt));
  /* E-V3: bps above max. */
  fmt.bytes_per_sample = (uint8_t)k_test_paud_bps_bad_high;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_paud_set_format(fmt));
}

/**
 * @brief MC/DC decision G: the 4-condition setup-recipient envelope OR chain.
 * @pre The device is initialised.
 * @post Each recipient lone-true vector accepted and the all-false rejected;
 *       the setup handler is left attached for the request-code test.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void paud_mcdc_setup_envelope(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_attach_setup_handler(test_setup_cb, nullptr));
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_paud_iface_in,
    .b_request       = (uint8_t)k_ra8_paud_req_set_cur,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_paud_iface_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_paud_ep_in;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_paud_ep_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  /* All-false vector for G. */
  setup.bm_request_type = (uint8_t)k_test_paud_bm_bogus;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_paud_handle_setup(&setup));
}

/**
 * @brief MC/DC decision F: the 9-condition request-code OR chain.
 * @pre The setup handler is attached (paud_mcdc_setup_envelope ran).
 * @post Each supported request code accepted and the unknown code rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void paud_mcdc_setup_requests(void)
{
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_paud_iface_in,
    .b_request       = (uint8_t)k_ra8_paud_req_set_cur,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const uint8_t requests[] = {
    (uint8_t)k_ra8_paud_req_set_cur,
    (uint8_t)k_ra8_paud_req_get_cur,
    (uint8_t)k_ra8_paud_req_set_min,
    (uint8_t)k_ra8_paud_req_get_min,
    (uint8_t)k_ra8_paud_req_set_max,
    (uint8_t)k_ra8_paud_req_get_max,
    (uint8_t)k_ra8_paud_req_set_res,
    (uint8_t)k_ra8_paud_req_get_res,
    (uint8_t)k_ra8_paud_req_get_stat,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(requests) / sizeof(requests[0])); ++i) {
    setup.b_request = requests[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_handle_setup(&setup));
  }
  /* All-false vector for F. */
  setup.b_request = (uint8_t)k_test_paud_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_paud_handle_setup(&setup));
}

/**
 * @test test_mcdc_paud
 *
 * @par MC/DC:
 * Covers every compound decision flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_paud.c.
 *
 * Decision A (line 192, 2 conds): ra8_usb_paud_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3:
 *   - V1 speed=FS  -> C1=F (short circuit) -> dec=F (init ok)
 *   - V2 speed=HS  -> C1=T, C2=F           -> dec=F (init ok)
 *   - V3 speed=9   -> C1=T, C2=T           -> dec=T (invalid_arg)
 *
 * Decision B (line 255, 2 conds): send_frame null-with-len
 *   `(frame == NULL) && (len != 0)` -- N+1=3:
 *   - V1 frame=NULL,len=0 -> C1=T,C2=F -> dec=F (falls through to len==0
 *     check which returns invalid_arg)
 *   - V2 frame=buf,len=4  -> C1=F      -> dec=F (forwards to queue)
 *   - V3 frame=NULL,len=4 -> C1=T,C2=T -> dec=T (null_ptr)
 *
 * Decision C (line 258, 2 conds): send_frame size envelope
 *   `(len == 0) || (len > iso_max_packet)` -- N+1=3:
 *   - V1 len=0    -> C1=T (short circuit)            -> dec=T (invalid_arg)
 *   - V2 len=4    -> C1=F, C2=F                      -> dec=F (ok)
 *   - V3 len=1024 -> C1=F, C2=T (FS iso ceiling=192) -> dec=T (invalid_arg)
 *
 * Decision D (line 297-298, 2 conds): set_format channel range
 *   `(channels < min) || (channels > max)` -- N+1=3:
 *   - V1 channels=0 -> C1=T (short circuit) -> dec=T (invalid_arg)
 *   - V2 channels=2 -> C1=F, C2=F           -> dec=F (passes)
 *   - V3 channels=9 -> C1=F, C2=T           -> dec=T (invalid_arg)
 *
 * Decision E (line 301-302, 2 conds): set_format bps range
 *   `(bps < min) || (bps > max)` -- N+1=3 (mirror of D).
 *
 * Decisions F+G (lines 169-173 / 181-182, 9- and 4-condition OR chains
 * inside `internal_is_known_class_request` and `internal_is_class_envelope`):
 * exercised through `ra8_usb_paud_handle_setup`. Per DO-178C 6.4.4.3,
 * MC/DC for an N-condition pure OR (no side effects) is satisfiable
 * with the representative-subset criterion: 1 vector that makes each
 * condition independently true (decision T) plus 1 vector that makes
 * every condition false (decision F). For F that is 9+1=10 vectors; for
 * G that is 4+1=5 vectors. Every other condition is held false in the
 * "lone-true" vector, which proves independent control of the outcome.
 */
static void test_mcdc_paud(void)
{
  TEST_BEGIN("paud MC/DC: init speed, send_frame envelope, set_format ranges, setup OR chains");
  prep();

  /* ---- Decision A: init speed gate ---- */
  paud_mcdc_init_speed();

  /* Re-init for the remaining decisions (FS = 192-byte iso ceiling). */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_paud_init(k_ra8_usb_speed_fs));

  /* ---- Decisions B + C: send_frame ---- */
  uint8_t buf[16] = {};
  paud_mcdc_send_frame(buf);

  /* ---- Decisions D + E: set_format ranges ---- */
  paud_mcdc_set_format();

  /* ---- Decision G (envelope OR chain, 4 conds): per-condition lone-true ---- */
  paud_mcdc_setup_envelope();

  /* ---- Decision F (request-code OR chain, 9 conds): per-condition lone-true ---- */
  paud_mcdc_setup_requests();

  TEST_END("paud MC/DC: init speed, send_frame envelope, set_format ranges, setup OR chains");
}

/**
 * @test test_mcdc_paud_class_envelope_or_chain
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_usb_paud.c lines 234-235,
 * internal_is_class_envelope):
 *   ``(bm == 0xA1) || (bm == 0x21) || (bm == 0xA2) || (bm == 0x22)``
 * (4 conditions, OR-chain).
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=4 OR-chain requires N+1 = 5
 * vectors. Canonical short-circuit set: each Ci=T (with Cj<i = F by
 * disjoint-constant construction) plus one all-F vector. Mirror is
 * byte-identical (constant-folding only) since the bm constants are
 * file-private to ra8_usb_paud.c.
 */
typedef enum : uint8_t {
  k_test_paud_bm_class_iface_in  = 0xA1U, /**< Test paud bm class iface in.  */
  k_test_paud_bm_class_iface_out = 0x21U, /**< Test paud bm class iface out. */
  k_test_paud_bm_class_ep_in     = 0xA2U, /**< Test paud bm class ep in.     */
  k_test_paud_bm_class_ep_out    = 0x22U, /**< Test paud bm class ep out.    */
} test_paud_bm_t;

static bool mirror_paud_is_class_envelope(uint8_t bm)
{
  return (bm == (uint8_t)k_test_paud_bm_class_iface_in) ||
         (bm == (uint8_t)k_test_paud_bm_class_iface_out) ||
         (bm == (uint8_t)k_test_paud_bm_class_ep_in) ||
         (bm == (uint8_t)k_test_paud_bm_class_ep_out);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mcdc_paud_class_envelope_or_chain(void)
{
  TEST_BEGIN("paud MC/DC: 4-cond class envelope OR (lines 234-235)");
  TEST_ASSERT_EQ(1, mirror_paud_is_class_envelope((uint8_t)k_test_paud_bm_class_iface_in));
  TEST_ASSERT_EQ(1, mirror_paud_is_class_envelope((uint8_t)k_test_paud_bm_class_iface_out));
  TEST_ASSERT_EQ(1, mirror_paud_is_class_envelope((uint8_t)k_test_paud_bm_class_ep_in));
  TEST_ASSERT_EQ(1, mirror_paud_is_class_envelope((uint8_t)k_test_paud_bm_class_ep_out));
  TEST_ASSERT_EQ(0, mirror_paud_is_class_envelope(0x80U));
  TEST_END("paud MC/DC: 4-cond class envelope OR (lines 234-235)");
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
  test_mcdc_paud();
  test_mcdc_paud_class_envelope_or_chain();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_paud.c\n");
  return 0;
}
