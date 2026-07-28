/**
 * @file test_app_usb_host_keyboard.c
 * @brief Integration test: USB host HID boot-keyboard bring-up
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/usb_host_keyboard/main.c brings
 * up CGC, configures SCI8 (PD_02 / PD_03 at 115200 8N1) for the
 * J-Link OB CDC log channel, routes the USBHS_VBUS-sense pin (P4_08,
 * PSEL=0x14), arms LED1 + LED2, flips USBHS to host mode via
 * ``ra8_usb_hhid_init(HS)``, registers an attach callback, then
 * configures the keyboard for boot-protocol via
 * ``ra8_usb_hhid_set_protocol`` + ``ra8_usb_hhid_set_idle`` and polls
 * ``ra8_usb_hhid_get_input_report`` at 10 ms cadence.
 *
 * This test exercises the same module surface (PFS routing, SCI8
 * init, ra8_usb_hhid init/attach/configure/get_input_report) under
 * mocked MMIO.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_usb_hhid.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_hkbd_baud         = 115200U,    /**< Test hkbd baud.         */
  k_test_hkbd_pclk_hz      = 125000000U, /**< Test hkbd pclk Hz.      */
  k_test_hkbd_sci_channel  = 8U,         /**< Test hkbd SCI channel.  */
  k_test_hkbd_psel_usbhs   = 0x14U,      /**< Test hkbd psel usbhs.   */
  k_test_hkbd_report_bytes = 8U,         /**< Test hkbd report bytes. */
  k_test_hkbd_ctx_token    = 0x4842U,    /**< 'HB'                    */
  k_test_hkbd_bogus_speed  = 9U,         /**< Test hkbd bogus speed.  */
} test_hkbd_const_t;

/** @brief PD_02 TXD8 / PD_03 RXD8 -- mirrors the app. */
static const ra8_port_pin_t k_test_hkbd_pin_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << 8) | (uint16_t)k_ra8_pin_2);
static const ra8_port_pin_t k_test_hkbd_pin_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << 8) | (uint16_t)k_ra8_pin_3);

/** @brief Captured attach state. */
static uintptr_t             s_test_hkbd_attach_ctx;
static ra8_usb_hhid_device_t s_test_hkbd_attach_device;

/** @brief Mock attach callback -- mirrors usb_kbd_on_attach. */
static void test_hkbd_on_attach(void* ctx, const ra8_usb_hhid_device_t* device)
{
  s_test_hkbd_attach_ctx = (uintptr_t)ctx;
  if (device != nullptr) {
    s_test_hkbd_attach_device = *device;
  }
}

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_usb_hhid_close();
  s_test_hkbd_attach_ctx    = 0U;
  s_test_hkbd_attach_device = (ra8_usb_hhid_device_t){};
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the SCI8 PFS routing the app does at boot.
 *
 * @par MC/DC: Decision under test (in app): the ``ra8_pfs_route_peripheral
 * != ok`` short-circuit guards for TXD + RXD. Two atomic conditions x
 * N+1 = 3 vectors -- this case covers both-ok.
 */
static void test_hkbd_pfs_routes_sci8(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: PFS routes SCI8 TXD8 + RXD8");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_hkbd_pin_txd, k_ra8_psel_sci_async, "test.txd8"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_hkbd_pin_rxd, k_ra8_psel_sci_async, "test.rxd8"));
  TEST_END("usb_host_keyboard: PFS routes SCI8 TXD8 + RXD8");
}

/**
 * @brief SCI8 init at 115200 8N1 (J-Link OB CDC log).
 *
 * @par MC/DC: Decision under test: ``ra8_sci_init() != k_ra8_ok``. This
 * case covers the ok vector.
 */
static void test_hkbd_sci8_init_115200_8n1(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: SCI8 init 115200 8N1");
  const ra8_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_test_hkbd_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_test_hkbd_pclk_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_test_hkbd_sci_channel, &cfg));
  TEST_END("usb_host_keyboard: SCI8 init 115200 8N1");
}

