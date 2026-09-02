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
 * @brief Pre-arm NTST so the driver's address + data wait loops fall
 *        through immediately, and BCST.BFREF so the bus-busy gate
 *        passes. @details Implements the prime ntst fixture operation used only by this focused test executable. @param[in] channel Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prime_ntst(uint8_t channel)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  /* HUM Ch 40.2.50 "NTST : Normal Transfer Status Register" p 2498 */
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  /* HUM Ch 40.2.58 "BCST : Bus Condition Status Register" p 2512 */
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

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

/** @brief Poll hook that models a target acknowledging the address. @since 0.1.0 */
RA8_INTERNAL static void internal_i3c_scan_ack_hook(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  if (reg != nullptr) {
    /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
    reg->BST = reg->BST | (uint32_t)k_ra8_i3c_i2c_msk_bst_tendf;
  }
}

/** @brief Verify scan no response behavior. @details Executes the scan no response scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_scan_no_response(void)
{
  TEST_BEGIN("ra8_i3c_i2c_scan: no BST flag => hw_timeout, acked false");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));
  internal_prime_ntst(0U);
  /* No bus activity in the fake; the scan times out waiting for
   * either TENDF or NACKDF, which is the correct behaviour for an
   * empty bus. */
  bool acked = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i3c_i2c_scan(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &acked));
  TEST_ASSERT(!acked);
  TEST_END("ra8_i3c_i2c_scan: no BST flag => hw_timeout, acked false");
}

/** @brief A target TENDF response makes scan report an ACK. @since 0.1.0 */
RA8_INTERNAL static void internal_test_scan_ack(void)
{
  TEST_BEGIN("ra8_i3c_i2c_scan: TENDF reports acked");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));
  internal_prime_ntst(0U);
  ra8_fake_mmio_set_poll_hook(internal_i3c_scan_ack_hook);
  bool            acked = false;
  const ra8_err_t err   = ra8_i3c_i2c_scan(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &acked);
  ra8_fake_mmio_set_poll_hook(nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT(acked);
  TEST_END("ra8_i3c_i2c_scan: TENDF reports acked");
}

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

/** @brief Verify errors mask and clear behavior. @details Executes the errors mask and clear scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_errors_mask_and_clear(void)
{
  TEST_BEGIN("internal_i3c_i2c_get/clear_errors: AL + NACK + TODF");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  reg->BST = (uint32_t)k_ra8_i3c_i2c_msk_bst_alf | (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf |
             (uint32_t)k_ra8_i3c_i2c_msk_bst_todf;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_get_errors(0U, &mask));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_i3c_i2c_err_arb_lost | (uint8_t)k_ra8_i3c_i2c_err_nack |
                  (uint8_t)k_ra8_i3c_i2c_err_timeout),
                 mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_clear_errors(0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_get_errors(0U, &mask));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_err_none, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i3c_i2c_get_errors(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i3c_i2c_get_errors((uint8_t)k_ra8_i3c_i2c_test_ch_oor, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i3c_i2c_clear_errors((uint8_t)k_ra8_i3c_i2c_test_ch_oor));
  TEST_END("internal_i3c_i2c_get/clear_errors: AL + NACK + TODF");
}

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
  TEST_BEGIN("ra8_i3c_i2c_attach_handler: BIE+NTIE toggled");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_attach_handler(0U, internal_stub_iic_b_cb, nullptr));
  /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
  TEST_ASSERT(i3c_i2c_regs(0U)->BIE != 0U);
  /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable Register" p 2504 */
  TEST_ASSERT(i3c_i2c_regs(0U)->NTIE != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_attach_handler(0U, nullptr, nullptr));
  /* HUM Ch 40.2.48 "BIE : Bus Interrupt Enable Register" p 2495 */
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->BIE);
  /* HUM Ch 40.2.52 "NTIE : Normal Transfer Interrupt Enable Register" p 2504 */
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->NTIE);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i3c_i2c_attach_handler((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                            internal_stub_iic_b_cb,
                                            nullptr));
  TEST_END("ra8_i3c_i2c_attach_handler: BIE+NTIE toggled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify dispatch eri fires callback behavior. @details Executes the dispatch eri fires callback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_eri_fires_callback(void)
{
  TEST_BEGIN("ra8_i3c_i2c_dispatch_eri: latched NACKDF -> callback fires");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_init(0U, &s_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_i2c_attach_handler(0U, internal_stub_iic_b_cb, nullptr));

  s_iic_b_cb_count = 0;
  s_iic_b_cb_err   = 0;
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  i3c_i2c_regs(0U)->BST = (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf;
  ra8_i3c_i2c_dispatch_eri(0U);
  TEST_ASSERT_EQ(1, s_iic_b_cb_count);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_err_nack, s_iic_b_cb_err);

  /* Zero mask must not fire the callback. */
  s_iic_b_cb_count = 0;
  /* HUM Ch 40.2.46 "BST : Bus Status Register" p 2490 */
  i3c_i2c_regs(0U)->BST = 0U;
  ra8_i3c_i2c_dispatch_eri(0U);
  TEST_ASSERT_EQ(0, s_iic_b_cb_count);

  /* Out-of-range channel is a no-op. */
  ra8_i3c_i2c_dispatch_eri((uint8_t)k_ra8_i3c_i2c_test_ch_oor);
  TEST_END("ra8_i3c_i2c_dispatch_eri: latched NACKDF -> callback fires");
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
  internal_test_scan_no_response,
  internal_test_scan_bad_args,
  internal_test_errors_mask_and_clear,
  internal_test_scan_ack,
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
