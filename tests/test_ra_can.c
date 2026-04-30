/**
 * @file test_ra_can.c
 * @brief Unit tests for the classical CAN driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_can_regs.h"
#include "ra_can.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_can_test_const_t
 * @brief Numeric constants used by the test cases.
 */
typedef enum : uint32_t {
  k_ra_can_test_ch0     = 0U,
  k_ra_can_test_ch_oor  = 1U,
  k_ra_can_test_mb0     = 0U,
  k_ra_can_test_mb1     = 1U,
  k_ra_can_test_mb_oor  = 32U,
  k_ra_can_test_std_id  = 0x123U,
  k_ra_can_test_ext_id  = 0x12345678U,
  k_ra_can_test_bad_std = 0x800U,
  k_ra_can_test_bad_ext = 0x40000000U,
  k_ra_can_test_dlc_ok  = 4U,
  k_ra_can_test_dlc_bad = 9U,
} ra_can_test_const_t;

static void prearm_str_clear(volatile r_can_t* reg)
{
  reg->STR = (uint16_t)k_ra_can_str_reset_state;
}

static void test_open_happy(void)
{
  TEST_BEGIN("ra_can_open happy");
  ra_sim_mmap_reset();
  volatile r_can_t* reg = ra_can((uint8_t)k_ra_can_test_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  prearm_str_clear(reg);
  const ra_err_t err = ra_can_open((uint8_t)k_ra_can_test_ch0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("ra_can_open happy");
}

static void test_open_oor(void)
{
  TEST_BEGIN("ra_can_open oor");
  ra_sim_mmap_reset();
  const ra_err_t err = ra_can_open((uint8_t)k_ra_can_test_ch_oor);
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("ra_can_open oor");
}

static void test_send_null(void)
{
  TEST_BEGIN("ra_can_send null frame rejected");
  ra_sim_mmap_reset();
  const ra_err_t err = ra_can_send((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb0, NULL);
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)err);
  TEST_END("ra_can_send null frame rejected");
}

static void test_send_std_ok(void)
{
  TEST_BEGIN("ra_can_send std-id ok");
  ra_sim_mmap_reset();
  volatile r_can_t* reg = ra_can((uint8_t)k_ra_can_test_ch0);
  prearm_str_clear(reg);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_can_open((uint8_t)k_ra_can_test_ch0));

  ra_can_frame_t frame = {};
  frame.id             = (uint32_t)k_ra_can_test_std_id;
  frame.dlc            = (uint8_t)k_ra_can_test_dlc_ok;
  frame.data[0]        = 0xAAU;
  frame.data[1]        = 0xBBU;

  const ra_err_t err = ra_can_send((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb0, &frame);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_can_test_std_id,
                 (int)(reg->MB[k_ra_can_test_mb0].ID & (uint32_t)k_ra_can_id_std_mask));
  TEST_ASSERT_EQ((int)k_ra_can_test_dlc_ok, (int)reg->MB[k_ra_can_test_mb0].DLC);
  TEST_ASSERT(reg->MCTL[k_ra_can_test_mb0] & (uint8_t)k_ra_can_mctl_trmreq);
  TEST_END("ra_can_send std-id ok");
}

static void test_send_bad_dlc(void)
{
  TEST_BEGIN("ra_can_send bad dlc rejected");
  ra_sim_mmap_reset();
  ra_can_frame_t frame = {};
  frame.id             = (uint32_t)k_ra_can_test_std_id;
  frame.dlc            = (uint8_t)k_ra_can_test_dlc_bad;
  const ra_err_t err = ra_can_send((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb0, &frame);
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)err);
  TEST_END("ra_can_send bad dlc rejected");
}

static void test_send_bad_id(void)
{
  TEST_BEGIN("ra_can_send oversized 11-bit id rejected");
  ra_sim_mmap_reset();
  ra_can_frame_t frame = {};
  frame.id             = (uint32_t)k_ra_can_test_bad_std;
  frame.dlc            = 1U;
  const ra_err_t err = ra_can_send((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb0, &frame);
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)err);
  TEST_END("ra_can_send oversized 11-bit id rejected");
}