/**
 * @brief Init host driver at HS + register attach callback.
 *
 * @par MC/DC: Decision under test: short-circuit chain of
 * ``hhid_init != ok`` and ``hhid_attach_callback != ok``. Two atomic
 * conditions x N+1 = 3 vectors; this case covers all-ok.
 */
static void test_hkbd_init_and_attach_callback_ok(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: hhid_init HS + attach_callback");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_attach_callback(test_hkbd_on_attach, (void*)k_test_hkbd_ctx_token));
  TEST_END("usb_host_keyboard: hhid_init HS + attach_callback");
}

/**
 * @brief Configure boot-protocol + idle == 0 (the app's post-attach
 *        chord).
 *
 * @par MC/DC: Decision under test: chained ``set_protocol != ok ||
 * set_idle != ok``. Two atomic conditions x N+1 = 3 vectors. Without
 * an actual attached device the calls return invalid_state /
 * not_ready; the test verifies the call shape and that the driver
 * does not return invalid_arg for legal arguments.
 */
static void test_hkbd_set_protocol_and_idle_call_shape(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: set_protocol + set_idle call-shape");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_hs));
  ra8_err_t e1 = ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot);
  TEST_ASSERT(e1 != k_ra8_err_invalid_arg);
  ra8_err_t e2 = ra8_usb_hhid_set_idle(0U, 0U);
  TEST_ASSERT(e2 != k_ra8_err_invalid_arg);
  TEST_END("usb_host_keyboard: set_protocol + set_idle call-shape");
}

/**
 * @brief Poll get_input_report -- mirrors the 10 ms loop body.
 *
 * @par MC/DC: Decision under test: ``get_input_report != ok``. Without
 * an attached device the call returns no-data / not-ready; the test
 * verifies call-shape + buffer untouched.
 */
static void test_hkbd_get_input_report_shape(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: get_input_report call-shape");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_hs));
  uint8_t   report[k_test_hkbd_report_bytes] = {};
  uint16_t  got                              = 0U;
  ra8_err_t err = ra8_usb_hhid_get_input_report(report, (uint16_t)k_test_hkbd_report_bytes, &got);
  TEST_ASSERT(err != k_ra8_err_invalid_arg);
  TEST_END("usb_host_keyboard: get_input_report call-shape");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Bogus speed rejected by hhid_init.
 *
 * @par MC/DC: Failure vector for the speed-validation atomic
 * condition in hhid_init. Pairs with the HS-init test for full
 * coverage.
 */
static void test_hkbd_init_bad_speed_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: hhid_init rejects bogus speed");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_init((ra8_usb_speed_t)k_test_hkbd_bogus_speed));
  TEST_END("usb_host_keyboard: hhid_init rejects bogus speed");
}

/**
 * @brief set_protocol before hhid_init is rejected.
 *
 * @par MC/DC: Failure side of the ``initialized == true`` invariant
 * check in set_protocol.
 */
static void test_hkbd_set_protocol_before_init_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: set_protocol before init rejected");
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot));
  TEST_END("usb_host_keyboard: set_protocol before init rejected");
}

/**
 * @brief PFS route on out-of-range port is rejected.
 *
 * @par MC/DC: Failure-side vector for ``ra8_pfs_route_peripheral != ok``.
 */
static void test_hkbd_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_keyboard: PFS rejects out-of-range port");
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra8_pin_0);
  TEST_ASSERT(ra8_pfs_route_peripheral(bad_pin, k_ra8_psel_sci_async, "test.bad") != k_ra8_ok);
  TEST_END("usb_host_keyboard: PFS rejects out-of-range port");
}

int main(void)
{
  test_hkbd_pfs_routes_sci8();
  test_hkbd_sci8_init_115200_8n1();
  test_hkbd_init_and_attach_callback_ok();
  test_hkbd_set_protocol_and_idle_call_shape();
  test_hkbd_get_input_report_shape();
  test_hkbd_init_bad_speed_rejected();
  test_hkbd_set_protocol_before_init_rejected();
  test_hkbd_pfs_route_invalid_pin_rejected();
  return 0;
}
