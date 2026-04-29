/**
 * @file test_ra_usb_hcdc.c
 * @brief Unit tests for the native USB host-side CDC ACM class layer
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
#include "ra_usb_hcdc.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_hcdc_max_steps = 16U, /**< Loop bound for stepping through enum. */
} test_hcdc_lim_t;

static uint32_t             s_attach_count;
static ra_usb_hcdc_device_t s_attach_last_device;
static void*                s_attach_last_ctx;
static const uintptr_t      k_test_hcdc_ctx_token = 0xCAFEBABEU;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_hcdc_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra_usb_hcdc_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra_usb_hcdc_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

/* ---- Lifecycle ---- */

static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_hcdc_init FS returns k_ra_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dprpu));

  TEST_END("ra_usb_hcdc_init FS returns k_ra_ok and flips DCFM");
}

static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_hcdc_init HS returns k_ra_ok and sets HSE");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_hs));
  volatile r_usb_regs_t* reg = ra_usb_hs();
  const uint16_t         hse = (uint16_t)(1U << k_ra_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hse) != 0U);

  TEST_END("ra_usb_hcdc_init HS returns k_ra_ok and sets HSE");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_hcdc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hcdc_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_hcdc_init rejects bogus speed");
}

static void test_close_without_init(void)
{
  TEST_BEGIN("ra_usb_hcdc_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hcdc_close());
  TEST_END("ra_usb_hcdc_close before init returns invalid_state");
}

/* ---- Attach callback fires once after a simulated descriptor walk ---- */

static void test_attach_callback_fires_once(void)
{
  TEST_BEGIN("attach callback fires once after the enum step machine completes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hcdc_attach_callback(stub_on_attach, (void*)k_test_hcdc_ctx_token));

  /* Walk the step machine until the attach callback fires. */
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the simulated regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_step());
  }

  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_count);
  TEST_ASSERT_EQ((int32_t)k_test_hcdc_ctx_token, (int32_t)(uintptr_t)s_attach_last_ctx);
  /* Default CDC-ACM EP layout populated by the descriptor-walk stub. */
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.bulk_in_ep);
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)s_attach_last_device.bulk_out_ep);
  TEST_ASSERT_EQ((int32_t)3U, (int32_t)s_attach_last_device.intr_in_ep);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.device_address);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- send / recv null-arg rejection ---- */

static void test_send_null_arg_rejection(void)
{
  TEST_BEGIN("ra_usb_hcdc_send rejects null buffer with non-zero len");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_fs));

  /* Pre-attach: any send returns invalid_state. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hcdc_send(nullptr, 0U));

  /* Walk through enumeration so we are post-attach. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_step());
  }
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_count);

  /* Post-attach: null+len rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hcdc_send(nullptr, 8U));
  TEST_END("ra_usb_hcdc_send rejects null buffer with non-zero len");
}

static void test_recv_null_arg_rejection(void)
{
  TEST_BEGIN("ra_usb_hcdc_recv rejects null buf / null got_len / zero max_len");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_hcdc_recv(nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_hcdc_recv(buf, sizeof(buf), nullptr));

  /* Pre-attach with valid pointers should still fail with invalid_state. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hcdc_recv(buf, sizeof(buf), &got));

  /* Walk to attach. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_step());
  }

  /* Zero max_len now hits invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hcdc_recv(buf, 0U, &got));
  TEST_END("ra_usb_hcdc_recv rejects null buf / null got_len / zero max_len");
}

/* ---- set_line_coding null + range arg rejection ---- */

