/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_usb_haud.c
 * @brief Unit tests for the native USB host-side Audio class layer
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_haud.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_haud_max_steps = 16U, /**< Loop bound for stepping through enum. */
} test_haud_lim_t;

/**
 * @enum test_haud_setup_t
 * @brief Wire-level constants the tests assert against.
 */
typedef enum : uint16_t {
  k_test_haud_bm_class_iface_out = 0x21U, /**< H2D | Class | Interface. */
  k_test_haud_bm_class_ep_out    = 0x22U, /**< H2D | Class | Endpoint.  */
  k_test_haud_breq_set_cur       = 0x01U, /**< Audio SET_CUR.           */
  k_test_haud_volume_sel         = 0x02U, /**< VOLUME_CONTROL.          */
  k_test_haud_mute_sel           = 0x01U, /**< MUTE_CONTROL.            */
  k_test_haud_sample_freq_sel    = 0x01U, /**< SAMPLING_FREQ_CONTROL.   */
} test_haud_setup_t;

static uint32_t              s_attach_count;
static ra8_usb_haud_device_t s_attach_last_device;
static void*                 s_attach_last_ctx;
static const uintptr_t       k_test_haud_ctx_token = 0xCAFE1234U;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_haud_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_haud_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_haud_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_haud_attach_callback(stub_on_attach, (void*)k_test_haud_ctx_token));
  for (uint8_t i = 0U; i < k_test_haud_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the fake regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_step());
  }
  TEST_ASSERT_EQ(1U, s_attach_count);
}

