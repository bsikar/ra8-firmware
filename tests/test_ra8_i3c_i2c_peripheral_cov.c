/**
 * @file test_ra8_i3c_i2c_peripheral_cov.c
 * @brief Coverage-extension tests for the IIC_B peripheral driver.
 *
 * @details
 * Targets the branches in ra8_i3c_i2c_peripheral.c that are not reached
 * by test_ra8_i3c_i2c_peripheral.c:
 *
 *  - address-range guard in internal_i3c_i2c_peripheral_open (line 99)
 *  - channel out-of-range guard in close  (line 114)
 *  - channel out-of-range guard in send   (line 126)
 *  - channel out-of-range guard in receive (line 146)
 *  - channel out-of-range guard in status  (line 167)
 *  - BST.SPCNDDF stop-bit path in status  (lines 179-180)
 *  - NTST poll-timeout path in internal_wait_ntst (lines 86-87)
 *
 * gcovr merges the .gcda produced by this executable with the one from
 * test_ra8_i3c_i2c_peripheral, so coverage from both binaries accumulates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c_i2c_peripheral.h"
#include "ra8_i3c_i2c_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_i3c_i2c_peripheral_cov_const_t
 * @brief Numeric constants used by the coverage-extension tests.
 *
 * @details
 * Channel 0 is the only valid channel on the RA8D2; channel 9 is
 * an intentionally out-of-range index. Address 0x42 fits in 7 bits;
 * 0x80 overflows the 7-bit field ceiling (0x7F) and must be rejected.
 */
typedef enum : uint32_t {
  k_cov_ch0      = 0U,    /**< Valid channel index.                       */
  k_cov_ch_oor   = 9U,    /**< Out-of-range channel index.                */
  k_cov_addr_ok  = 0x42U, /**< 7-bit address that passes the range check. */
  k_cov_addr_oor = 0x80U, /**< Address that exceeds the 0x7F ceiling.     */
  k_cov_byte_a   = 0xA5U, /**< Arbitrary data byte for send tests.        */
} ra8_i3c_i2c_peripheral_cov_const_t;

/**
 * @test test_cov_open_addr_oor
 * @brief Verify that an address > 0x7F is rejected by open.
 *
 * @details
 * Exercises the `cfg->peripheral_addr_7b > k_ra8_i3c_i2c_peripheral_max_addr_7b`
 * guard inside internal_i3c_i2c_peripheral_open. Passing 0x80 must yield
 * k_ra8_err_invalid_arg without touching the hardware registers.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @post No register side-effects (guard fires before any register write).
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `cfg->peripheral_addr_7b > k_ra8_i3c_i2c_peripheral_max_addr_7b`
 * (single condition, no compound boolean).
 * - Vector 1: addr=0x42 -> dec F (k_ra8_ok)              [test_open_close]
 * - Vector 2: addr=0x80 -> dec T (k_ra8_err_invalid_arg) [this test]
 * N+1 = 2 satisfied jointly across the suite.
 *
 * @since 0.1.0
 */
static void test_cov_open_addr_oor(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_open addr > 0x7F");
  ra8_i3c_i2c_peripheral_cfg_t cfg_oor = {
    .peripheral_addr_7b = (uint8_t)k_cov_addr_oor,
    .general_call       = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_peripheral_open((uint8_t)k_cov_ch0, &cfg_oor));
  TEST_END("internal_i3c_i2c_peripheral_open addr > 0x7F");
}

/**
 * @test test_cov_close_oor_channel
 * @brief Verify that an out-of-range channel is rejected by close.
 *
 * @details
 * Exercises the `reg == nullptr` guard in internal_i3c_i2c_peripheral_close.
 * Channel index 9 has no hardware mapping on the RA8D2 (only channel 0
 * exists), so i3c_i2c_regs returns nullptr and the function returns
 * k_ra8_err_invalid_arg immediately.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @post No register side-effects.
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `reg == nullptr` (single condition).
 * - Vector 1: ch=0  -> dec F (k_ra8_ok)              [test_open_close]
 * - Vector 2: ch=9  -> dec T (k_ra8_err_invalid_arg) [this test]
 * N+1 = 2 satisfied jointly.
 *
 * @since 0.1.0
 */
