/**
 * @file test_ra_iic_b_slave.c
 * @brief Unit tests for the IIC_B slave driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_iic_b_regs.h"
#include "ra_err.h"
#include "ra_iic_b_slave.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_iic_b_slave_test_const_t
 * @brief Numeric constants used by the tests.
 */
typedef enum : uint32_t {
  k_ra_iic_b_slave_test_ch0         = 0U,
  k_ra_iic_b_slave_test_ch_oor      = 9U,
  k_ra_iic_b_slave_test_addr_7b     = 0x42U,
  k_ra_iic_b_slave_test_byte_a      = 0xA5U,
  k_ra_iic_b_slave_test_byte_b      = 0x5AU,
  k_ra_iic_b_slave_test_ntst_tdbef0 = 0x01U,
  k_ra_iic_b_slave_test_ntst_rdbff0 = 0x02U,
} ra_iic_b_slave_test_const_t;

static const ra_iic_b_slave_cfg_t k_cfg = {
  .slave_addr_7b = (uint8_t)k_ra_iic_b_slave_test_addr_7b,
  .general_call  = 1U,
};

static void prearm(volatile r_iic_b_regs_t* reg)
{
  reg->NTST = (uint32_t)((uint32_t)k_ra_iic_b_slave_test_ntst_tdbef0 |
                         (uint32_t)k_ra_iic_b_slave_test_ntst_rdbff0);
}

static void test_open_null(void)
{
  TEST_BEGIN("ra_iic_b_slave_open null cfg");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_open((uint8_t)k_ra_iic_b_slave_test_ch0, NULL));
  TEST_END("ra_iic_b_slave_open null cfg");
}

static void test_open_oor(void)
{
  TEST_BEGIN("ra_iic_b_slave_open oor channel");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_iic_b_slave_open((uint8_t)k_ra_iic_b_slave_test_ch_oor, &k_cfg));
  TEST_END("ra_iic_b_slave_open oor channel");
}

static void test_open_close(void)
{
  TEST_BEGIN("ra_iic_b_slave_open + close happy");
  ra_sim_mmap_reset();
  volatile r_iic_b_regs_t* reg = ra_iic_b((uint8_t)k_ra_iic_b_slave_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_open((uint8_t)k_ra_iic_b_slave_test_ch0, &k_cfg));
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_iic_b_slave_test_addr_7b << 1U), (int)reg->MSDVAD);
  TEST_ASSERT(reg->BCTL & (uint32_t)k_ra_iic_b_msk_bctl_buse);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_b_slave_close((uint8_t)k_ra_iic_b_slave_test_ch0));
  TEST_END("ra_iic_b_slave_open + close happy");
}

static void test_send_ok(void)
{
  TEST_BEGIN("ra_iic_b_slave_send ok");
  ra_sim_mmap_reset();
  volatile r_iic_b_regs_t* reg = ra_iic_b((uint8_t)k_ra_iic_b_slave_test_ch0);
  prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra_iic_b_slave_test_byte_a};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_send((uint8_t)k_ra_iic_b_slave_test_ch0, data, 1U));
  TEST_ASSERT_EQ((int)k_ra_iic_b_slave_test_byte_a, (int)(reg->NTDTBP0 & 0xFFU));
  TEST_END("ra_iic_b_slave_send ok");
}

static void test_send_null(void)
{
  TEST_BEGIN("ra_iic_b_slave_send null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_send((uint8_t)k_ra_iic_b_slave_test_ch0, NULL, 1U));
  TEST_END("ra_iic_b_slave_send null");
}

static void test_receive_ok(void)
{
  TEST_BEGIN("ra_iic_b_slave_receive ok");
  ra_sim_mmap_reset();
  volatile r_iic_b_regs_t* reg = ra_iic_b((uint8_t)k_ra_iic_b_slave_test_ch0);
  prearm(reg);
  reg->NTDTBP0   = (uint32_t)k_ra_iic_b_slave_test_byte_b;
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_receive((uint8_t)k_ra_iic_b_slave_test_ch0, buf, 1U));
  TEST_ASSERT_EQ((int)k_ra_iic_b_slave_test_byte_b, (int)buf[0]);
  TEST_END("ra_iic_b_slave_receive ok");
}

static void test_status(void)
{
  TEST_BEGIN("ra_iic_b_slave_status reflects flags");
  ra_sim_mmap_reset();
  volatile r_iic_b_regs_t* reg = ra_iic_b((uint8_t)k_ra_iic_b_slave_test_ch0);
  reg->NTST                    = (uint32_t)((uint32_t)k_ra_iic_b_slave_test_ntst_tdbef0 |
                                            (uint32_t)k_ra_iic_b_slave_test_ntst_rdbff0);
  reg->BST                     = (uint32_t)k_ra_iic_b_msk_bst_nackdf;
  uint8_t mask                 = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_status((uint8_t)k_ra_iic_b_slave_test_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra_iic_b_slave_status_tx_empty);
  TEST_ASSERT(mask & (uint8_t)k_ra_iic_b_slave_status_rx_full);
  TEST_ASSERT(mask & (uint8_t)k_ra_iic_b_slave_status_nack);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_status((uint8_t)k_ra_iic_b_slave_test_ch0, NULL));
  TEST_END("ra_iic_b_slave_status reflects flags");
}

/**
 * @test test_mcdc_ra_iic_b_slave
 *
 * @par MC/DC:
 * Decision A: ``ra_iic_b_slave_send`` line 113,
 * libs/ra_hal/src/ra_iic_b_slave.c:
 * ``if ((len > 0U) && (data == nullptr))`` (2 conditions, ``&&``).
 * N+1 = 3:
 * - V1: len=0,  data=*       -> dec F (ok empty)
 * - V2: len=1,  data=valid   -> dec F (sends)
 * - V3: len=1,  data=NULL    -> dec T (null_ptr)
 *
 * Decision B: ``ra_iic_b_slave_receive`` line 132, mirror topology
 * ``(len > 0U) && (buf == nullptr)``. Mirror vectors. DO-178C 6.4.4.3.
 */
static void test_mcdc_ra_iic_b_slave(void)
{
  TEST_BEGIN("iic_b_slave MC/DC: send/receive 2-cond null+len");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_open((uint8_t)k_ra_iic_b_slave_test_ch0, &k_cfg));
  volatile r_iic_b_regs_t* reg = ra_iic_b((uint8_t)k_ra_iic_b_slave_test_ch0);
  prearm(reg);
  const uint8_t data[1] = {(uint8_t)k_ra_iic_b_slave_test_byte_a};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_send((uint8_t)k_ra_iic_b_slave_test_ch0, NULL, 0U));
  prearm(reg);
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_send((uint8_t)k_ra_iic_b_slave_test_ch0, data, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_send((uint8_t)k_ra_iic_b_slave_test_ch0, NULL, 1U));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_receive((uint8_t)k_ra_iic_b_slave_test_ch0, NULL, 0U));
  prearm(reg);
  reg->NTDTBP0 = (uint32_t)k_ra_iic_b_slave_test_byte_b;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_b_slave_receive((uint8_t)k_ra_iic_b_slave_test_ch0, buf, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_b_slave_receive((uint8_t)k_ra_iic_b_slave_test_ch0, NULL, 1U));
  (void)ra_iic_b_slave_close((uint8_t)k_ra_iic_b_slave_test_ch0);
  TEST_END("iic_b_slave MC/DC: send/receive 2-cond null+len");
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
  test_mcdc_ra_iic_b_slave();
  return 0;
}
