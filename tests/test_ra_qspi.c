/**
 * @file test_ra_qspi.c
 * @brief Unit tests for the legacy QSPI driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_qspi_regs.h"
#include "ra_err.h"
#include "ra_qspi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_qspi_test_const_t
 * @brief Numeric constants used by the test cases.
 */
typedef enum : uint32_t {
  k_ra_qspi_test_ch0       = 0U,
  k_ra_qspi_test_ch_oor    = 4U,
  k_ra_qspi_test_clock_div = 4U,
  k_ra_qspi_test_dummy     = 8U,
  k_ra_qspi_test_xip_op    = 0xEBU,
  k_ra_qspi_test_byte_a    = 0xA5U,
  k_ra_qspi_test_byte_b    = 0x5AU,
  k_ra_qspi_test_xip_off   = 0x40U,
  k_ra_qspi_test_xip_max   = 0x10000U,
} ra_qspi_test_const_t;

static const ra_qspi_cfg_t k_cfg = {
  .clock_div    = (uint32_t)k_ra_qspi_test_clock_div,
  .cpol         = 0U,
  .cpha         = 0U,
  .dummy_cycles = (uint8_t)k_ra_qspi_test_dummy,
  .protocol     = k_ra_qspi_protocol_quad,
  .xip_read_op  = (uint8_t)k_ra_qspi_test_xip_op,
};

static void test_open_null_cfg(void)
{
  TEST_BEGIN("ra_qspi_open null cfg rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_qspi_open((uint8_t)k_ra_qspi_test_ch0, NULL));
  TEST_END("ra_qspi_open null cfg rejected");
}

static void test_open_oor(void)
{
  TEST_BEGIN("ra_qspi_open oor channel rejected");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_qspi_open((uint8_t)k_ra_qspi_test_ch_oor, &k_cfg));
  TEST_END("ra_qspi_open oor channel rejected");
}

static void test_open_close_happy(void)
{
  TEST_BEGIN("ra_qspi_open + close happy");
  ra_sim_mmap_reset();
  volatile r_qspi_t* reg = ra_qspi_regs((uint8_t)k_ra_qspi_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_open((uint8_t)k_ra_qspi_test_ch0, &k_cfg));
  TEST_ASSERT_EQ((int)k_ra_qspi_test_clock_div, (int)reg->SFMSKC);
  TEST_ASSERT_EQ((int)k_ra_qspi_protocol_quad, (int)reg->SFMSPC);
  TEST_ASSERT_EQ((int)k_ra_qspi_test_xip_op, (int)reg->SFMSIC);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_close((uint8_t)k_ra_qspi_test_ch0));
  TEST_END("ra_qspi_open + close happy");
}

static void test_open_bad_clock(void)
{
  TEST_BEGIN("ra_qspi_open bad clock_div rejected");
  ra_sim_mmap_reset();
  ra_qspi_cfg_t bad = k_cfg;
  bad.clock_div     = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_qspi_open((uint8_t)k_ra_qspi_test_ch0, &bad));
  TEST_END("ra_qspi_open bad clock_div rejected");
}

static void test_send_ok(void)
{
  TEST_BEGIN("ra_qspi_send ok");
  ra_sim_mmap_reset();
  volatile r_qspi_t* reg   = ra_qspi_regs((uint8_t)k_ra_qspi_test_ch0);
  reg->SFMSST              = 0U; /* TX not full, BUSY clear */
  const uint8_t payload[1] = {(uint8_t)k_ra_qspi_test_byte_a};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_send((uint8_t)k_ra_qspi_test_ch0, payload, 1U));
  TEST_ASSERT_EQ((int)k_ra_qspi_test_byte_a, (int)reg->SFMCOM);
  TEST_END("ra_qspi_send ok");
}

static void test_send_null(void)
{
  TEST_BEGIN("ra_qspi_send null data rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_qspi_send((uint8_t)k_ra_qspi_test_ch0, NULL, 1U));
  TEST_END("ra_qspi_send null data rejected");
}

static void test_receive_xip(void)
{
  TEST_BEGIN("ra_qspi_receive copies from XIP window");
  ra_sim_mmap_reset();
  volatile uint8_t* xip            = ra_qspi_xip();
  xip[k_ra_qspi_test_xip_off + 0U] = (uint8_t)k_ra_qspi_test_byte_a;
  xip[k_ra_qspi_test_xip_off + 1U] = (uint8_t)k_ra_qspi_test_byte_b;
  uint8_t buf[2]                   = {0U, 0U};
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_qspi_receive((uint8_t)k_ra_qspi_test_ch0, (uint32_t)k_ra_qspi_test_xip_off, buf, 2U));
  TEST_ASSERT_EQ((int)k_ra_qspi_test_byte_a, (int)buf[0]);
  TEST_ASSERT_EQ((int)k_ra_qspi_test_byte_b, (int)buf[1]);

  /* Out-of-range offset rejected. */
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_qspi_receive((uint8_t)k_ra_qspi_test_ch0, (uint32_t)k_ra_qspi_test_xip_max, buf, 1U));
  TEST_END("ra_qspi_receive copies from XIP window");
}

static void test_status(void)
{
  TEST_BEGIN("ra_qspi_status reflects SFMSST");
  ra_sim_mmap_reset();
  volatile r_qspi_t* reg = ra_qspi_regs((uint8_t)k_ra_qspi_test_ch0);
  reg->SFMSST  = (uint32_t)((uint32_t)k_ra_qspi_sfmsst_busy | (uint32_t)k_ra_qspi_sfmsst_rxrdy);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_status((uint8_t)k_ra_qspi_test_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra_qspi_status_busy);
  TEST_ASSERT(mask & (uint8_t)k_ra_qspi_status_rx_rdy);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_qspi_status((uint8_t)k_ra_qspi_test_ch0, NULL));
  TEST_END("ra_qspi_status reflects SFMSST");
}

static void test_xip_enter_exit(void)
{
  TEST_BEGIN("ra_qspi_xip_enter / exit");
  ra_sim_mmap_reset();
  volatile r_qspi_t* reg = ra_qspi_regs((uint8_t)k_ra_qspi_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_xip_enter((uint8_t)k_ra_qspi_test_ch0));
  TEST_ASSERT_EQ((int)k_ra_qspi_sfmcmd_xip_enter, (int)reg->SFMCMD);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_qspi_xip_exit((uint8_t)k_ra_qspi_test_ch0));
  TEST_ASSERT_EQ((int)k_ra_qspi_sfmcmd_xip_exit, (int)reg->SFMCMD);
  TEST_END("ra_qspi_xip_enter / exit");
}

int main(void)
{
  test_open_null_cfg();
  test_open_oor();
  test_open_close_happy();
  test_open_bad_clock();
  test_send_ok();
  test_send_null();
  test_receive_xip();
  test_status();
  test_xip_enter_exit();
  return 0;
}
