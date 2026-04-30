/**
 * @file test_ra_usb_hhub.c
 * @brief Unit tests for the native USB host-side HUB class layer
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
#include "ra_usb_hhub.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_hhub_max_steps = 12U, /**< Loop bound for stepping through enum. */
} test_hhub_lim_t;

typedef enum : uint16_t {
  k_test_hhub_bm_class_other_in  = 0xA3U, /**< D2H | Class | Other.  */
  k_test_hhub_bm_class_other_out = 0x23U, /**< H2D | Class | Other.  */
  k_test_hhub_bm_class_dev_in    = 0xA0U, /**< D2H | Class | Device. */
  k_test_hhub_breq_get_status    = 0x00U, /**< HUB GET_STATUS.       */
  k_test_hhub_breq_clear_feature = 0x01U, /**< HUB CLEAR_FEATURE.    */
  k_test_hhub_breq_set_feature   = 0x03U, /**< HUB SET_FEATURE.      */
  k_test_hhub_default_ports      = 4U,    /**< Stub default port count. */
} test_hhub_setup_t;

static uint32_t             s_attach_count;
static ra_usb_hhub_device_t s_attach_last_device;
static void*                s_attach_last_ctx;
static const uintptr_t      k_test_hhub_ctx_token = 0xCAFEBABEU;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_hhub_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra_usb_hhub_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra_usb_hhub_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhub_attach_callback(stub_on_attach, (void*)k_test_hhub_ctx_token));
  for (uint8_t i = 0U; i < k_test_hhub_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_step());
  }
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_count);
}

/* ---- Lifecycle ---- */

static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_hhub_init FS returns k_ra_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dprpu));

  TEST_END("ra_usb_hhub_init FS returns k_ra_ok and flips DCFM");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_hhub_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hhub_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_hhub_init rejects bogus speed");
}

static void test_close_without_init(void)
{
  TEST_BEGIN("ra_usb_hhub_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhub_close());
  TEST_END("ra_usb_hhub_close before init returns invalid_state");
}

/* ---- Attach callback fires once ---- */

static void test_attach_callback_fires_once(void)
{
  TEST_BEGIN("attach callback fires once after the enum step machine completes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();

  TEST_ASSERT_EQ((int32_t)k_test_hhub_ctx_token, (int32_t)(uintptr_t)s_attach_last_ctx);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.device_address);
  TEST_ASSERT_EQ((int32_t)k_test_hhub_default_ports, (int32_t)s_attach_last_device.port_count);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- Pre-init guards ---- */

static void test_pre_init_guards(void)
{
  TEST_BEGIN("class API rejects pre-init");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhub_step());

  uint8_t  count  = 0U;
  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_get_port_status(1U, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_set_port_feature(1U, k_ra_hhub_feature_port_power));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_clear_port_feature(1U, k_ra_hhub_feature_c_port_reset));
  TEST_END("class API rejects pre-init");
}

/* ---- Pre-attach guards (post-init) ---- */

static void test_pre_attach_guards(void)
{
  TEST_BEGIN("class API rejects pre-attach with invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));

  uint8_t  count  = 0U;
  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_get_port_status(1U, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_set_port_feature(1U, k_ra_hhub_feature_port_power));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhub_clear_port_feature(1U, k_ra_hhub_feature_c_port_reset));
  TEST_END("class API rejects pre-attach with invalid_state");
}

/* ---- Null-arg rejection ---- */

static void test_null_arg_rejection(void)
{
  TEST_BEGIN("get_port_count / get_port_status reject NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_hhub_get_port_count(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_hhub_get_port_status(1U, nullptr));
  TEST_END("get_port_count / get_port_status reject NULL");
}

/* ---- Port range rejection ---- */

static void test_port_range_rejection(void)
{
  TEST_BEGIN("port-range guards reject port=0 and port>port_count");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hhub_get_port_status(0U, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hhub_get_port_status(99U, &status));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hhub_set_port_feature(0U, k_ra_hhub_feature_port_power));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hhub_clear_port_feature(99U, k_ra_hhub_feature_c_port_reset));
  TEST_END("port-range guards reject port=0 and port>port_count");
}

/* ---- get_port_count returns the cached default port count ---- */

static void test_get_port_count_value(void)
{
  TEST_BEGIN("get_port_count returns the cached port count from descriptor walk");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();

  uint8_t count = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_get_port_count(&count));
  TEST_ASSERT_EQ((int32_t)k_test_hhub_default_ports, (int32_t)count);
  TEST_END("get_port_count returns the cached port count from descriptor walk");
}

/* ---- get_port_status stages bmRequestType=0xA3 + bRequest=0x00 ---- */

static void test_get_port_status_envelope(void)
{
  TEST_BEGIN("ra_usb_hhub_get_port_status stages bmRequestType=0xA3 + bRequest=0x00");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  uint32_t status = 0xDEADBEEFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_get_port_status(2U, &status));
  /* status[]] zeroed on entry. */
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)status);
  /* USBREQ low byte = bmRequestType, high byte = bRequest = 0xA3 | (0x00<<8). */
  TEST_ASSERT_EQ((int32_t)0x00A3U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)ra_usb_fs()->USBINDX);
  TEST_ASSERT_EQ((int32_t)4U, (int32_t)ra_usb_fs()->USBLENG);
  TEST_END("ra_usb_hhub_get_port_status stages bmRequestType=0xA3 + bRequest=0x00");
}

/* ---- set_port_feature stages bmRequestType=0x23 + bRequest=0x03 ---- */

static void test_set_port_feature_envelope(void)
{
  TEST_BEGIN("ra_usb_hhub_set_port_feature stages bmRequestType=0x23 + bRequest=0x03");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_hhub_set_port_feature(3U, k_ra_hhub_feature_port_reset));
  /* USBREQ = (0x03 << 8) | 0x23 = 0x0323. */
  TEST_ASSERT_EQ((int32_t)0x0323U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)k_ra_hhub_feature_port_reset, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)3U, (int32_t)ra_usb_fs()->USBINDX);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_usb_fs()->USBLENG);
  TEST_END("ra_usb_hhub_set_port_feature stages bmRequestType=0x23 + bRequest=0x03");
}

/* ---- clear_port_feature stages bmRequestType=0x23 + bRequest=0x01 ---- */

static void test_clear_port_feature_envelope(void)
{
  TEST_BEGIN("ra_usb_hhub_clear_port_feature stages bmRequestType=0x23 + bRequest=0x01");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhub_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_hhub_clear_port_feature(1U, k_ra_hhub_feature_c_port_reset));
  /* USBREQ = (0x01 << 8) | 0x23 = 0x0123. */
  TEST_ASSERT_EQ((int32_t)0x0123U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)k_ra_hhub_feature_c_port_reset, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)ra_usb_fs()->USBINDX);
  TEST_END("ra_usb_hhub_clear_port_feature stages bmRequestType=0x23 + bRequest=0x01");
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
  (void)fprintf(stderr, "[OK ] test_ra_usb_hhub.c\n");
  return 0;
}