static void test_receive_no_data(void)
{
  TEST_BEGIN("ra_can_receive no-data");
  ra_sim_mmap_reset();
  ra_can_frame_t frame = {};
  const ra_err_t err =
    ra_can_receive((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb1, &frame);
  TEST_ASSERT_EQ((int)k_ra_err_no_data, (int)err);
  TEST_END("ra_can_receive no-data");
}

static void test_receive_ext_ok(void)
{
  TEST_BEGIN("ra_can_receive ext-id frame ok");
  ra_sim_mmap_reset();
  volatile r_can_t* reg = ra_can((uint8_t)k_ra_can_test_ch0);
  reg->MB[k_ra_can_test_mb1].ID =
    ((uint32_t)k_ra_can_test_ext_id & (uint32_t)k_ra_can_id_ext_mask) |
    (uint32_t)k_ra_can_id_ide_msk;
  reg->MB[k_ra_can_test_mb1].DLC     = 2U;
  reg->MB[k_ra_can_test_mb1].DATA[0] = 0x11U;
  reg->MB[k_ra_can_test_mb1].DATA[1] = 0x22U;
  reg->MCTL[k_ra_can_test_mb1]       = (uint8_t)k_ra_can_mctl_newdata;

  ra_can_frame_t frame = {};
  const ra_err_t err =
    ra_can_receive((uint8_t)k_ra_can_test_ch0, (uint8_t)k_ra_can_test_mb1, &frame);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ(1, (int)frame.is_extended);
  TEST_ASSERT_EQ((int)k_ra_can_test_ext_id, (int)frame.id);
  TEST_ASSERT_EQ(2, (int)frame.dlc);
  TEST_ASSERT_EQ(0x11, (int)frame.data[0]);
  TEST_ASSERT_EQ(0, reg->MCTL[k_ra_can_test_mb1] & (uint8_t)k_ra_can_mctl_newdata);
  TEST_END("ra_can_receive ext-id frame ok");
}

static void test_get_status(void)
{
  TEST_BEGIN("ra_can_get_status");
  ra_sim_mmap_reset();
  volatile r_can_t* reg = ra_can((uint8_t)k_ra_can_test_ch0);
  reg->STR  = (uint16_t)((uint16_t)k_ra_can_str_bus_off | (uint16_t)k_ra_can_str_error_passive);
  reg->TECR = 0x55U;
  reg->RECR = 0xAAU;

  ra_can_status_t st = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_can_get_status((uint8_t)k_ra_can_test_ch0, &st));
  TEST_ASSERT_EQ(0x55, (int)st.tx_err_cnt);
  TEST_ASSERT_EQ(0xAA, (int)st.rx_err_cnt);
  TEST_ASSERT_EQ(1, (int)st.bus_off);
  TEST_ASSERT_EQ(1, (int)st.err_passive);

  /* NULL pointer rejected. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_can_get_status((uint8_t)k_ra_can_test_ch0, NULL));
  TEST_END("ra_can_get_status");
}

static void test_mode_transition_close(void)
{
  TEST_BEGIN("ra_can_mode_transition + ra_can_close");
  ra_sim_mmap_reset();
  volatile r_can_t* reg = ra_can((uint8_t)k_ra_can_test_ch0);
  reg->STR              = (uint16_t)k_ra_can_str_halt_state;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_can_mode_transition((uint8_t)k_ra_can_test_ch0, k_ra_can_mode_halt));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_can_close((uint8_t)k_ra_can_test_ch0));
  TEST_END("ra_can_mode_transition + ra_can_close");
}

int main(void)
{
  test_open_happy();
  test_open_oor();
  test_send_null();
  test_send_std_ok();
  test_send_bad_dlc();
  test_send_bad_id();
  test_receive_no_data();
  test_receive_ext_ok();
  test_get_status();
  test_mode_transition_close();
  return 0;
}
