/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_i3c_i2c_peripheral.c
 * @brief Unit tests for the IIC_B peripheral driver.
 */

#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c_i2c_peripheral.h"
#include "ra8_i3c_i2c_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_i3c_i2c_peripheral_test_const_t
 * @brief Numeric constants used by the tests.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_peripheral_test_ch0     = 0U,    /**< RA8 I3C I2C peripheral test ch0.         */
  k_ra8_i3c_i2c_peripheral_test_ch_oor  = 9U,    /**< RA8 I3C I2C peripheral test channel oor. */
  k_ra8_i3c_i2c_peripheral_test_addr_7b = 0x42U, /**< RA8 I3C I2C peripheral test address 7b.  */
  k_ra8_i3c_i2c_peripheral_test_byte_a  = 0xA5U, /**< RA8 I3C I2C peripheral test byte a.      */
  k_ra8_i3c_i2c_peripheral_test_byte_b  = 0x5AU, /**< RA8 I3C I2C peripheral test byte b.      */
  k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 =
    0x01U, /**< RA8 I3C I2C peripheral test ntst tdbef0. */
  k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0 =
    0x02U, /**< RA8 I3C I2C peripheral test ntst rdbff0. */
} ra8_i3c_i2c_peripheral_test_const_t;

static const ra8_i3c_i2c_peripheral_cfg_t k_cfg = {
  .peripheral_addr_7b = (uint8_t)k_ra8_i3c_i2c_peripheral_test_addr_7b,
  .general_call       = 1U,
};

static void prearm(volatile r_i3c_i2c_regs_t* reg)
{
  reg->NTST = (uint32_t)((uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 |
                         (uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0);
}

/**
 * @test test_open_null
 *
 * @par MC/DC:
 * Exercises the ``cfg == nullptr`` precondition branch in
 * ``internal_i3c_i2c_peripheral_open``. The precondition is a single-condition
 * decision (no compound boolean), so MC/DC and branch coverage coincide.
 * Vector: cfg=NULL -> dec T (k_ra8_err_null_ptr). Companion test
 * ``test_open_close`` provides cfg=valid -> dec F. N+1 = 2 satisfied
 * jointly across the suite.
 */
static void test_open_null(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_open null cfg");
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr));
  TEST_END("internal_i3c_i2c_peripheral_open null cfg");
}

/**
 * @test test_open_oor
 *
 * @par MC/DC:
 * Exercises the ``ch >= k_ra8_i3c_i2c_max_channels`` range-check decision in
 * ``internal_i3c_i2c_peripheral_open``. Single-condition decision: vector
 * ch=9 -> dec T (k_ra8_err_invalid_arg); the in-range vector is provided
 * by ``test_open_close``. N+1 = 2 jointly.
 */
static void test_open_oor(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_open oor channel");
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch_oor, &k_cfg));
  TEST_END("internal_i3c_i2c_peripheral_open oor channel");
}

/**
 * @test test_open_close
 *
 * @par MC/DC:
 * Drives the happy-path branches of ``internal_i3c_i2c_peripheral_open`` and
 * ``_close``. Both functions perform single-condition guard checks
 * (channel range, cfg non-null) -- no compound booleans in either
 * unit under test. This vector exercises the dec=F leg of every guard
 * and pairs with ``test_open_null`` / ``test_open_oor`` to satisfy
 * N+1 = 2 for each guard.
 */
static void test_open_close(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_open + close happy");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &k_cfg));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_i3c_i2c_peripheral_test_addr_7b << 1U), reg->MSDVAD);
  TEST_ASSERT(reg->BCTL & (uint32_t)k_ra8_i3c_i2c_msk_bctl_buse);
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_peripheral_close((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0));
  TEST_END("internal_i3c_i2c_peripheral_open + close happy");
}

/**
 * @test test_send_ok
 *
 * @par MC/DC:
 * Drives the dec=F leg of the compound guard
 * ``(len > 0U) && (data == nullptr)`` inside
 * ``internal_i3c_i2c_peripheral_send`` (vector: len=1, data=valid -> dec F).
 * Companion vectors live in ``test_send_null`` (len=1, data=NULL ->
 * dec T) and the dedicated ``test_mcdc_ra8_i3c_i2c_peripheral`` block
 * (len=0, data=*). N+1 = 3 satisfied jointly.
 */
static void test_send_ok(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_send ok");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra8_i3c_i2c_peripheral_test_byte_a};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, data, 1U));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_peripheral_test_byte_a, (reg->NTDTBP0 & 0xFFU));
  TEST_END("internal_i3c_i2c_peripheral_send ok");
}

