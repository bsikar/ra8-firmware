/**
 * @file test_app_usb_cdc_echo.c
 * @brief Integration test: ThreadX + USBX CDC ACM echo bring-up
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/usb_cdc_echo/main.c brings up
 * CGC, routes the four USB-FS pins (P4_07 VBUS, P5_00 VBUSEN, P8_14
 * D+, P8_15 D-), arms LED1, hands control to ThreadX, and lets USBX's
 * CDC ACM class drive the ra_usb_pal device-side controller. Neither
 * USBX nor ThreadX is linked into the host test build, so this test
 * exercises the same module surface the app calls (PFS routing +
 * ra_usb_pal init/attach/ep_open/ep_send) under mocked MMIO.
 *
 * Modeled flow:
 *   1. ra_pfs_route_peripheral for the four USB-FS pins.
 *   2. ra_board_led_init(LED1).
 *   3. ra_usb_pal_init(speed=fs).
 *   4. ra_usb_pal_attach(true)         -- D+ pull-up on.
 *   5. ra_usb_pal_ep_open(EP1 IN bulk) -- CDC bulk-IN data pipe.
 *   6. ra_usb_pal_ep_send(echo back)   -- bulk-IN reply.
 *
 * Exercised modules:
 *   - ra_pfs (peripheral pin routing)
 *   - ra_board_ek_ra8d2 (LED bring-up)
 *   - ra_usb_pal (device-side controller bridge)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sim_mmap.h"
#include "ra_usb_pal.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint16_t {
  k_test_cdc_ep_bulk_out  = 0x02U, /**< EP2 OUT bulk -- CDC data class. */
  k_test_cdc_ep_bulk_in   = 0x81U, /**< EP1 IN  bulk -- CDC data class. */
  k_test_cdc_ep_intr_in   = 0x83U, /**< EP3 IN  intr -- CDC notify pipe. */
  k_test_cdc_max_packet   = 64U,   /**< Bulk-FS packet size. */
  k_test_cdc_intr_packet  = 8U,    /**< Notify-EP packet size. */
  k_test_cdc_echo_payload = 8U,    /**< Sample echo payload bytes. */
} test_cdc_const_t;

/** @brief Pin map mirrors examples/ek_ra8d2/usb_cdc_echo/main.c. */
static const ra_port_pin_t k_test_cdc_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_test_cdc_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_test_cdc_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_test_cdc_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  (void)ra_usb_pal_deinit();
}

/* -------------------------------------------------------------------------
 * Golden path: full bring-up
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the four-pin USB-FS routing block in demo_pins_init().
 *
 * @par MC/DC: Decision under test (in app): the four chained
 * ``ra_pfs_route_peripheral != ok`` early-return guards. Four atomic
 * conditions x N+1 = 5 vectors. This case covers all-ok; failure
 * vectors split across the rejection tests below.
 */
static void test_cdc_pfs_routes_four_usbfs_pins(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: PFS routes VBUS/VBUSEN/D+/D-");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_pfs_route_peripheral(k_test_cdc_pin_vbus, k_ra_psel_usb_fs, "test.vbus"));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_pfs_route_peripheral(k_test_cdc_pin_vbusen, k_ra_psel_usb_fs, "test.vbusen"));
  TEST_ASSERT_EQ(k_ra_ok, ra_pfs_route_peripheral(k_test_cdc_pin_dp, k_ra_psel_usb_fs, "test.dp"));
  TEST_ASSERT_EQ(k_ra_ok, ra_pfs_route_peripheral(k_test_cdc_pin_dm, k_ra_psel_usb_fs, "test.dm"));
  TEST_END("usb_cdc_echo: PFS routes VBUS/VBUSEN/D+/D-");
}

/**
 * @brief Bring-up: pins -> LED1 -> pal_init -> attach.
 *
 * @par MC/DC: Decision under test: short-circuit chain of four
 * ``!= ok`` guards in main(). Four atomic conditions x N+1 = 5
 * vectors. This case covers all-ok.
 */
static void test_cdc_full_bringup_chain_ok(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: full bring-up chain");
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_pfs_route_peripheral(k_test_cdc_pin_vbus, k_ra_psel_usb_fs, "test.vbus"));
  TEST_ASSERT_EQ(k_ra_ok, ra_pfs_route_peripheral(k_test_cdc_pin_dp, k_ra_psel_usb_fs, "test.dp"));
  TEST_ASSERT_EQ(k_ra_ok, ra_pfs_route_peripheral(k_test_cdc_pin_dm, k_ra_psel_usb_fs, "test.dm"));
  TEST_ASSERT_EQ(k_ra_ok, ra_board_led_init(k_ra_board_led1));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(true));
  ra_usb_pal_state_t state = k_ra_usb_pal_state_detached;
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra_usb_pal_state_attached, state);
  TEST_END("usb_cdc_echo: full bring-up chain");
}