static void test_set_line_coding_arg_rejection(void)
{
  TEST_BEGIN("ra_usb_hcdc_set_line_coding rejects bogus args");
  prep();

  /* Pre-init: invalid_state. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hcdc_set_line_coding(9600U, k_ra_hcdc_parity_none, k_ra_hcdc_stop_1));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_fs));

  /* Pre-attach: invalid_state. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hcdc_set_line_coding(9600U, k_ra_hcdc_parity_none, k_ra_hcdc_stop_1));

  /* Walk to attach. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_step());
  }

  /* Zero baud is bogus. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hcdc_set_line_coding(0U, k_ra_hcdc_parity_none, k_ra_hcdc_stop_1));
  /* Out-of-range parity. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_hcdc_set_line_coding(9600U, (ra_usb_hcdc_parity_t)99U, k_ra_hcdc_stop_1));
  /* Out-of-range stop bits. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hcdc_set_line_coding(9600U,
                                                      k_ra_hcdc_parity_none,
                                                      (ra_usb_hcdc_stop_bits_t)99U));

  /* Valid call succeeds (DCPCTR has SUREQ cleared). */
  ra_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hcdc_set_line_coding(115200U, k_ra_hcdc_parity_even, k_ra_hcdc_stop_2));
  TEST_END("ra_usb_hcdc_set_line_coding rejects bogus args");
}

/* ---- Pre-init guards on attach_callback / step ---- */

static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / step / send / recv reject pre-init");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hcdc_step());

  uint8_t  buf[4] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hcdc_send(buf, sizeof(buf)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hcdc_recv(buf, sizeof(buf), &got));
  TEST_END("attach_callback / step / send / recv reject pre-init");
}

/* ---- Underlying ra_usb_host_* surface smoke test ---- */

static void test_host_set_uact_and_bus_reset(void)
{
  TEST_BEGIN("ra_usb_host_set_uact + bus_reset toggle DVSTCTR0 bits");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_set_uact(k_ra_usb_speed_fs, true));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT((reg->DVSTCTR0 & (uint16_t)0x10U) != 0U); /* UACT bit 4. */

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_bus_reset(k_ra_usb_speed_fs, true));
  TEST_ASSERT((reg->DVSTCTR0 & (uint16_t)0x40U) != 0U); /* USBRST bit 6. */
  /* USBRST asserted should force UACT low. */
  TEST_ASSERT_EQ(0, (int)(reg->DVSTCTR0 & (uint16_t)0x10U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_bus_reset(k_ra_usb_speed_fs, false));
  TEST_ASSERT_EQ(0, (int)(reg->DVSTCTR0 & (uint16_t)0x40U));

  /* Bogus speed rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_host_set_uact((ra_usb_speed_t)9U, true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_host_bus_reset((ra_usb_speed_t)9U, true));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_deinit(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_host_init((ra_usb_speed_t)9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_host_deinit((ra_usb_speed_t)9U));
  TEST_END("ra_usb_host_set_uact + bus_reset toggle DVSTCTR0 bits");
}

static void test_host_setup_request_validates(void)
{
  TEST_BEGIN("ra_usb_host_setup_request validates args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_init(k_ra_usb_speed_fs));

  const ra_usb_setup_t setup = {
    .bm_request_type = 0x80U,
    .b_request       = 0x06U,
    .w_value         = 0x0100U,
    .w_index         = 0U,
    .w_length        = 18U,
  };

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_host_setup_request(k_ra_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_host_setup_request((ra_usb_speed_t)9U, &setup));

  /* Clean SUREQ first; success path. */
  ra_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_host_setup_request(k_ra_usb_speed_fs, &setup));
  /* The setup mirror registers should now hold the request. */
  TEST_ASSERT_EQ((int32_t)0x0680U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)0x0100U, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)18U, (int32_t)ra_usb_fs()->USBLENG);

  /* SUREQ still asserted -> next call gets busy. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_usb_host_setup_request(k_ra_usb_speed_fs, &setup));
  TEST_END("ra_usb_host_setup_request validates args");
}

int32_t main(void)
{
  test_init_fs_returns_ok();
  test_init_hs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_attach_callback_fires_once();
  test_send_null_arg_rejection();
  test_recv_null_arg_rejection();
  test_set_line_coding_arg_rejection();
  test_pre_init_guards();
  test_host_set_uact_and_bus_reset();
  test_host_setup_request_validates();
  (void)fprintf(stderr, "[OK ] test_ra_usb_hcdc.c\n");
  return 0;
}
