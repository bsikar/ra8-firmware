/**
 * @file test_ra8_usb_hhub.c
 * @brief Unit tests for the native USB host-side HUB class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_hhub.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_hhub_t
 * @brief Port-status out-parameter seed.
 */
typedef enum : uint32_t {
  k_t_status_unset = 0xDEADBEEFU, /**< Pre-set port status; a query that fails
                                       must leave it rather than report zero.    */
} t_hhub_t;

typedef enum : uint8_t {
  k_test_hhub_max_steps = 12U, /**< Loop bound for stepping through enum. */
} test_hhub_lim_t;

typedef enum : uint16_t {
  k_test_hhub_bm_class_other_in  = 0xA3U, /**< D2H | Class | Other.     */
  k_test_hhub_bm_class_other_out = 0x23U, /**< H2D | Class | Other.     */
  k_test_hhub_bm_class_dev_in    = 0xA0U, /**< D2H | Class | Device.    */
  k_test_hhub_breq_get_status    = 0x00U, /**< HUB GET_STATUS.          */
  k_test_hhub_breq_clear_feature = 0x01U, /**< HUB CLEAR_FEATURE.       */
  k_test_hhub_breq_set_feature   = 0x03U, /**< HUB SET_FEATURE.         */
  k_test_hhub_default_ports      = 4U,    /**< Stub default port count. */
} test_hhub_setup_t;

static uint32_t              s_attach_count;
static ra8_usb_hhub_device_t s_attach_last_device;
static void*                 s_attach_last_ctx;
static const uintptr_t       k_test_hhub_ctx_token = 0xCAFEBABEU;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hhub_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_hhub_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_hhub_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhub_attach_callback(stub_on_attach, (void*)k_test_hhub_ctx_token));
  for (uint8_t i = 0U; i < k_test_hhub_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_step());
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
  TEST_BEGIN("ra8_usb_hhub_init FS returns k_ra8_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_END("ra8_usb_hhub_init FS returns k_ra8_ok and flips DCFM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_hhub_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_hhub_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_hhub_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_close());
  TEST_END("ra8_usb_hhub_close before init returns invalid_state");
}

/* ---- Attach callback fires once ---- */

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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  TEST_ASSERT_EQ(k_test_hhub_ctx_token, (uintptr_t)s_attach_last_ctx);
  TEST_ASSERT_EQ(1U, s_attach_last_device.device_address);
  TEST_ASSERT_EQ(k_test_hhub_default_ports, s_attach_last_device.port_count);
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
  TEST_BEGIN("class API rejects pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_step());

  uint8_t  count  = 0U;
  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_get_port_status(1U, &status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhub_set_port_feature(1U, k_ra8_hhub_feature_port_power));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhub_clear_port_feature(1U, k_ra8_hhub_feature_c_port_reset));
  TEST_END("class API rejects pre-init");
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));

  uint8_t  count  = 0U;
  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhub_get_port_status(1U, &status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhub_set_port_feature(1U, k_ra8_hhub_feature_port_power));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhub_clear_port_feature(1U, k_ra8_hhub_feature_c_port_reset));
  TEST_END("class API rejects pre-attach with invalid_state");
}