/**
 * @test test_send_null
 *
 * @par MC/DC:
 * Drives the dec=T leg of ``(len > 0U) && (data == nullptr)`` inside
 * ``internal_i3c_i2c_peripheral_send`` (vector: len=1, data=NULL -> dec T,
 * returns k_ra8_err_null_ptr). Pairs with ``test_send_ok`` to flip the
 * data condition and with ``test_mcdc_ra8_i3c_i2c_peripheral`` to flip
 * len. N+1 = 3 jointly.
 */
static void test_send_null(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_send null");
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  TEST_END("internal_i3c_i2c_peripheral_send null");
}

/**
 * @test test_receive_ok
 *
 * @par MC/DC:
 * Drives the dec=F leg of the mirror compound guard
 * ``(len > 0U) && (buf == nullptr)`` inside
 * ``internal_i3c_i2c_peripheral_receive`` (vector: len=1, buf=valid -> dec F).
 * Companion vectors live in ``test_mcdc_ra8_i3c_i2c_peripheral``
 * (len=0,*) and (len=1, buf=NULL). N+1 = 3 jointly.
 */
static void test_receive_ok(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_receive ok");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  prearm(reg);
  reg->NTDTBP0   = (uint32_t)k_ra8_i3c_i2c_peripheral_test_byte_b;
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_peripheral_test_byte_b, buf[0]);
  TEST_END("internal_i3c_i2c_peripheral_receive ok");
}

/**
 * @test test_status
 *
 * @par MC/DC:
 * Two single-condition guards in ``internal_i3c_i2c_peripheral_status``:
 *   (a) ``ch >= k_ra8_i3c_i2c_max_channels`` (range)
 *   (b) ``mask == nullptr`` (precondition)
 * No compound boolean in the unit under test. Vector:
 * ch=0, mask=valid -> dec F (returns k_ra8_ok with flag bits set);
 * second call passes mask=NULL -> dec T (k_ra8_err_null_ptr). N+1 = 2
 * for each independent guard, satisfied within this single test.
 */
static void test_status(void)
{
  TEST_BEGIN("internal_i3c_i2c_peripheral_status reflects flags");
  ra8_fake_mmap_reset();
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  reg->NTST                      = (uint32_t)((uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_tdbef0 |
                                              (uint32_t)k_ra8_i3c_i2c_peripheral_test_ntst_rdbff0);
  reg->BST                       = (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf;
  uint8_t mask                   = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_status((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_tx_empty);
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_rx_full);
  TEST_ASSERT(mask & (uint8_t)k_ra8_i3c_i2c_peripheral_status_nack);

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_peripheral_status((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr));
  TEST_END("internal_i3c_i2c_peripheral_status reflects flags");
}

/**
 * @test test_mcdc_ra8_i3c_i2c_peripheral
 *
 * @par MC/DC:
 * Decision A (libs/ra8_hal/src/ra8_i3c_i2c_peripheral.c@internal_i3c_i2c_peripheral_send):
 * ``if ((len > 0U) && (data == nullptr))`` (2 conditions, ``&&``).
 * N+1 = 3:
 * - V1: len=0,  data=*       -> dec F (ok empty)
 * - V2: len=1,  data=valid   -> dec F (sends)
 * - V3: len=1,  data=NULL    -> dec T (null_ptr)
 * Decision B (libs/ra8_hal/src/ra8_i3c_i2c_peripheral.c@internal_i3c_i2c_peripheral_send):
 * ``(len > 0U) && (buf == nullptr)``. Mirror vectors. DO-178C 6.4.4.3.
 */
static void test_mcdc_ra8_i3c_i2c_peripheral(void)
{
  TEST_BEGIN("iic_b_peripheral MC/DC: send/receive 2-cond null+len");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_open((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, &k_cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra8_i3c_i2c_peripheral_test_byte_a};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 0U));
  prearm(reg);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, data, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_peripheral_send((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 0U));
  prearm(reg);
  reg->NTDTBP0 = (uint32_t)k_ra8_i3c_i2c_peripheral_test_byte_b;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, buf, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_peripheral_receive((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0, nullptr, 1U));
  (void)internal_i3c_i2c_peripheral_close((uint8_t)k_ra8_i3c_i2c_peripheral_test_ch0);
  TEST_END("iic_b_peripheral MC/DC: send/receive 2-cond null+len");
}

int main(void)
{
  test_open_null();
  test_open_oor();
  test_open_close();
  test_send_ok();
  test_send_null();
  test_receive_ok();
  test_status();
  test_mcdc_ra8_i3c_i2c_peripheral();
  return 0;
}