static void test_cov_close_oor_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_close oor channel");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_peripheral_close((uint8_t)k_cov_ch_oor));
  TEST_END("internal_i3c_i2c_peripheral_close oor channel");
}

/**
 * @test test_cov_send_oor_channel
 * @brief Verify that an out-of-range channel is rejected by send.
 *
 * @details
 * Exercises the `reg == nullptr` guard in internal_i3c_i2c_peripheral_send.
 * The guard fires before any null-pointer or length check, so no data
 * pre-arming is required.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @post No register side-effects.
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `reg == nullptr` (single condition).
 * - Vector 1: ch=0  -> dec F (k_ra8_ok)              [test_send_ok]
 * - Vector 2: ch=9  -> dec T (k_ra8_err_invalid_arg) [this test]
 * N+1 = 2 satisfied jointly.
 *
 * @since 0.1.0
 */
static void test_cov_send_oor_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_send oor channel");
  const uint8_t data[1] = {(uint8_t)k_cov_byte_a};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_peripheral_send((uint8_t)k_cov_ch_oor, data, 1U));
  TEST_END("internal_i3c_i2c_peripheral_send oor channel");
}

/**
 * @test test_cov_receive_oor_channel
 * @brief Verify that an out-of-range channel is rejected by receive.
 *
 * @details
 * Exercises the `reg == nullptr` guard in internal_i3c_i2c_peripheral_receive.
 * The guard fires before any null-pointer or length check.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @post No register side-effects.
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `reg == nullptr` (single condition).
 * - Vector 1: ch=0  -> dec F (k_ra8_ok)              [test_receive_ok]
 * - Vector 2: ch=9  -> dec T (k_ra8_err_invalid_arg) [this test]
 * N+1 = 2 satisfied jointly.
 *
 * @since 0.1.0
 */
static void test_cov_receive_oor_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_receive oor channel");
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_peripheral_receive((uint8_t)k_cov_ch_oor, buf, 1U));
  TEST_END("internal_i3c_i2c_peripheral_receive oor channel");
}

/**
 * @test test_cov_status_oor_channel
 * @brief Verify that an out-of-range channel is rejected by status.
 *
 * @details
 * Exercises the `reg == nullptr` guard in internal_i3c_i2c_peripheral_status.
 * A valid (non-null) out_mask pointer is supplied so RA8_CHECK_NULL_PTR
 * does not fire first, allowing execution to reach the channel-range guard.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @post No register side-effects.
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `reg == nullptr` (single condition).
 * - Vector 1: ch=0  -> dec F (k_ra8_ok)              [test_status]
 * - Vector 2: ch=9  -> dec T (k_ra8_err_invalid_arg) [this test]
 * N+1 = 2 satisfied jointly.
 *
 * @since 0.1.0
 */
static void test_cov_status_oor_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_status oor channel");
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_peripheral_status((uint8_t)k_cov_ch_oor, &mask));
  TEST_END("internal_i3c_i2c_peripheral_status oor channel");
}

/**
 * @test test_cov_status_stop_bit
 * @brief Verify that BST.SPCNDDF sets the stop bit in the status mask.
 *
 * @details
 * Exercises the `(bst & k_ra8_i3c_i2c_msk_bst_spcnddf) != 0U` branch
 * in internal_i3c_i2c_peripheral_status. Pre-seeding BST with
 * k_ra8_i3c_i2c_msk_bst_spcnddf (bit 1, representing a Stop Condition
 * Detected Flag) causes the status function to OR in
 * k_ra8_i3c_i2c_peripheral_status_stop. The existing test_status covers
 * only the NACKDF bit and leaves this if-arm uncovered.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @pre ra8_fake_mmap_reset() clears all backing registers to zero.
 * @post mask includes k_ra8_i3c_i2c_peripheral_status_stop.
 *
 * @note Not thread-safe; tests are single-threaded.
 *
 * @par MC/DC:
 * Decision: `(bst & k_ra8_i3c_i2c_msk_bst_spcnddf) != 0U` (single condition).
 * - Vector 1: BST.SPCNDDF=0 -> dec F (stop bit absent) [test_status in sibling]
 * - Vector 2: BST.SPCNDDF=1 -> dec T (stop bit set)    [this test]
 * N+1 = 2 satisfied jointly.
 *
 * @since 0.1.0
 */