/* ---- Null-arg rejection ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_arg_rejection(void)
{
  TEST_BEGIN("get_port_count / get_port_status reject NULL");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hhub_get_port_count(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hhub_get_port_status(1U, nullptr));
  TEST_END("get_port_count / get_port_status reject NULL");
}

/* ---- Port range rejection ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_port_range_rejection(void)
{
  TEST_BEGIN("port-range guards reject port=0 and port>port_count");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  uint32_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_get_port_status(0U, &status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_get_port_status(99U, &status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhub_set_port_feature(0U, k_ra8_hhub_feature_port_power));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhub_clear_port_feature(99U, k_ra8_hhub_feature_c_port_reset));
  TEST_END("port-range guards reject port=0 and port>port_count");
}

/* ---- get_port_count returns the cached default port count ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_port_count_value(void)
{
  TEST_BEGIN("get_port_count returns the cached port count from descriptor walk");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  uint8_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ(k_test_hhub_default_ports, count);
  TEST_END("get_port_count returns the cached port count from descriptor walk");
}

/* ---- get_port_status stages bmRequestType=0xA3 + bRequest=0x00 ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_port_status_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhub_get_port_status stages bmRequestType=0xA3 + bRequest=0x00");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  uint32_t status = k_t_status_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_get_port_status(2U, &status));
  /* status[]] zeroed on entry. */
  TEST_ASSERT_EQ(0U, status);
  /* USBREQ low byte = bmRequestType, high byte = bRequest = 0xA3 | (0x00<<8). */
  TEST_ASSERT_EQ(0x00A3U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(0U, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(2U, ra8_usb_fs()->USBINDX);
  TEST_ASSERT_EQ(4U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hhub_get_port_status stages bmRequestType=0xA3 + bRequest=0x00");
}

/* ---- set_port_feature stages bmRequestType=0x23 + bRequest=0x03 ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_port_feature_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhub_set_port_feature stages bmRequestType=0x23 + bRequest=0x03");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_set_port_feature(3U, k_ra8_hhub_feature_port_reset));
  /* USBREQ = (0x03 << 8) | 0x23 = 0x0323. */
  TEST_ASSERT_EQ(0x0323U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(k_ra8_hhub_feature_port_reset, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(3U, ra8_usb_fs()->USBINDX);
  TEST_ASSERT_EQ(0U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hhub_set_port_feature stages bmRequestType=0x23 + bRequest=0x03");
}

/* ---- clear_port_feature stages bmRequestType=0x23 + bRequest=0x01 ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_port_feature_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhub_clear_port_feature stages bmRequestType=0x23 + bRequest=0x01");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_clear_port_feature(1U, k_ra8_hhub_feature_c_port_reset));
  /* USBREQ = (0x01 << 8) | 0x23 = 0x0123. */
  TEST_ASSERT_EQ(0x0123U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(k_ra8_hhub_feature_c_port_reset, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(1U, ra8_usb_fs()->USBINDX);
  TEST_END("ra8_usb_hhub_clear_port_feature stages bmRequestType=0x23 + bRequest=0x01");
}

/**
 * @test test_mcdc_hhub
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_hhub.c.
 *
 * Decision A (line 300, 2 conds): hhub_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 211, 2 conds): internal_port_ok bounds check
 *   `(port >= first_port) && (port <= port_count)` -- N+1=3:
 *   - V1 port=2 (1..4 valid) -> C1=T,C2=T -> dec=T (forwards)
 *   - V2 port=0 (< first=1)  -> C1=F (short circuit) -> dec=F (invalid_arg)
 *   - V3 port=99 (> 4)       -> C1=T,C2=F -> dec=F (invalid_arg)
 *   Exercised through `ra8_usb_hhub_get_port_status`.
 */
static void test_mcdc_hhub(void)
{
  TEST_BEGIN("hhub MC/DC: init speed / port_ok bounds");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhub_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  uint32_t status = 0U;
  /* B-V1: port=2 in [1..4] -> ok (forwards SETUP). */
  const ra8_err_t b_v1 = ra8_usb_hhub_get_port_status(2U, &status);
  TEST_ASSERT(b_v1 != k_ra8_err_invalid_arg);
  /* B-V2: port=0 below first -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_get_port_status(0U, &status));
  /* B-V3: port=99 above count -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhub_get_port_status(99U, &status));

  TEST_END("hhub MC/DC: init speed / port_ok bounds");
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
  test_port_range_rejection();
  test_get_port_count_value();
  test_get_port_status_envelope();
  test_set_port_feature_envelope();
  test_clear_port_feature_envelope();
  test_mcdc_hhub();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hhub.c\n");
  return 0;
}
