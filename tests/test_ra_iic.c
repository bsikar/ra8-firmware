/**
 * @file test_ra_iic.c
 * @brief Unit tests for the legacy IIC (RIIC) master + slave driver.
 *
 * @details
 * Drives ``ra_iic_master_*`` and ``ra_iic_slave_*`` against the
 * host-side ``ra_sim_mmap`` substrate. Status flags
 * (``ICSR2.TDRE`` / ``TEND`` / ``RDRF``) are pre-armed so the polling
 * wait loops fall through immediately.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_iic_regs.h"
#include "ra_err.h"
#include "ra_iic.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_iic_test_const_t
 * @brief Numeric constants used by the tests.
 */
typedef enum : uint32_t {
  k_ra_iic_test_ch0      = 0U,
  k_ra_iic_test_ch_oor   = 7U,
  k_ra_iic_test_target   = 0x50U,
  k_ra_iic_test_bus_hz   = 100000U,
  k_ra_iic_test_pclkb_hz = 60000000U,
  k_ra_iic_test_byte_a   = 0xA5U,
  k_ra_iic_test_byte_b   = 0x5AU,
  k_ra_iic_test_slave_7b = 0x42U,
} ra_iic_test_const_t;

static const ra_iic_master_cfg_t k_master_cfg = {
  .bus_hz   = (uint32_t)k_ra_iic_test_bus_hz,
  .pclkb_hz = (uint32_t)k_ra_iic_test_pclkb_hz,
};

static void prearm_master(volatile r_iic_legacy_t* reg)
{
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra_iic_legacy_icsr2_tdre | (uint8_t)k_ra_iic_legacy_icsr2_tend |
                         (uint8_t)k_ra_iic_legacy_icsr2_rdrf);
}

static void test_master_open_null_cfg(void)
{
  TEST_BEGIN("ra_iic_master_open null cfg rejected");
  const ra_err_t err = ra_iic_master_open((uint8_t)k_ra_iic_test_ch0, NULL);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)err);
  TEST_END("ra_iic_master_open null cfg rejected");
}

static void test_master_open_oor(void)
{
  TEST_BEGIN("ra_iic_master_open oor channel rejected");
  const ra_err_t err = ra_iic_master_open((uint8_t)k_ra_iic_test_ch_oor, &k_master_cfg);
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)err);
  TEST_END("ra_iic_master_open oor channel rejected");
}

static void test_master_open_close_happy(void)
{
  TEST_BEGIN("ra_iic_master_open + close happy");
  ra_sim_mmap_reset();
  const ra_err_t err = ra_iic_master_open((uint8_t)k_ra_iic_test_ch0, &k_master_cfg);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_master_close((uint8_t)k_ra_iic_test_ch0));
  TEST_END("ra_iic_master_open + close happy");
}

static void test_master_send_ok(void)
{
  TEST_BEGIN("ra_iic_master_send ok");
  ra_sim_mmap_reset();
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs((uint8_t)k_ra_iic_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_master_open((uint8_t)k_ra_iic_test_ch0, &k_master_cfg));
  prearm_master(reg);
  const uint8_t  payload[2] = {(uint8_t)k_ra_iic_test_byte_a, (uint8_t)k_ra_iic_test_byte_b};
  const ra_err_t err        = ra_iic_master_send((uint8_t)k_ra_iic_test_ch0,
                                                 (uint8_t)k_ra_iic_test_target,
                                                 payload,
                                                 (uint32_t)sizeof(payload));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  /* Last byte written into ICDRT is the trailing data byte. */
  TEST_ASSERT_EQ((int)k_ra_iic_test_byte_b, (int)reg->ICDRT);
  TEST_END("ra_iic_master_send ok");
}

static void test_master_send_null_data(void)
{
  TEST_BEGIN("ra_iic_master_send null data with non-zero len rejected");
  ra_sim_mmap_reset();
  const ra_err_t err =
    ra_iic_master_send((uint8_t)k_ra_iic_test_ch0, (uint8_t)k_ra_iic_test_target, NULL, 1U);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)err);
  TEST_END("ra_iic_master_send null data with non-zero len rejected");
}

static void test_master_receive_ok(void)
{
  TEST_BEGIN("ra_iic_master_receive ok");
  ra_sim_mmap_reset();
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs((uint8_t)k_ra_iic_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_master_open((uint8_t)k_ra_iic_test_ch0, &k_master_cfg));
  prearm_master(reg);
  reg->ICDRR = (uint8_t)k_ra_iic_test_byte_a;

  uint8_t        buf[1] = {0U};
  const ra_err_t err =
    ra_iic_master_receive((uint8_t)k_ra_iic_test_ch0, (uint8_t)k_ra_iic_test_target, buf, 1U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_iic_test_byte_a, (int)buf[0]);
  TEST_END("ra_iic_master_receive ok");
}

static void test_master_status(void)
{
  TEST_BEGIN("ra_iic_master_status reflects TEND/RDRF");
  ra_sim_mmap_reset();
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs((uint8_t)k_ra_iic_test_ch0);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra_iic_legacy_icsr2_tend | (uint8_t)k_ra_iic_legacy_icsr2_rdrf);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_master_status((uint8_t)k_ra_iic_test_ch0, &mask));
  TEST_ASSERT(mask & (uint8_t)k_ra_iic_status_tx_done);
  TEST_ASSERT(mask & (uint8_t)k_ra_iic_status_rx_full);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_master_status((uint8_t)k_ra_iic_test_ch0, NULL));
  TEST_END("ra_iic_master_status reflects TEND/RDRF");
}

static void test_slave_open_close(void)
{
  TEST_BEGIN("ra_iic_slave_open / close");
  ra_sim_mmap_reset();
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs((uint8_t)k_ra_iic_test_ch0);
  const ra_iic_slave_cfg_t cfg = {.slave_addr_7b = (uint8_t)k_ra_iic_test_slave_7b,
                                  .general_call  = 1U};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_iic_slave_open(0U, NULL));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_slave_open((uint8_t)k_ra_iic_test_ch0, &cfg));
  TEST_ASSERT_EQ((int)(uint8_t)((uint8_t)k_ra_iic_test_slave_7b << 1U), (int)reg->SARL[0]);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_slave_close((uint8_t)k_ra_iic_test_ch0));
  TEST_END("ra_iic_slave_open / close");
}

static void test_slave_send_recv(void)
{
  TEST_BEGIN("ra_iic_slave_send / receive happy");
  ra_sim_mmap_reset();
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs((uint8_t)k_ra_iic_test_ch0);
  prearm_master(reg);
  const uint8_t data[1] = {(uint8_t)k_ra_iic_test_byte_b};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_slave_send((uint8_t)k_ra_iic_test_ch0, data, 1U));
  TEST_ASSERT_EQ((int)k_ra_iic_test_byte_b, (int)reg->ICDRT);

  reg->ICDRR     = (uint8_t)k_ra_iic_test_byte_a;
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_slave_receive((uint8_t)k_ra_iic_test_ch0, buf, 1U));
  TEST_ASSERT_EQ((int)k_ra_iic_test_byte_a, (int)buf[0]);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_slave_send((uint8_t)k_ra_iic_test_ch0, NULL, 1U));
  TEST_END("ra_iic_slave_send / receive happy");
}

int main(void)
{
  test_master_open_null_cfg();
  test_master_open_oor();
  test_master_open_close_happy();
  test_master_send_ok();
  test_master_send_null_data();
  test_master_receive_ok();
  test_master_status();
  test_slave_open_close();
  test_slave_send_recv();
  return 0;
}