/* ---- Lifecycle ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_haud_init FS returns k_ra8_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_END("ra8_usb_haud_init FS returns k_ra8_ok and flips DCFM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_haud_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_haud_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_haud_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_close());
  TEST_END("ra8_usb_haud_close before init returns invalid_state");
}

/* ---- Attach callback fires once after a fake descriptor walk ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_callback_fires_once(void)
{
  TEST_BEGIN("attach callback fires once after the enum step machine completes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  TEST_ASSERT_EQ(k_test_haud_ctx_token, (uintptr_t)s_attach_last_ctx);
  /* Default canonical USB-speaker layout populated by the descriptor-
   * walk stub: device address 1, AC iface 0, AS iface 1, FU id 2,
   * iso-OUT EP 1, 2 channels, 16-bit, 48 kHz. */
  TEST_ASSERT_EQ(1U, s_attach_last_device.device_address);
  TEST_ASSERT_EQ(0U, s_attach_last_device.ac_interface);
  TEST_ASSERT_EQ(1U, s_attach_last_device.as_interface);
  TEST_ASSERT_EQ(2U, s_attach_last_device.feature_unit_id);
  TEST_ASSERT_EQ(1U, s_attach_last_device.iso_out_ep);
  TEST_ASSERT_EQ(0U, s_attach_last_device.iso_in_ep);
  TEST_ASSERT_EQ(2U, s_attach_last_device.channel_count);
  TEST_ASSERT_EQ(16U, s_attach_last_device.bits_per_sample);
  TEST_ASSERT_EQ(48000U, s_attach_last_device.sample_rate_min_hz);
  TEST_ASSERT_EQ(48000U, s_attach_last_device.sample_rate_max_hz);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- Pre-init guards ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / step / set_format / set_volume / set_mute / "
             "send_samples / recv_samples reject pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_step());

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_format(2U, 16U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_volume(0U, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_mute(0U, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_send_samples(buf, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_haud_recv_samples(buf, (uint16_t)sizeof(buf), &got));
  TEST_END("attach_callback / step / set_format / set_volume / set_mute / "
           "send_samples / recv_samples reject pre-init");
}

/* ---- Pre-attach guards (post-init) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_attach_guards(void)
{
  TEST_BEGIN("class API rejects pre-attach with invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_format(2U, 16U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_volume(0U, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_set_mute(0U, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_haud_send_samples(buf, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_haud_recv_samples(buf, (uint16_t)sizeof(buf), &got));
  TEST_END("class API rejects pre-attach with invalid_state");
}

/* ---- Null-arg rejection on send / recv samples ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_arg_rejection(void)
{
  TEST_BEGIN("send_samples / recv_samples reject NULL buf / got_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_haud_send_samples(nullptr, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_haud_recv_samples(nullptr, (uint16_t)sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_haud_recv_samples(buf, (uint16_t)sizeof(buf), nullptr));
  TEST_END("send_samples / recv_samples reject NULL buf / got_len");
}

/* ---- set_format invalid params ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_format_invalid_args(void)
{
  TEST_BEGIN("set_format rejects out-of-range channels / bits / rate");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(0U, 16U, 48000U)); /* 0 channels. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(9U, 16U, 48000U)); /* 9 channels. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_haud_set_format(2U, 4U, 48000U)); /* 4-bit too low. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_haud_set_format(2U, 64U, 48000U)); /* 64-bit too high. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_haud_set_format(2U, 16U, 4000U)); /* 4 kHz too low. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_haud_set_format(2U, 16U, 384000U)); /* 384 kHz too high. */
  TEST_END("set_format rejects out-of-range channels / bits / rate");
}

/* ---- set_format happy: stages SAMPLING_FREQ_CONTROL on the wire ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_format_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_haud_set_format stages bmRequestType=0x22 + bRequest=0x01 "
             "(SAMPLING_FREQ_CONTROL)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_set_format(2U, 24U, 96000U));
  /* USBREQ low byte = bmRequestType, high byte = bRequest.
   * For SET_CUR endpoint: 0x01 << 8 | 0x22 = 0x0122. */
  TEST_ASSERT_EQ(0x0122U, ra8_usb_fs()->USBREQ);
  /* USBVAL = SAMPLING_FREQ_CONTROL << 8 = 0x0100. */
  TEST_ASSERT_EQ(0x0100U, ra8_usb_fs()->USBVAL);
  /* USBLENG = 3 (24-bit LE sample rate payload). */
  TEST_ASSERT_EQ(3U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_haud_set_format stages bmRequestType=0x22 + bRequest=0x01");
}

/* ---- set_volume stages a Feature-Unit SET_CUR(VOLUME) on the wire ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_volume_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_haud_set_volume stages bmRequestType=0x21 + bRequest=0x01 "
             "(VOLUME_CONTROL)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  /* channel=0 (main), volume=-12 dB (-3072 in 1/256 step). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_set_volume(0U, (int16_t)-3072));
  /* USBREQ = (0x01 << 8) | 0x21 = 0x0121. */
  TEST_ASSERT_EQ(0x0121U, ra8_usb_fs()->USBREQ);
  /* USBVAL = (VOLUME_CONTROL << 8) | channel = 0x0200. */
  TEST_ASSERT_EQ(0x0200U, ra8_usb_fs()->USBVAL);
  /* USBINDX = (FU id 2 << 8) | AC iface 0 = 0x0200. */
  TEST_ASSERT_EQ(0x0200U, ra8_usb_fs()->USBINDX);
  /* USBLENG = 2 (signed 16-bit dB payload). */
  TEST_ASSERT_EQ(2U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_haud_set_volume stages bmRequestType=0x21 + bRequest=0x01");
}

/* ---- set_mute stages a Feature-Unit SET_CUR(MUTE) on the wire ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mute_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_haud_set_mute stages bmRequestType=0x21 + bRequest=0x01 "
             "(MUTE_CONTROL)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_set_mute(0U, true));
  /* USBREQ = (0x01 << 8) | 0x21 = 0x0121. */
  TEST_ASSERT_EQ(0x0121U, ra8_usb_fs()->USBREQ);
  /* USBVAL = (MUTE_CONTROL << 8) | channel = 0x0100. */
  TEST_ASSERT_EQ(0x0100U, ra8_usb_fs()->USBVAL);
  /* USBINDX = (FU id 2 << 8) | AC iface 0 = 0x0200. */
  TEST_ASSERT_EQ(0x0200U, ra8_usb_fs()->USBINDX);
  /* USBLENG = 1 (boolean payload). */
  TEST_ASSERT_EQ(1U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_haud_set_mute stages bmRequestType=0x21 + bRequest=0x01");
}

/* ---- send_samples / recv_samples invalid args (max_len = 0) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_iso_invalid_args(void)
{
  TEST_BEGIN("send_samples / recv_samples reject zero-length post-attach");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_send_samples(buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_recv_samples(buf, 0U, &got));
  /* No iso-IN EP in the default stub layout, so recv_samples should
   * return invalid_state for any non-zero max_len. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_haud_recv_samples(buf, (uint16_t)sizeof(buf), &got));
  TEST_END("send_samples / recv_samples reject zero-length post-attach");
}

/**
 * @test test_mcdc_haud
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_haud.c.
 *
 * Decision A (line 469, 2 conds): haud_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3 (FS / HS / 9).
 * Decision B (line 636, 2 conds): send_samples NULL-with-len
 *   `(buf == NULL) && (len_bytes != 0)` -- N+1=3.
 *   Note: source has a redundant 2nd `if (buf == nullptr)` immediately
 *   after, so any NULL buf returns null_ptr regardless. The compound
 *   decision is still exercised: V1=(NULL,0) flips to false because
 *   C2=F, V3=(NULL,4) is true.
 * Decisions C/D/E (lines 447-456, 3 x 2-cond OR decisions inside
 *   `internal_format_ok`, exercised through `ra8_usb_haud_set_format`):
 *   each tested with N+1=3 vectors (in-range, low-out-of-range,
 *   high-out-of-range) for channel_count, bits_per_sample, and
 *   sample_rate respectively.
 */
static void test_mcdc_haud(void)
{
  TEST_BEGIN("haud MC/DC: init speed / send_samples / format ranges");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_haud_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_haud_send_samples(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_haud_send_samples(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_send_samples(buf, 0U));

  const ra8_err_t c_in = ra8_usb_haud_set_format(2U, 16U, 48000U);
  TEST_ASSERT(c_in != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(0U, 16U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(9U, 16U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(2U, 4U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(2U, 64U, 48000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(2U, 16U, 4000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_haud_set_format(2U, 16U, 384000U));

  TEST_END("haud MC/DC: init speed / send_samples / format ranges");
}

int32_t main(void)
{
  test_init_fs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_attach_callback_fires_once();
  test_pre_init_guards();
  test_pre_attach_guards();
  test_null_arg_rejection();
  test_set_format_invalid_args();
  test_set_format_setup_envelope();
  test_set_volume_setup_envelope();
  test_set_mute_setup_envelope();
  test_iso_invalid_args();
  test_mcdc_haud();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_haud.c\n");
  return 0;
}