static void test_cov_status_stop_bit(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_status BST.SPCNDDF -> stop");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_cov_ch0);
  /* Stop-condition-detected flag (BST bit 1). HUM Ch 40.2 "BST" p 2482 */
  reg->BST     = (uint32_t)k_ra8_i3c_i2c_msk_bst_spcnddf;
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_peripheral_status((uint8_t)k_cov_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_stop);
  TEST_END("internal_i3c_i2c_peripheral_status BST.SPCNDDF -> stop");
}

/**
 * @test test_cov_send_timeout
 * @brief Verify that an unasserted NTST.TDBEF0 causes a hw-timeout return.
 *
 * @details
 * Exercises the timeout path in internal_wait_ntst (lines 86-87).
 * With NTST.TDBEF0 (bit 0) never asserted, the bounded spin runs
 * k_ra8_i3c_i2c_peripheral_spin_budget (50000) iterations and
 * internal_wait_ntst returns k_ra8_err_hw_timeout. RA8_RETURN_ON_ERROR
 * inside internal_i3c_i2c_peripheral_send propagates that code back
 * to the caller.
 *
 * The spin executes 50000 volatile host-RAM reads (approximately 250 us
 * on a modern CPU), which is well within the test time budget and does
 * not use any signal/alarm mechanism. NTST stays zero after
 * ra8_fake_mmap_reset(); no register pre-seeding is required to reach
 * the timeout path.
 *
 * @pre ra8_i3c_i2c_peripheral module compiled with RA8_OFF_TARGET.
 * @pre ra8_fake_mmap_reset() clears NTST to zero.
 * @post Function returns k_ra8_err_hw_timeout.
 *
 * @note Not thread-safe; tests are single-threaded.
 * @note The spin is deterministic: it always takes exactly
 *       k_ra8_i3c_i2c_peripheral_spin_budget iterations before returning.
 *
 * @par MC/DC:
 * Decision (loop exhaustion): `i < k_ra8_i3c_i2c_peripheral_spin_budget`
 * with TDBEF0 never set -- the loop runs to completion. Branch annotations
 * on the source side (GCOVR_EXCL_BR_LINE) suppress branch-level counting
 * for the loop and inner if; the line-level counters for lines 86-87 are
 * covered by this test.
 * - Vector 1: NTST.TDBEF0 set before budget exhausts -> k_ra8_ok [test_send_ok]
 * - Vector 2: NTST.TDBEF0 never set, budget exhausts -> k_ra8_err_hw_timeout
 *             [this test]
 * N+1 = 2 satisfied jointly across the suite.
 *
 * @since 0.1.0
 */
static void test_cov_send_timeout(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_send NTST timeout");
  ra8_fake_mmap_reset();
  /* NTST.TDBEF0 is zero after reset; withhold pre-arming so the spin exhausts. */
  const uint8_t data[1] = {(uint8_t)k_cov_byte_a};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_i3c_i2c_peripheral_send((uint8_t)k_cov_ch0, data, 1U));
  TEST_END("internal_i3c_i2c_peripheral_send NTST timeout");
}

/**
 * @brief Test entry point.
 *
 * @details
 * Runs all coverage-extension test cases sequentially. Returns 0 on
 * success; individual tests call exit(1) on first assertion failure.
 *
 * @return int Always 0 when all tests pass.
 *
 * @since 0.1.0
 */
int main(void)
{
  test_cov_open_addr_oor();
  test_cov_close_oor_channel();
  test_cov_send_oor_channel();
  test_cov_receive_oor_channel();
  test_cov_status_oor_channel();
  test_cov_status_stop_bit();
  test_cov_send_timeout();
  return 0;
}
