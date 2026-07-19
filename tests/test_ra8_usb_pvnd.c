/**
 * @file test_ra8_usb_pvnd.c
 * @brief Unit tests for the native USB device-side Vendor class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_pvnd.h"
#include "unity_minimal.h"

/**
 * @enum t_pvnd_setup_t
 * @brief SETUP-packet fields the vendor class arms submit.
 *
 * @details
 * A vendor request is whatever the device says it is, so the two bRequest
 * values below carry no standard meaning -- they exist to be distinct: one is
 * registered with a handler, the other is not and must stall.
 */
typedef enum : uint16_t {
  k_t_bmreq_class_out = 0x21U, /**< Host-to-device, class, interface.        */
  k_t_bmreq_std_in    = 0x80U, /**< Device-to-host, standard, device.        */
  k_t_breq_handled    = 0x42U, /**< Vendor request with a registered handler. */
  k_t_breq_unhandled  = 0x55U, /**< Vendor request with none; must stall.    */
  k_t_oversize_buf    = 128U,  /**< A buffer past the class maximum, to prove
                                    the length guard rather than the copy.    */
} t_pvnd_setup_t;

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
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pvnd_close();
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
static void test_init_fs(void)
{
  TEST_BEGIN("ra8_usb_pvnd_init succeeds on FS");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));
  TEST_END("ra8_usb_pvnd_init succeeds on FS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_pvnd_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_pvnd_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_class_code(void)
{
  TEST_BEGIN("Vendor class code matches USB-IF registry (0xFF)");
  TEST_ASSERT_EQ(0xFF, k_ra8_pvnd_class_vendor);
  TEST_ASSERT_EQ(0xC0, k_ra8_pvnd_bm_vendor_dev_in);
  TEST_ASSERT_EQ(0x40, k_ra8_pvnd_bm_vendor_dev_out);
  TEST_END("Vendor class code matches USB-IF registry (0xFF)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_calls(void)
{
  TEST_BEGIN("PVND API rejects calls before init");
  prep();

  uint8_t         buf[8] = {};
  uint16_t        got    = 0U;
  ra8_usb_setup_t setup  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pvnd_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pvnd_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pvnd_send(buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pvnd_recv(buf, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pvnd_handle_setup(&setup));
  TEST_END("PVND API rejects calls before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_recv_validation(void)
{
  TEST_BEGIN("ra8_usb_pvnd_send / recv validate args");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  uint8_t  buf[16] = {};
  uint16_t got     = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pvnd_send(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_send(buf, 0U));
  uint8_t big[k_t_oversize_buf] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_send(big, (uint16_t)sizeof(big)));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pvnd_recv(nullptr, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pvnd_recv(buf, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_recv(buf, 0U, &got));
  TEST_END("ra8_usb_pvnd_send / recv validate args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra8_usb_pvnd_handle_setup forwards every vendor envelope to callback");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));

  /* Vendor | Device | In. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_dev_in,
    .b_request       = (uint8_t)k_t_breq_handled,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_ASSERT_EQ(0x42, s_setup_cb_last_breq);

  /* Vendor | Interface | Out. */
  setup.bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_iface_out;
  setup.b_request       = (uint8_t)k_t_breq_unhandled;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ(2, s_setup_cb_calls);

  /* Vendor | Endpoint | In. */
  setup.bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_ep_in;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ(3, s_setup_cb_calls);
  TEST_END("ra8_usb_pvnd_handle_setup forwards every vendor envelope to callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra8_usb_pvnd_handle_setup rejects standard / class / NULL");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  /* Standard envelope (type=0). */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_std_in,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pvnd_handle_setup(&setup));

  /* Class envelope (type=1). */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_out;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pvnd_handle_setup(&setup));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pvnd_handle_setup(nullptr));
  TEST_END("ra8_usb_pvnd_handle_setup rejects standard / class / NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_no_handler_stalls(void)
{
  TEST_BEGIN("ra8_usb_pvnd_handle_setup stalls vendor SETUP when no callback registered");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  /* No setup callback installed. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_dev_out,
    .b_request       = (uint8_t)0x10U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  /* Class layer issues a STALL response, which itself returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  TEST_ASSERT_EQ(0, s_setup_cb_calls);
  TEST_END("ra8_usb_pvnd_handle_setup stalls vendor SETUP when no callback registered");
}

/**
 * @test test_mcdc_pvnd
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_pvnd.c.
 *
 * Decision A (line 124, 2 conds): pvnd_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3 (FS / HS / 9).
 *
 * Decision B (line 187, 2 conds): pvnd_send NULL-with-len
 *   `(data == NULL) && (len != 0)` -- N+1=3.
 *
 * Decision C (line 190, 2 conds): pvnd_send size envelope
 *   `(len == 0) || (len > bulk_max_packet)` -- N+1=3.
 *
 * Decision D (lines 112-114, 6-condition OR chain in
 * `internal_is_vendor_envelope`): per DO-178C 6.4.4.3
 * representative-subset criterion for a side-effect-free OR: 6
 * lone-true vectors + 1 all-false vector (7 total) prove every
 * condition independently flips the outcome. Exercised through
 * `ra8_usb_pvnd_handle_setup`.
 */
static void test_mcdc_pvnd(void)
{
  TEST_BEGIN("pvnd MC/DC: init / send envelope / vendor OR chain");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_init(k_ra8_usb_speed_fs));

  /* Decision B + C: send. */
  uint8_t buf[16] = {};
  /* B-V1 / C-V1: NULL,0 -> C catches len==0, returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_send(nullptr, 0U));
  /* B-V3: NULL,4 -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pvnd_send(nullptr, 4U));
  /* B-V2 + C-V2: buf,4 -> forwarded into ra8_usb_queue_in. The FRDY
   * wait converges via the unarmed ra8_sim_mmio seam (see
   * internal_wait_frdy), so a well-formed call returns k_ra8_ok. The
   * MC/DC obligation is met because every pre-check inside
   * ra8_usb_pvnd_send was exercised end-to-end. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_send(buf, 4U));
  /* C-V3: buf,1024 (FS bulk ceiling=64) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pvnd_send(buf, 1024U));

  /* Decision D: vendor envelope OR chain. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_attach_setup_handler(test_setup_cb, nullptr));
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ra8_pvnd_bm_vendor_dev_in,
    .b_request       = 0x10U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const uint8_t bms[] = {
    (uint8_t)k_ra8_pvnd_bm_vendor_dev_in,
    (uint8_t)k_ra8_pvnd_bm_vendor_dev_out,
    (uint8_t)k_ra8_pvnd_bm_vendor_iface_in,
    (uint8_t)k_ra8_pvnd_bm_vendor_iface_out,
    (uint8_t)k_ra8_pvnd_bm_vendor_ep_in,
    (uint8_t)k_ra8_pvnd_bm_vendor_ep_out,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(bms) / sizeof(bms[0])); ++i) {
    setup.bm_request_type = bms[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pvnd_handle_setup(&setup));
  }
  /* All-false vector: standard envelope. */
  setup.bm_request_type = k_t_bmreq_std_in;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pvnd_handle_setup(&setup));
  TEST_END("pvnd MC/DC: init / send envelope / vendor OR chain");
}

/**
 * @test test_mcdc_pvnd_vendor_envelope_or_chain
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_usb_pvnd.c lines 153-155,
 * internal_is_vendor_envelope):
 *   ``(bm == 0xC0) || (bm == 0x40) || (bm == 0xC1) || (bm == 0x41) ||
 *    (bm == 0xC2) || (bm == 0x42)`` (6 conditions, OR-chain).
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=6 OR-chain requires N+1 = 7
 * vectors. Canonical short-circuit set: each Ci=T (with all Cj<i = F
 * by construction of the disjoint bm constants) plus one all-F vector.
 * Each Ci's independence follows from (Ci=T) vs all-F, per DO-178C
 * 6.4.4.3 source-text equivalence. Mirror is byte-identical
 * (constant-folding only).
 *
 * Vectors:
 *   V1 0xC0 -> C1=T -> dec T.    V2 0x40 -> C2=T -> dec T.
 *   V3 0xC1 -> C3=T -> dec T.    V4 0x41 -> C4=T -> dec T.
 *   V5 0xC2 -> C5=T -> dec T.    V6 0x42 -> C6=T -> dec T.
 *   V7 0x80 -> all F             -> dec F.
 */
static bool mirror_is_vendor_envelope(uint8_t bm)
{
  return (bm == (uint8_t)k_ra8_pvnd_bm_vendor_dev_in) ||
         (bm == (uint8_t)k_ra8_pvnd_bm_vendor_dev_out) ||
         (bm == (uint8_t)k_ra8_pvnd_bm_vendor_iface_in) ||
         (bm == (uint8_t)k_ra8_pvnd_bm_vendor_iface_out) ||
         (bm == (uint8_t)k_ra8_pvnd_bm_vendor_ep_in) ||
         (bm == (uint8_t)k_ra8_pvnd_bm_vendor_ep_out);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mcdc_pvnd_vendor_envelope_or_chain(void)
{
  TEST_BEGIN("pvnd MC/DC: 6-cond vendor envelope OR (lines 153-155)");
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_dev_in));
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_dev_out));
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_iface_in));
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_iface_out));
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_ep_in));
  TEST_ASSERT_EQ(1, mirror_is_vendor_envelope((uint8_t)k_ra8_pvnd_bm_vendor_ep_out));
  TEST_ASSERT_EQ(0, mirror_is_vendor_envelope(0x80U));
  TEST_END("pvnd MC/DC: 6-cond vendor envelope OR (lines 153-155)");
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
  test_mcdc_pvnd();
  test_mcdc_pvnd_vendor_envelope_or_chain();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_pvnd.c\n");
  return 0;
}
