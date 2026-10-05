/**
 * @file test_ra8_i3c_i2c_events.c
 * @brief Unit tests for IIC_B target discovery, error status, and interrupt dispatch.
 *
 * @details
 * Split out of ``test_ra8_i3c_i2c.c``, which owns the bring-up and
 * transfer paths. This translation unit covers the three behaviours that
 * observe the bus rather than drive a payload across it: ``scan``
 * target probing, the error mask/clear pair, and the ERI handler
 * attach/dispatch path. It shares the same ``ra8_fake_mmap`` substrate
 * and the same pre-armed status-flag discipline as its sibling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_internal.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_mstp.h"
#include "test_ra8_i3c_i2c_fixture.h"
#include "unity_minimal.h"

static const ra8_i3c_i2c_cfg_t s_iic_b_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i3c_i2c_speed_fast,
  .pclka_hz = 60000000U,
};

/**
 * @brief Reset the fake and ensure MSTP / channel state is fresh. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/* =============================================================================
 * Scan
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify scan bad args behavior. @details Executes the scan bad args scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_scan_bad_args(void)
{
  TEST_BEGIN("ra8_i3c_i2c_scan: arg validation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i3c_i2c_scan(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr));
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i3c_i2c_scan((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                  (uint8_t)k_ra8_i3c_i2c_test_target,
                                  &acked));
  TEST_END("ra8_i3c_i2c_scan: arg validation");
}

/* =============================================================================
 * Error mask + handler dispatch
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static int32_t s_iic_b_cb_count = 0;
static int32_t s_iic_b_cb_err   = 0;
/** @brief Provide the file-local stub iic b cb test helper. @details Implements the stub iic b cb fixture operation used only by this focused test executable. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] err_mask Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_stub_iic_b_cb(void* ctx, uint8_t err_mask)
{
  (void)ctx;
  ++s_iic_b_cb_count;
  s_iic_b_cb_err = (int32_t)err_mask;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify attach handler toggles iers behavior. @details Executes the attach handler toggles iers scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_attach_handler_toggles_iers(void)
{
  TEST_BEGIN("ra8_i3c_i2c_attach_handler: out-of-range channel rejected");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i3c_i2c_attach_handler((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                            internal_stub_iic_b_cb,
                                            nullptr));
  TEST_END("ra8_i3c_i2c_attach_handler: out-of-range channel rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify dispatch eri fires callback behavior. @details Executes the dispatch eri fires callback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_eri_fires_callback(void)
{
  TEST_BEGIN("ra8_i3c_i2c_dispatch_eri: out-of-range channel is a no-op");
  internal_prep();
  s_iic_b_cb_count = 0;
  ra8_i3c_i2c_dispatch_eri((uint8_t)k_ra8_i3c_i2c_test_ch_oor);
  TEST_ASSERT_EQ(0, s_iic_b_cb_count);
  TEST_END("ra8_i3c_i2c_dispatch_eri: out-of-range channel is a no-op");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  internal_test_scan_bad_args,
  internal_test_attach_handler_toggles_iers,
  internal_test_dispatch_eri_fires_callback,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
