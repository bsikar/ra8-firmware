/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_usb_host_msc_browse.c
 * @brief Integration test: USB host MSC bring-up + SCSI browse path
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/usb_host_msc_browse/main.c
 * brings up CGC, routes the USBHS_VBUS-sense pin (P4_08, PSEL=0x14),
 * arms LED1, flips USBHS to host mode via ``ra8_usb_hmsc_init(HS)``,
 * registers an attach callback, and on attach drives a sequence of
 * SCSI commands (INQUIRY -> READ_CAPACITY(10) -> READ(10) of LBA 0)
 * to "browse" the device. This test exercises the same module
 * surface (PFS routing + ra8_usb_hmsc init/attach/inquiry/read_capacity
 * /read10) under mocked MMIO.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_usb_hmsc.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_hmsc_psel_usbhs  = 0x14U,   /**< Test hmsc psel usbhs.  */
  k_test_hmsc_target_lun  = 0U,      /**< Test hmsc target lun.  */
  k_test_hmsc_block_size  = 512U,    /**< Test hmsc block size.  */
  k_test_hmsc_ctx_token   = 0x484DU, /**< 'HM'                   */
  k_test_hmsc_bogus_speed = 9U,      /**< Test hmsc bogus speed. */
} test_hmsc_const_t;

/** @brief Captured attach state. */
static uintptr_t             s_test_hmsc_attach_ctx;
static ra8_usb_hmsc_device_t s_test_hmsc_attach_device;

/** @brief Mock attach callback -- mirrors usb_msc_on_attach. */
static void test_hmsc_on_attach(void* ctx, const ra8_usb_hmsc_device_t* device)
{
  s_test_hmsc_attach_ctx = (uintptr_t)ctx;
  if (device != nullptr) {
    s_test_hmsc_attach_device = *device;
  }
}

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_usb_hmsc_close();
  s_test_hmsc_attach_ctx    = 0U;
  s_test_hmsc_attach_device = (ra8_usb_hmsc_device_t){};
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the VBUS-sense PFS routing the app does at boot.
 *
 * @par MC/DC: Decision under test (in app): the
 * ``ra8_pfs_route_peripheral != ok`` short-circuit guard. Two atomic
 * conditions x N+1 = 3 vectors -- this case covers the ok vector;
 * failure vector covered below.
 */
static void test_hmsc_pfs_routes_vbus_sense(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: PFS routes USBHS_VBUS sense");
  const ra8_port_pin_t pin =
    (ra8_port_pin_t)(((uint16_t)k_ra8_port_4 << 8) | (uint16_t)k_ra8_pin_8);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_pfs_route_peripheral(pin, (ra8_psel_t)k_test_hmsc_psel_usbhs, "test.vbus_sns"));
  TEST_END("usb_host_msc_browse: PFS routes USBHS_VBUS sense");
}

/**
 * @brief Init the host MSC driver at HS (the app's selected speed).
 *
 * @par MC/DC: Decision under test: ``ra8_usb_hmsc_init() != k_ra8_ok``.
 * Two vectors: speed=hs (this test) + invalid-speed (rejection test).
 */
static void test_hmsc_init_high_speed(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: hmsc_init HS");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_hs));
  TEST_END("usb_host_msc_browse: hmsc_init HS");
}

/**
 * @brief init -> attach_callback chain (post-attach the app drives
 *        INQUIRY/READ_CAPACITY/READ(10)).
 *
 * @par MC/DC: Decision under test: short-circuit chain of two
 * ``!= ok`` guards. Two atomic conditions x N+1 = 3 vectors; this
 * case covers all-ok.
 */
static void test_hmsc_init_attach_callback_chain(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: init + attach_callback");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hmsc_attach_callback(test_hmsc_on_attach, (void*)k_test_hmsc_ctx_token));
  TEST_END("usb_host_msc_browse: init + attach_callback");
}

/**
 * @brief INQUIRY + READ_CAPACITY(10) call-shape (browse phase 1).
 *
 * @par MC/DC: Decision under test: ``inquiry != ok`` short-circuited
 * with ``read_capacity != ok``. Without an attached device both calls
 * report not_ready / invalid_state; the test verifies neither rejects
 * legal arguments with invalid_arg.
 */
static void test_hmsc_inquiry_and_read_capacity_shape(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: inquiry + read_capacity call-shape");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_hs));
  ra8_usb_hmsc_inquiry_response_t resp = {};
  ra8_err_t                       e1 = ra8_usb_hmsc_inquiry((uint8_t)k_test_hmsc_target_lun, &resp);
  TEST_ASSERT(e1 != k_ra8_err_invalid_arg);
  uint32_t  blocks = 0U;
  uint32_t  bsize  = 0U;
  ra8_err_t e2     = ra8_usb_hmsc_read_capacity((uint8_t)k_test_hmsc_target_lun, &blocks, &bsize);
  TEST_ASSERT(e2 != k_ra8_err_invalid_arg);
  TEST_END("usb_host_msc_browse: inquiry + read_capacity call-shape");
}

/**
 * @brief READ(10) of LBA 0 / 1 block -- browse phase 2.
 *
 * @par MC/DC: Decision under test: ``read10 != ok``. Without an
 * attached device the call returns not_ready; the test verifies legal
 * args are not rejected for the wrong reason.
 */
static void test_hmsc_read10_lba0_call_shape(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: read10 LBA0 call-shape");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_hs));
  uint8_t   buf[k_test_hmsc_block_size] = {};
  ra8_err_t err = ra8_usb_hmsc_read10((uint8_t)k_test_hmsc_target_lun, 0U, 1U, buf);
  TEST_ASSERT(err != k_ra8_err_invalid_arg);
  TEST_END("usb_host_msc_browse: read10 LBA0 call-shape");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Bogus speed rejected by hmsc_init.
 *
 * @par MC/DC: Failure vector for the speed-validation atomic condition.
 */
static void test_hmsc_init_bad_speed_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: hmsc_init rejects bogus speed");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_init((ra8_usb_speed_t)k_test_hmsc_bogus_speed));
  TEST_END("usb_host_msc_browse: hmsc_init rejects bogus speed");
}

/**
 * @brief close before init is rejected (worker-thread orderings).
 *
 * @par MC/DC: Failure side of the ``initialized == true`` invariant.
 */
static void test_hmsc_close_before_init_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: hmsc_close before init rejected");
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_close());
  TEST_END("usb_host_msc_browse: hmsc_close before init rejected");
}

/**
 * @brief PFS route on out-of-range port is rejected.
 *
 * @par MC/DC: Failure-side vector for ``ra8_pfs_route_peripheral != ok``.
 */
static void test_hmsc_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_host_msc_browse: PFS rejects out-of-range port");
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra8_pin_0);
  TEST_ASSERT(ra8_pfs_route_peripheral(bad_pin, (ra8_psel_t)k_test_hmsc_psel_usbhs, "test.bad") !=
              k_ra8_ok);
  TEST_END("usb_host_msc_browse: PFS rejects out-of-range port");
}

int main(void)
{
  test_hmsc_pfs_routes_vbus_sense();
  test_hmsc_init_high_speed();
  test_hmsc_init_attach_callback_chain();
  test_hmsc_inquiry_and_read_capacity_shape();
  test_hmsc_read10_lba0_call_shape();
  test_hmsc_init_bad_speed_rejected();
  test_hmsc_close_before_init_rejected();
  test_hmsc_pfs_route_invalid_pin_rejected();
  return 0;
}