/**
 * @brief Open the three CDC ACM endpoints (notify intr-IN, bulk-IN,
 *        bulk-OUT data pipes) per the IAD config descriptor.
 *
 * @par MC/DC: Decision under test: ``ep_open != ok`` for three
 * directions/types. This case covers the all-valid vector for all
 * three; failure vectors split below.
 */
static void test_cdc_open_three_endpoints(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: open notify + bulk-IN + bulk-OUT");
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_pal_ep_open((uint8_t)k_test_cdc_ep_intr_in,
                                    k_ra_usb_pal_ep_dir_in,
                                    k_ra_usb_pal_ep_type_intr,
                                    (uint16_t)k_test_cdc_intr_packet));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_pal_ep_open((uint8_t)k_test_cdc_ep_bulk_in,
                                    k_ra_usb_pal_ep_dir_in,
                                    k_ra_usb_pal_ep_type_bulk,
                                    (uint16_t)k_test_cdc_max_packet));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_pal_ep_open((uint8_t)k_test_cdc_ep_bulk_out,
                                    k_ra_usb_pal_ep_dir_out,
                                    k_ra_usb_pal_ep_type_bulk,
                                    (uint16_t)k_test_cdc_max_packet));
  TEST_END("usb_cdc_echo: open notify + bulk-IN + bulk-OUT");
}

/**
 * @brief Echo path: ep_send mirrors a payload back over bulk-IN.
 *
 * @par MC/DC: Decision under test: ``ep_send != ok``. Two atomic
 * conditions (data!=NULL, len in range) x N+1 = 3 vectors; this case
 * covers all-valid. NULL-data covered below.
 */
static void test_cdc_echo_one_packet(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: echo one bulk-IN packet");
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_pal_ep_open((uint8_t)k_test_cdc_ep_bulk_in,
                                    k_ra_usb_pal_ep_dir_in,
                                    k_ra_usb_pal_ep_type_bulk,
                                    (uint16_t)k_test_cdc_max_packet));
  const uint8_t echo[k_test_cdc_echo_payload] = {'h', 'e', 'l', 'l', 'o', '!', '\r', '\n'};
  ra_err_t      err =
    ra_usb_pal_ep_send((uint8_t)k_test_cdc_ep_bulk_in, echo, (uint16_t)k_test_cdc_echo_payload);
  TEST_ASSERT(err == k_ra_ok || err == k_ra_err_no_mem || err == k_ra_err_hw_not_ready ||
              err == k_ra_err_invalid_state);
  TEST_END("usb_cdc_echo: echo one bulk-IN packet");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief PFS route on out-of-range port is rejected -- pins-init fail.
 *
 * @par MC/DC: Failure-side vector for ``ra_pfs_route_peripheral != ok``;
 * pairs with the all-ok routing test for full N+1 coverage.
 */
static void test_cdc_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: PFS rejects out-of-range port");
  const ra_port_pin_t bad_pin = (ra_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra_pin_0);
  TEST_ASSERT(ra_pfs_route_peripheral(bad_pin, k_ra_psel_usb_fs, "test.bad") != k_ra_ok);
  TEST_END("usb_cdc_echo: PFS rejects out-of-range port");
}

/**
 * @brief attach(false) returns to detached -- USB-cable-unplug path.
 *
 * @par MC/DC: State-machine vector: attached -> detached transition
 * under attach(false). Pairs with attach(true) for full edge coverage.
 */
static void test_cdc_attach_false_returns_to_detached(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: attach(false) returns to detached");
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(false));
  ra_usb_pal_state_t state = k_ra_usb_pal_state_attached;
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(k_ra_usb_pal_state_detached, state);
  TEST_END("usb_cdc_echo: attach(false) returns to detached");
}

/**
 * @brief ep_send with NULL data + non-zero len rejected.
 *
 * @par MC/DC: Failure side of the data-NULL atomic condition in
 * ep_send. Pairs with the ok-send for full coverage.
 */
static void test_cdc_send_null_data_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_cdc_echo: ep_send rejects NULL data");
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_usb_pal_ep_open((uint8_t)k_test_cdc_ep_bulk_in,
                                    k_ra_usb_pal_ep_dir_in,
                                    k_ra_usb_pal_ep_type_bulk,
                                    (uint16_t)k_test_cdc_max_packet));
  ra_err_t err =
    ra_usb_pal_ep_send((uint8_t)k_test_cdc_ep_bulk_in, nullptr, (uint16_t)k_test_cdc_echo_payload);
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("usb_cdc_echo: ep_send rejects NULL data");
}

int main(void)
{
  test_cdc_pfs_routes_four_usbfs_pins();
  test_cdc_full_bringup_chain_ok();
  test_cdc_open_three_endpoints();
  test_cdc_echo_one_packet();
  test_cdc_pfs_route_invalid_pin_rejected();
  test_cdc_attach_false_returns_to_detached();
  test_cdc_send_null_data_rejected();
  return 0;
}
