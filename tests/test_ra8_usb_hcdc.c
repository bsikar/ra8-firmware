/**
 * @file test_ra8_usb_hcdc.c
 * @brief Unit tests for the native USB host-side CDC ACM class layer
 * @details Covers host CDC enumeration, line coding, control/data transfers, attach callbacks, and validation failures.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_hcdc.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_hcdc_max_steps = 16U, /**< Loop bound for stepping through enum. */
} test_hcdc_lim_t;

static uint32_t              s_attach_count;
static ra8_usb_hcdc_device_t s_attach_last_device;
static void*                 s_attach_last_ctx;
static const uintptr_t       k_test_hcdc_ctx_token = 0xCAFEBABEU;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hcdc_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_hcdc_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_hcdc_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
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
  TEST_BEGIN("ra8_usb_hcdc_init FS returns k_ra8_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_END("ra8_usb_hcdc_init FS returns k_ra8_ok and flips DCFM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_hcdc_init HS returns k_ra8_ok and sets HSE");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_hs));
  volatile r_usb_regs_t* reg = ra8_usb_hs();
  const uint16_t         hse = (uint16_t)(1U << k_ra8_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hse) != 0U);

  TEST_END("ra8_usb_hcdc_init HS returns k_ra8_ok and sets HSE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_hcdc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_hcdc_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_hcdc_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_close());
  TEST_END("ra8_usb_hcdc_close before init returns invalid_state");
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hcdc_attach_callback(stub_on_attach, (void*)k_test_hcdc_ctx_token));

  /* Walk the step machine until the attach callback fires. */
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the fake regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  }

  TEST_ASSERT_EQ(1U, s_attach_count);
  TEST_ASSERT_EQ(k_test_hcdc_ctx_token, (uintptr_t)s_attach_last_ctx);
  /* Default CDC-ACM EP layout populated by the descriptor-walk stub. */
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
  TEST_BEGIN("ra8_usb_hcdc_send rejects null buffer with non-zero len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));

  /* Pre-attach: any send returns invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_send(nullptr, 0U));

  /* Walk through enumeration so we are post-attach. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  }
  TEST_ASSERT_EQ(1U, s_attach_count);

  /* Post-attach: null+len rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_send(nullptr, 8U));
  TEST_END("ra8_usb_hcdc_send rejects null buffer with non-zero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_null_arg_rejection(void)
{
  TEST_BEGIN("ra8_usb_hcdc_recv rejects null buf / null got_len / zero max_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_recv(nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hcdc_recv(buf, sizeof(buf), nullptr));

  /* Pre-attach with valid pointers should still fail with invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_recv(buf, sizeof(buf), &got));

  /* Walk to attach. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  }

  /* Zero max_len now hits invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_recv(buf, 0U, &got));
  TEST_END("ra8_usb_hcdc_recv rejects null buf / null got_len / zero max_len");
}

/* ---- set_line_coding null + range arg rejection ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_line_coding_arg_rejection(void)
{
  TEST_BEGIN("ra8_usb_hcdc_set_line_coding rejects bogus args");
  prep();

  /* Pre-init: invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_set_line_coding(9600U, k_ra8_hcdc_parity_none, k_ra8_hcdc_stop_1));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));

  /* Pre-attach: invalid_state. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hcdc_set_line_coding(9600U, k_ra8_hcdc_parity_none, k_ra8_hcdc_stop_1));

  /* Walk to attach. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  }

  /* Zero baud is bogus. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hcdc_set_line_coding(0U, k_ra8_hcdc_parity_none, k_ra8_hcdc_stop_1));
  /* Out-of-range parity. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_hcdc_set_line_coding(9600U, (ra8_usb_hcdc_parity_t)99U, k_ra8_hcdc_stop_1));
  /* Out-of-range stop bits. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_hcdc_set_line_coding(9600U, k_ra8_hcdc_parity_none, (ra8_usb_hcdc_stop_bits_t)99U));

  /* Valid call succeeds (DCPCTR has SUREQ cleared). */
  ra8_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hcdc_set_line_coding(115200U, k_ra8_hcdc_parity_even, k_ra8_hcdc_stop_2));
  TEST_END("ra8_usb_hcdc_set_line_coding rejects bogus args");
}

