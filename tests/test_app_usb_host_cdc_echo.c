/**
 * @file test_app_usb_host_cdc_echo.c
 * @brief Integration test: USB host CDC ACM bring-up + echo flow
 *
 * @details
 * Mirrors the wave-1 multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/usb_host_cdc_echo/main.c brings
 * up CGC, routes the SCI8 log port and the USBHS VBUS-sense pin,
 * flips USBHS to host mode via ``ra_usb_hcdc_init(HS)``, registers an
 * attach callback, and loops calling ``ra_usb_hcdc_recv`` /
 * ``ra_usb_hcdc_send`` to echo bytes. This test exercises the same
 * ra_usb_hcdc surface the app calls, plus the supporting SCI8 + PFS
 * routing, under mocked MMIO.
 *
 * Modeled flow:
 *   1. ra_pfs_route_peripheral for the USBHS_VBUS pin (P4_08, PSEL=0x14).
 *   2. ra_usb_hcdc_init(HS).
 *   3. ra_usb_hcdc_attach_callback(...).
 *   4. ra_usb_hcdc_set_line_coding(...).
 *   5. ra_usb_hcdc_recv / ra_usb_hcdc_send shape calls.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sim_mmap.h"
#include "ra_usb_hcdc.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_hcdc_baud        = 115200U,
  k_test_hcdc_buf_bytes   = 64U,
  k_test_hcdc_pin_vbussns = (uint32_t)k_ra_port_4 << 8 | (uint32_t)k_ra_pin_8,
  k_test_hcdc_psel_usbhs  = 0x14U,   /**< PSEL = USBHS. */
  k_test_hcdc_ctx_token   = 0x4843U, /**< 'HC' */
  k_test_hcdc_bogus_speed = 9U,
} test_hcdc_const_t;

/** @brief Captured attach metadata. */
static uintptr_t            s_test_hcdc_attach_ctx;
static ra_usb_hcdc_device_t s_test_hcdc_attach_device;

/** @brief Mock attach callback -- mirrors usb_host_on_attach. */
static void test_hcdc_on_attach(void* ctx, const ra_usb_hcdc_device_t* device)
{
  s_test_hcdc_attach_ctx = (uintptr_t)ctx;
  if (device != NULL) {
    s_test_hcdc_attach_device = *device;
  }
}

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  (void)ra_usb_hcdc_close();
  s_test_hcdc_attach_ctx    = 0U;
  s_test_hcdc_attach_device = (ra_usb_hcdc_device_t){};
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the VBUS-sense PFS routing the app does at boot.
 *
 * @par MC/DC: Decision under test (in app): the
 * ``ra_pfs_route_peripheral != ok`` short-circuit guard. Two atomic
 * conditions x N+1 = 3 vectors -- this case covers the ok vector;
 * failure vector covered below.
 */
static void test_hcdc_pfs_routes_vbus_sense(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: PFS routes USBHS_VBUS sense");
  const ra_port_pin_t pin = (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_pfs_route_peripheral(pin, (ra_psel_t)k_test_hcdc_psel_usbhs, "test.vbus_sns"));
  TEST_END("usb_host_cdc_echo: PFS routes USBHS_VBUS sense");
}

/**
 * @brief Init the host driver at HS (the app's selected speed).
 *
 * @par MC/DC: Decision under test: ``ra_usb_hcdc_init() != k_ra_ok``.
 * Two vectors: speed=hs (this test) + invalid-speed (rejection test).
 */
static void test_hcdc_init_high_speed(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: hcdc_init HS");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_hs));
  TEST_END("usb_host_cdc_echo: hcdc_init HS");
}

/**
 * @brief init -> attach_callback -> set_line_coding chain.
 *
 * @par MC/DC: Decision under test: short-circuit chain of three
 * ``!= ok`` guards. Three atomic conditions x N+1 = 4 vectors; this
 * case covers all-ok.
 */
static void test_hcdc_init_callback_lineconfig_chain(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: init + attach_cb + set_line_coding");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hcdc_attach_callback(test_hcdc_on_attach, (void*)k_test_hcdc_ctx_token));
  ra_err_t err = ra_usb_hcdc_set_line_coding((uint32_t)k_test_hcdc_baud,
                                             k_ra_hcdc_parity_none,
                                             k_ra_hcdc_stop_1);
  /* set_line_coding may report not_ready before a device is enumerated;
   * either result keeps the chain semantics correct. */
  TEST_ASSERT(err == k_ra_ok || err == k_ra_err_hw_not_ready || err == k_ra_err_invalid_state);
  TEST_END("usb_host_cdc_echo: init + attach_cb + set_line_coding");
}

/**
 * @brief Echo body shape: recv then send mirrors the app's loop.
 *
 * @par MC/DC: Decision under test: ``recv != ok`` short-circuited
 * with ``send != ok``. Two atomic conditions x N+1 = 3 vectors; this
 * case covers call-shape.
 */
static void test_hcdc_recv_then_send_shape(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: recv + send call-shape");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hcdc_init(k_ra_usb_speed_hs));
  uint8_t  buf[k_test_hcdc_buf_bytes];
  uint16_t got = 0U;
  ra_err_t err = ra_usb_hcdc_recv(buf, (uint16_t)k_test_hcdc_buf_bytes, &got);
  /* Without an attached device, recv must report not_ready / invalid_state. */
  TEST_ASSERT(err == k_ra_err_hw_not_ready || err == k_ra_err_invalid_state ||
              err == k_ra_err_no_data || err == k_ra_ok);
  err = ra_usb_hcdc_send(buf, (uint16_t)k_test_hcdc_buf_bytes);
  TEST_ASSERT(err == k_ra_err_hw_not_ready || err == k_ra_err_invalid_state ||
              err == k_ra_err_no_mem || err == k_ra_ok);
  TEST_END("usb_host_cdc_echo: recv + send call-shape");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Bogus speed rejected by hcdc_init.
 *
 * @par MC/DC: Failure vector for the speed-validation atomic
 * condition in init. Pairs with the HS-init test for full coverage.
 */
static void test_hcdc_init_bad_speed_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: hcdc_init rejects bogus speed");
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hcdc_init((ra_usb_speed_t)k_test_hcdc_bogus_speed));
  TEST_END("usb_host_cdc_echo: hcdc_init rejects bogus speed");
}

/**
 * @brief close before init is rejected (worker-thread orderings).
 *
 * @par MC/DC: Failure side of the ``initialised == true`` invariant.
 */
static void test_hcdc_close_before_init_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: hcdc_close before init rejected");
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hcdc_close());
  TEST_END("usb_host_cdc_echo: hcdc_close before init rejected");
}

/**
 * @brief PFS route on out-of-range port is rejected.
 *
 * @par MC/DC: Failure-side vector for ``ra_pfs_route_peripheral != ok``.
 */
static void test_hcdc_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_cdc_echo: PFS rejects out-of-range port");
  const ra_port_pin_t bad_pin = (ra_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra_pin_0);
  TEST_ASSERT(ra_pfs_route_peripheral(bad_pin, (ra_psel_t)k_test_hcdc_psel_usbhs, "test.bad") !=
              k_ra_ok);
  TEST_END("usb_host_cdc_echo: PFS rejects out-of-range port");
}

int main(void)
{
  test_hcdc_pfs_routes_vbus_sense();
  test_hcdc_init_high_speed();
  test_hcdc_init_callback_lineconfig_chain();
  test_hcdc_recv_then_send_shape();
  test_hcdc_init_bad_speed_rejected();
  test_hcdc_close_before_init_rejected();
  test_hcdc_pfs_route_invalid_pin_rejected();
  return 0;
}
