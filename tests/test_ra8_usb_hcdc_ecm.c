/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_usb_hcdc_ecm.c
 * @brief Unit tests for the native USB host-side CDC-ECM (Ethernet
 *        over USB) class layer
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_hcdc_ecm.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_hcdc_ecm_max_steps = 24U, /**< Loop bound for stepping through enum. */
} test_hcdc_ecm_lim_t;

static uint32_t                  s_attach_count;
static ra8_usb_hcdc_ecm_device_t s_attach_last_device;
static void*                     s_attach_last_ctx;
static const uintptr_t           k_test_hcdc_ecm_ctx_token = 0xFEEDFACEU;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hcdc_ecm_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_hcdc_ecm_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_hcdc_ecm_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  for (uint8_t i = 0U; i < k_test_hcdc_ecm_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the fake regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_step());
  }
}

/* ---- Lifecycle ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_close_fs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_init FS / close round-trips ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_close());
  TEST_END("ra8_usb_hcdc_ecm_init FS / close round-trips ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_init HS sets HSE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_hs));
  volatile r_usb_regs_t* reg = ra8_usb_hs();
  const uint16_t         hse = (uint16_t)(1U << k_ra8_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hse) != 0U);
  TEST_END("ra8_usb_hcdc_ecm_init HS sets HSE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_hcdc_ecm_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_close());
  TEST_END("ra8_usb_hcdc_ecm_close before init returns invalid_state");
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, (void*)k_test_hcdc_ecm_ctx_token));
  walk_to_attach();

  TEST_ASSERT_EQ(1U, s_attach_count);
  TEST_ASSERT_EQ(k_test_hcdc_ecm_ctx_token, (uintptr_t)s_attach_last_ctx);
  TEST_ASSERT_EQ(1U, s_attach_last_device.bulk_in_ep);
  TEST_ASSERT_EQ(2U, s_attach_last_device.bulk_out_ep);
  TEST_ASSERT_EQ(3U, s_attach_last_device.intr_in_ep);
  TEST_ASSERT_EQ(1U, s_attach_last_device.device_address);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- send / recv null-arg rejection ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_null_arg_rejection(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_send_frame rejects null+len and oversized frames");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));

  /* Pre-attach: any send returns invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_send_frame(nullptr, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  walk_to_attach();
  TEST_ASSERT_EQ(1U, s_attach_count);

  /* Post-attach: null+non-zero len rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_send_frame(nullptr, 8U));
  /* Oversized frame (> 1514) rejected. */
  uint8_t dummy = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_send_frame(&dummy, 1600U));
  TEST_END("ra8_usb_hcdc_ecm_send_frame rejects null+len and oversized frames");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_null_arg_rejection(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_recv_frame rejects null buf / null got_len / zero max_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_ecm_recv_frame(nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_ecm_recv_frame(buf, sizeof(buf), nullptr));

  /* Pre-attach with valid pointers should still fail with invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_recv_frame(buf, sizeof(buf), &got));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  walk_to_attach();

  /* Zero max_len now hits invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_recv_frame(buf, 0U, &got));
  TEST_END("ra8_usb_hcdc_ecm_recv_frame rejects null buf / null got_len / zero max_len");
}

/* ---- set_packet_filter null + class-request envelope ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_packet_filter_class_envelope(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_set_packet_filter formats class-iface-out SETUP");
  prep();

  /* Pre-init: invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_ecm_set_packet_filter((uint16_t)k_ra8_hcdc_ecm_filter_broadcast));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));

  /* Pre-attach: invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_ecm_set_packet_filter((uint16_t)k_ra8_hcdc_ecm_filter_broadcast));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  walk_to_attach();

  /* Out-of-range bits rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_set_packet_filter(0x8000U));

  /* Valid call -- envelope check: bmRequestType=0x21, bRequest=0x43. */
  ra8_usb_fs()->DCPCTR = 0U;
  const uint16_t mask  = (uint16_t)((uint16_t)k_ra8_hcdc_ecm_filter_directed |
                                    (uint16_t)k_ra8_hcdc_ecm_filter_broadcast);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_set_packet_filter(mask));

  /* USBREQ holds (bRequest << 8) | bmRequestType. 0x43 << 8 | 0x21 = 0x4321. */
  TEST_ASSERT_EQ(0x4321U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(mask, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(0U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hcdc_ecm_set_packet_filter formats class-iface-out SETUP");
}

/* ---- get_link_status null + class-request envelope ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_link_status(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_get_link_status validates and queues GET_ETHERNET_STATISTIC");
  prep();
  ra8_usb_hcdc_ecm_link_t link = k_ra8_hcdc_ecm_link_up;

  /* Pre-init: NULL gets caught first. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_ecm_get_link_status(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));

  /* Pre-attach: invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_get_link_status(&link));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  walk_to_attach();

  /* Valid call. USBREQ = (0x44 << 8) | 0xA1 = 0x44A1. */
  ra8_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_get_link_status(&link));
  TEST_ASSERT_EQ(0x44A1U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(1U, ra8_usb_fs()->USBLENG);
  /* Default cached link is "down" until a NetworkConnection notification lands. */
  TEST_ASSERT_EQ(k_ra8_hcdc_ecm_link_down, link);
  TEST_END("ra8_usb_hcdc_ecm_get_link_status validates and queues GET_ETHERNET_STATISTIC");
}

/* ---- MAC parse from iMACAddress ASCII hex ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_parse_mac_address(void)
{
  TEST_BEGIN("ra8_usb_hcdc_ecm_parse_mac decodes 12 ASCII hex chars to 6 bytes");
  uint8_t mac[6] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_ecm_parse_mac(nullptr, mac));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_ecm_parse_mac("001122334455", nullptr));

  /* All-zero MAC. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_parse_mac("000000000000", mac));
  for (uint8_t i = 0U; i < 6U; ++i) {
    TEST_ASSERT_EQ(0, mac[i]);
  }

  /* Mixed-case digits. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_parse_mac("0123456789aB", mac));
  TEST_ASSERT_EQ(0x01, mac[0]);
  TEST_ASSERT_EQ(0x23, mac[1]);
  TEST_ASSERT_EQ(0x45, mac[2]);
  TEST_ASSERT_EQ(0x67, mac[3]);
  TEST_ASSERT_EQ(0x89, mac[4]);
  TEST_ASSERT_EQ(0xAB, mac[5]);

  /* Bogus character. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("00112233ZZ55", mac));

  TEST_END("ra8_usb_hcdc_ecm_parse_mac decodes 12 ASCII hex chars to 6 bytes");
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
  TEST_BEGIN("attach_callback / step / send / recv / filter / link reject pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_step());

  uint8_t  buf[4] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_send_frame(buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_recv_frame(buf, sizeof(buf), &got));

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_ecm_set_packet_filter((uint16_t)k_ra8_hcdc_ecm_filter_broadcast));

  ra8_usb_hcdc_ecm_link_t link = k_ra8_hcdc_ecm_link_up;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_ecm_get_link_status(&link));
  TEST_END("attach_callback / step / send / recv / filter / link reject pre-init");
}

/* ---- Filter mask covers documented bits ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_filter_mask_bits(void)
{
  TEST_BEGIN("filter mask enum bits match USB CDC ECM 1.20 sec 6.2.4");
  TEST_ASSERT_EQ(0x01, k_ra8_hcdc_ecm_filter_promiscuous);
  TEST_ASSERT_EQ(0x02, k_ra8_hcdc_ecm_filter_all_multicast);
  TEST_ASSERT_EQ(0x04, k_ra8_hcdc_ecm_filter_directed);
  TEST_ASSERT_EQ(0x08, k_ra8_hcdc_ecm_filter_broadcast);
  TEST_ASSERT_EQ(0x10, k_ra8_hcdc_ecm_filter_multicast);
  TEST_ASSERT_EQ(0x1F, k_ra8_hcdc_ecm_filter_all);
  TEST_END("filter mask enum bits match USB CDC ECM 1.20 sec 6.2.4");
}

/**
 * @test test_mcdc_hcdc_ecm
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_hcdc_ecm.c.
 *
 * Decision A (line 509, 2 conds): hcdc_ecm_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 575, 2 conds): send_frame NULL-with-len
 *   `(buf == NULL) && (len != 0)` -- N+1=3.
 * Decisions C/D/E (lines 305 / 309 / 313, 3 x 2-cond AND ranges in
 *   `internal_hex_nibble`): each tested via parse_mac with N+1=3
 *   vectors per range (in-range + low-out + high-out):
 *   - C `(c >= '0') && (c <= '9')`: '5' (T,T), '/' (F=short circuit),
 *     ':' (T,F).
 *   - D `(c >= 'a') && (c <= 'f')`: 'c' (T,T), '`' (F), 'g' (T,F).
 *   - E `(c >= 'A') && (c <= 'F')`: 'C' (T,T), '@' (F), 'G' (T,F).
 */
static void test_mcdc_hcdc_ecm(void)
{
  TEST_BEGIN("hcdc_ecm MC/DC: init speed / send_frame / hex_nibble ranges");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_close());
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_close());
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_init((ra8_usb_speed_t)9U));

  uint8_t mac[6] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_parse_mac("0123456789AB", mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_parse_mac("aabbccddeeff", mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_parse_mac("AABBCCDDEEFF", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("/123456789A", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac(":123456789A", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("`123456789A", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("g123456789A", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("@123456789A", mac));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_parse_mac("G123456789A", mac));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_ecm_attach_callback(stub_on_attach, nullptr));
  walk_to_attach();

  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_ecm_send_frame(nullptr, 4U));
  const ra8_err_t b_v1 = ra8_usb_hcdc_ecm_send_frame(nullptr, 0U);
  TEST_ASSERT(b_v1 != k_ra8_err_invalid_arg);
  const ra8_err_t b_v2 = ra8_usb_hcdc_ecm_send_frame(buf, 4U);
  TEST_ASSERT(b_v2 != k_ra8_err_invalid_arg);

  TEST_END("hcdc_ecm MC/DC: init speed / send_frame / hex_nibble ranges");
}

int32_t main(void)
{
  test_init_close_fs_returns_ok();
  test_init_hs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_attach_callback_fires_once();
  test_send_null_arg_rejection();
  test_recv_null_arg_rejection();
  test_set_packet_filter_class_envelope();
  test_get_link_status();
  test_parse_mac_address();
  test_pre_init_guards();
  test_filter_mask_bits();
  test_mcdc_hcdc_ecm();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hcdc_ecm.c\n");
  return 0;
}