/* ---- Pre-init guards on attach_callback / step ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / step / send / recv reject pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_step());

  uint8_t  buf[4] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_send(buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hcdc_recv(buf, sizeof(buf), &got));
  TEST_END("attach_callback / step / send / recv reject pre-init");
}

/* ---- Underlying ra8_usb_host_* surface smoke test ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_host_set_uact_and_bus_reset(void)
{
  TEST_BEGIN("ra8_usb_host_set_uact + bus_reset toggle DVSTCTR0 bits");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_set_uact(k_ra8_usb_speed_fs, true));
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT((reg->DVSTCTR0 & (uint16_t)0x10U) != 0U); /* UACT bit 4. */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_bus_reset(k_ra8_usb_speed_fs, true));
  TEST_ASSERT((reg->DVSTCTR0 & (uint16_t)0x40U) != 0U); /* USBRST bit 6. */
  /* USBRST asserted should force UACT low. */
  TEST_ASSERT_EQ(0, (reg->DVSTCTR0 & (uint16_t)0x10U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_bus_reset(k_ra8_usb_speed_fs, false));
  TEST_ASSERT_EQ(0, (reg->DVSTCTR0 & (uint16_t)0x40U));

  /* Bogus speed rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_host_set_uact((ra8_usb_speed_t)9U, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_host_bus_reset((ra8_usb_speed_t)9U, true));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_deinit(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_host_init((ra8_usb_speed_t)9U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_host_deinit((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_host_set_uact + bus_reset toggle DVSTCTR0 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_host_setup_request_validates(void)
{
  TEST_BEGIN("ra8_usb_host_setup_request validates args");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_init(k_ra8_usb_speed_fs));

  const ra8_usb_setup_t setup = {
    .bm_request_type = 0x80U,
    .b_request       = 0x06U,
    .w_value         = 0x0100U,
    .w_index         = 0U,
    .w_length        = 18U,
  };

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_host_setup_request(k_ra8_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_host_setup_request((ra8_usb_speed_t)9U, &setup));

  /* Clean SUREQ first; success path. */
  ra8_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_setup_request(k_ra8_usb_speed_fs, &setup));
  /* The setup mirror registers should now hold the request. */
  TEST_ASSERT_EQ(0x0680U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(0x0100U, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(18U, ra8_usb_fs()->USBLENG);

  /* SUREQ still asserted -> next call gets busy. */
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_usb_host_setup_request(k_ra8_usb_speed_fs, &setup));
  TEST_END("ra8_usb_host_setup_request validates args");
}

/**
 * @test test_mcdc_hcdc
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_hcdc.c.
 *
 * Decision A (line 416, 2 conds): hcdc_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 481, 2 conds): hcdc_send NULL-with-len
 *   `(data == NULL) && (len != 0)` -- N+1=3.
 *   - V1 (NULL,0)  -> C1=T, C2=F -> dec=F (forwards to queue)
 *   - V2 (buf,4)   -> C1=F       -> dec=F (forwards to queue)
 *   - V3 (NULL,4)  -> C1=T, C2=T -> dec=T (invalid_arg)
 */
static void test_mcdc_hcdc(void)
{
  TEST_BEGIN("hcdc MC/DC: init speed / send NULL-with-len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_attach_callback(stub_on_attach, nullptr));
  for (uint8_t i = 0U; i < k_test_hcdc_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hcdc_step());
  }
  /* Post-attach: exercise B vectors. */
  uint8_t buf[16] = {};
  /* B-V3: (NULL, 4) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hcdc_send(nullptr, 4U));
  /* B-V1: (NULL, 0) -> not invalid_arg (forwarded). */
  const ra8_err_t b_v1 = ra8_usb_hcdc_send(nullptr, 0U);
  TEST_ASSERT(b_v1 != k_ra8_err_invalid_arg);
  /* B-V2: (buf, 4) -> not invalid_arg (forwarded). */
  const ra8_err_t b_v2 = ra8_usb_hcdc_send(buf, 4U);
  TEST_ASSERT(b_v2 != k_ra8_err_invalid_arg);

  TEST_END("hcdc MC/DC: init speed / send NULL-with-len");
}

int main(void)
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
  test_mcdc_hcdc();
  return 0;
}
