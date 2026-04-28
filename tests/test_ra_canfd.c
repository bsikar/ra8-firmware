/**
 * @file test_ra_canfd.c
 * @brief Unit tests for ra_canfd.c (CANFD Lite driver framework)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_canfd_regs.h"
#include "ra_canfd.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_canfd_test_channel_0   = 0U,
  k_ra_canfd_test_channel_1   = 1U,
  k_ra_canfd_test_channel_bad = 2U,
} ra_canfd_test_channel_t;

typedef enum : uint32_t {
  k_ra_test_bitrate_500k     = 500000U,
  k_ra_test_bitrate_1m       = 1000000U,
  k_ra_test_bitrate_250k     = 250000U,
  k_ra_test_bitrate_zero     = 0U,
  k_ra_test_bitrate_invalid  = 999999U,
  k_ra_test_bitrate_bad_data = 1234567U,
  k_ra_test_ext_id           = 0x1FABCDEFU,
  k_ra_test_std_id           = 0x123U,
  k_ra_test_oversized_std_id = 0x800U,
  k_ra_test_invalid_ext_id   = 0x40000000U,
  k_ra_test_erfl_encoded     = 0xAA550000U,
  k_ra_test_expected_tec     = 0x55U,
  k_ra_test_expected_rec     = 0xAAU,
} ra_canfd_test_vals_t;

static void test_init_channel0_happy(void)
{
  TEST_BEGIN("canfd init channel 0 happy");
  ra_sim_mmap_reset();

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDCNSTS = 0xFFFFFFFFUL;

  const ra_err_t err = ra_canfd_init((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_chmdc_operation, (int)reg->CFDCNCTR);
  TEST_END("canfd init channel 0 happy");
}

static void test_init_channel0_timeout(void)
{
  TEST_BEGIN("canfd init channel 0 timeout path");
  ra_sim_mmap_reset();

  const ra_err_t err = ra_canfd_init((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("canfd init channel 0 timeout path");
}

static void test_init_channel1(void)
{
  TEST_BEGIN("canfd init channel 1");
  ra_sim_mmap_reset();

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_1);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDCNSTS      = 0xFFFFFFFFUL;
  const ra_err_t err = ra_canfd_init((uint8_t)k_ra_canfd_test_channel_1);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("canfd init channel 1");
}

static void test_init_channel_bad(void)
{
  TEST_BEGIN("canfd init bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_canfd_init((uint8_t)k_ra_canfd_test_channel_bad));
  TEST_END("canfd init bad channel");
}

static void test_deinit_happy(void)
{
  TEST_BEGIN("canfd deinit happy");
  ra_sim_mmap_reset();

  const ra_err_t err = ra_canfd_deinit((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_EQ((int)k_ra_chmdc_reset, (int)reg->CFDCNCTR);
  TEST_END("canfd deinit happy");
}

static void test_deinit_bad_channel(void)
{
  TEST_BEGIN("canfd deinit bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_deinit((uint8_t)k_ra_canfd_test_channel_bad));
  TEST_END("canfd deinit bad channel");
}

static void test_set_bitrate_500k_happy(void)
{
  TEST_BEGIN("canfd set_bitrate 500k happy");
  ra_sim_mmap_reset();

  const ra_err_t err =
    ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0, (uint32_t)k_ra_test_bitrate_500k, 0U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT(reg->CFDCNCFG != 0U);
  TEST_END("canfd set_bitrate 500k happy");
}

static void test_set_bitrate_250k_with_fd(void)
{
  TEST_BEGIN("canfd set_bitrate 250k nominal + 1M data");
  ra_sim_mmap_reset();

  const ra_err_t err = ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0,
                                            (uint32_t)k_ra_test_bitrate_250k,
                                            (uint32_t)k_ra_test_bitrate_1m);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT(reg->CFDCNCFG != 0U);
  TEST_ASSERT(reg->CFDCNDCFG != 0U);
  TEST_END("canfd set_bitrate 250k nominal + 1M data");
}

static void test_set_bitrate_zero_rejected(void)
{
  TEST_BEGIN("canfd set_bitrate rejects zero");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0,
                                           (uint32_t)k_ra_test_bitrate_zero,
                                           0U));
  TEST_END("canfd set_bitrate rejects zero");
}

static void test_set_bitrate_invalid_resolve(void)
{
  TEST_BEGIN("canfd set_bitrate rejects unresolvable rate");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0,
                                           (uint32_t)k_ra_test_bitrate_invalid,
                                           0U));
  TEST_END("canfd set_bitrate rejects unresolvable rate");
}

static void test_set_bitrate_prescaler_too_big(void)
{
  TEST_BEGIN("canfd set_bitrate rejects rate needing prescaler > 256");
  ra_sim_mmap_reset();

  /* 1 bps with 8 MHz PCLKA requires a prescaler of ~1 million, well
   * outside the 1..256 window, so the solver exhausts every tq and
   * returns invalid_arg. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0, 1U, 0U));
  TEST_END("canfd set_bitrate rejects rate needing prescaler > 256");
}

static void test_set_bitrate_bad_data_rate(void)
{
  TEST_BEGIN("canfd set_bitrate rejects bad data rate");
  ra_sim_mmap_reset();

  /* Nominal 250k resolves fine, but data-phase 1234567 will not
   * divide evenly into 8 MHz. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_0,
                                           (uint32_t)k_ra_test_bitrate_250k,
                                           (uint32_t)k_ra_test_bitrate_bad_data));
  TEST_END("canfd set_bitrate rejects bad data rate");
}

static void test_set_bitrate_bad_channel(void)
{
  TEST_BEGIN("canfd set_bitrate rejects bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_set_bitrate((uint8_t)k_ra_canfd_test_channel_bad,
                                           (uint32_t)k_ra_test_bitrate_500k,
                                           0U));
  TEST_END("canfd set_bitrate rejects bad channel");
}

static void test_transmit_standard_frame_happy(void)
{
  TEST_BEGIN("canfd transmit standard frame happy");
  ra_sim_mmap_reset();

  ra_canfd_frame_t frame = {};
  frame.id               = (uint32_t)k_ra_test_std_id;
  frame.dlc              = 8U;
  frame.is_extended      = 0U;
  frame.is_fd            = 0U;
  frame.is_brs           = 0U;
  for (uint8_t i = 0U; i < 8U; i++) {
    frame.data[i] = (uint8_t)(i + 1U);
  }

  const ra_err_t err = ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, &frame);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  TEST_ASSERT_EQ((int)k_ra_test_std_id, (int)reg->CFDTMID);
  TEST_ASSERT_EQ((int)1, (int)reg->CFDTMC);
  TEST_END("canfd transmit standard frame happy");
}

static void test_transmit_extended_fd_frame(void)
{
  TEST_BEGIN("canfd transmit extended CAN-FD frame");
  ra_sim_mmap_reset();

  ra_canfd_frame_t frame = {};
  frame.id               = (uint32_t)k_ra_test_ext_id;
  frame.dlc              = 15U;
  frame.is_extended      = 1U;
  frame.is_fd            = 1U;
  frame.is_brs           = 1U;

  const ra_err_t err = ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_1, &frame);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_1);
  TEST_ASSERT((reg->CFDTMID & k_ra_canfd_id_ide) != 0U);
  TEST_ASSERT((reg->CFDTMFDSTS & k_ra_canfd_fd_fdf) != 0U);
  TEST_ASSERT((reg->CFDTMFDSTS & k_ra_canfd_fd_brs) != 0U);
  TEST_END("canfd transmit extended CAN-FD frame");
}

static void test_transmit_null_frame(void)
{
  TEST_BEGIN("canfd transmit rejects NULL frame");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, nullptr));
  TEST_END("canfd transmit rejects NULL frame");
}

static void test_transmit_bad_channel(void)
{
  TEST_BEGIN("canfd transmit rejects bad channel");
  ra_sim_mmap_reset();
  ra_canfd_frame_t frame = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_bad, &frame));
  TEST_END("canfd transmit rejects bad channel");
}

static void test_transmit_bad_dlc(void)
{
  TEST_BEGIN("canfd transmit rejects DLC > 15");
  ra_sim_mmap_reset();
  ra_canfd_frame_t frame = {};
  frame.dlc              = 16U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects DLC > 15");
}

static void test_transmit_oversized_std_id(void)
{
  TEST_BEGIN("canfd transmit rejects 11-bit overflow");
  ra_sim_mmap_reset();
  ra_canfd_frame_t frame = {};
  frame.id               = (uint32_t)k_ra_test_oversized_std_id;
  frame.is_extended      = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects 11-bit overflow");
}

static void test_transmit_oversized_ext_id(void)
{
  TEST_BEGIN("canfd transmit rejects 29-bit overflow");
  ra_sim_mmap_reset();
  ra_canfd_frame_t frame = {};
  frame.id               = (uint32_t)k_ra_test_invalid_ext_id;
  frame.is_extended      = 1U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects 29-bit overflow");
}

static void test_transmit_brs_without_fd(void)
{
  TEST_BEGIN("canfd transmit rejects BRS without FD");
  ra_sim_mmap_reset();
  ra_canfd_frame_t frame = {};
  frame.is_fd            = 0U;
  frame.is_brs           = 1U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_canfd_transmit((uint8_t)k_ra_canfd_test_channel_0, &frame));
  TEST_END("canfd transmit rejects BRS without FD");
}

static void test_receive_empty_fifo(void)
{
  TEST_BEGIN("canfd receive returns no_data on empty FIFO");
  ra_sim_mmap_reset();

  /* Seed RFEMP so the driver reads empty. */
  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  reg->CFDRFSTS                        = (uint32_t)k_ra_rfsts_bit_empty;

  ra_canfd_frame_t out = {};
  TEST_ASSERT_EQ((int)k_ra_err_no_data,
                 (int)ra_canfd_receive((uint8_t)k_ra_canfd_test_channel_0, &out));
  TEST_END("canfd receive returns no_data on empty FIFO");
}

static void test_receive_standard_frame(void)
{
  TEST_BEGIN("canfd receive decodes standard frame");
  ra_sim_mmap_reset();

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  reg->CFDRFSTS                        = 0U; /* not empty */
  reg->CFDRFID                         = (uint32_t)k_ra_test_std_id;
  reg->CFDRFPTR                        = (uint32_t)(8UL << 28U); /* DLC = 8 */
  reg->CFDRFFDSTS                      = 0U;
  reg->CFDRFDF[0]                      = 0x44332211U;

  ra_canfd_frame_t out = {};
  const ra_err_t   err = ra_canfd_receive((uint8_t)k_ra_canfd_test_channel_0, &out);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_test_std_id, (int)out.id);
  TEST_ASSERT_EQ(0, (int)out.is_extended);
  TEST_ASSERT_EQ(8, (int)out.dlc);
  TEST_ASSERT_EQ(0x11, (int)out.data[0]);
  TEST_ASSERT_EQ(0x22, (int)out.data[1]);
  TEST_ASSERT_EQ(0x33, (int)out.data[2]);
  TEST_ASSERT_EQ(0x44, (int)out.data[3]);
  TEST_END("canfd receive decodes standard frame");
}

static void test_receive_extended_fd_frame(void)
{
  TEST_BEGIN("canfd receive decodes extended FD frame");
  ra_sim_mmap_reset();

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_1);
  reg->CFDRFSTS                        = 0U;
  reg->CFDRFID                         = (uint32_t)k_ra_test_ext_id | (uint32_t)k_ra_canfd_id_ide;
  reg->CFDRFPTR                        = (uint32_t)(15UL << 28U);
  reg->CFDRFFDSTS                      = (uint32_t)k_ra_canfd_fd_fdf | (uint32_t)k_ra_canfd_fd_brs;

  ra_canfd_frame_t out = {};
  const ra_err_t   err = ra_canfd_receive((uint8_t)k_ra_canfd_test_channel_1, &out);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ(1, (int)out.is_extended);
  TEST_ASSERT_EQ((int)k_ra_test_ext_id, (int)out.id);
  TEST_ASSERT_EQ(1, (int)out.is_fd);
  TEST_ASSERT_EQ(1, (int)out.is_brs);
  TEST_ASSERT_EQ(15, (int)out.dlc);
  TEST_END("canfd receive decodes extended FD frame");
}

static void test_receive_null_out(void)
{
  TEST_BEGIN("canfd receive rejects NULL out");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_receive((uint8_t)k_ra_canfd_test_channel_0, nullptr));
  TEST_END("canfd receive rejects NULL out");
}

static void test_receive_bad_channel(void)
{
  TEST_BEGIN("canfd receive rejects bad channel");
  ra_sim_mmap_reset();
  ra_canfd_frame_t out = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_canfd_receive((uint8_t)k_ra_canfd_test_channel_bad, &out));
  TEST_END("canfd receive rejects bad channel");
}

static void test_get_error_state_happy(void)
{
  TEST_BEGIN("canfd get_error_state happy");
  ra_sim_mmap_reset();

  volatile r_canfd_channel_regs_t* reg = ra_canfd((uint8_t)k_ra_canfd_test_channel_0);
  reg->CFDCNERFL                       = (uint32_t)k_ra_test_erfl_encoded;

  uint8_t        tx_err = 0U;
  uint8_t        rx_err = 0U;
  const ra_err_t err =
    ra_canfd_get_error_state((uint8_t)k_ra_canfd_test_channel_0, &tx_err, &rx_err);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_test_expected_tec, (int)tx_err);
  TEST_ASSERT_EQ((int)k_ra_test_expected_rec, (int)rx_err);
  TEST_END("canfd get_error_state happy");
}

static void test_get_error_state_null_tx(void)
{
  TEST_BEGIN("canfd get_error_state rejects NULL tx_err");
  ra_sim_mmap_reset();

  uint8_t rx_err = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_canfd_get_error_state((uint8_t)k_ra_canfd_test_channel_0, nullptr, &rx_err));
  TEST_END("canfd get_error_state rejects NULL tx_err");
}

static void test_get_error_state_null_rx(void)
{
  TEST_BEGIN("canfd get_error_state rejects NULL rx_err");
  ra_sim_mmap_reset();

  uint8_t tx_err = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_canfd_get_error_state((uint8_t)k_ra_canfd_test_channel_0, &tx_err, nullptr));
  TEST_END("canfd get_error_state rejects NULL rx_err");
}

static void test_get_error_state_bad_channel(void)
{
  TEST_BEGIN("canfd get_error_state rejects bad channel");
  ra_sim_mmap_reset();

  uint8_t tx_err = 0U;
  uint8_t rx_err = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_canfd_get_error_state((uint8_t)k_ra_canfd_test_channel_bad, &tx_err, &rx_err));
  TEST_END("canfd get_error_state rejects bad channel");
}

/* ---- status + IRQ + power ---- */

static uint32_t s_canfd_cb_count;
static uint32_t s_canfd_cb_last_mask;
static uint8_t  s_canfd_cb_last_channel;

static void stub_canfd_cb(void* ctx, uint8_t ch, uint32_t mask)
{
  (void)ctx;
  ++s_canfd_cb_count;
  s_canfd_cb_last_mask    = mask;
  s_canfd_cb_last_channel = ch;
}

static void prep_w53(void)
{
  ra_sim_mmap_reset();
  s_canfd_cb_count        = 0U;
  s_canfd_cb_last_mask    = 0U;
  s_canfd_cb_last_channel = 0U;
}

static void test_get_status(void)
{
  TEST_BEGIN("canfd get_status");
  prep_w53();
  ra_canfd((uint8_t)k_ra_canfd_test_channel_0)->CFDCNSTS = 0xDEADBEEFU;
  uint32_t mask                                          = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_canfd_get_status((uint8_t)k_ra_canfd_test_channel_0, &mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_canfd_get_status((uint8_t)k_ra_canfd_test_channel_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_canfd_get_status((uint8_t)k_ra_canfd_test_channel_bad, &mask));
  TEST_END("canfd get_status");
}

static void test_clear_status(void)
{
  TEST_BEGIN("canfd clear_status");
  prep_w53();
  ra_canfd((uint8_t)k_ra_canfd_test_channel_0)->CFDCNERFL = 0xFFFFFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_canfd_clear_status((uint8_t)k_ra_canfd_test_channel_0, 0x0000FF00U));
  TEST_ASSERT_EQ((int32_t)0xFFFF00FFU,
                 (int32_t)ra_canfd((uint8_t)k_ra_canfd_test_channel_0)->CFDCNERFL);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_canfd_clear_status((uint8_t)k_ra_canfd_test_channel_bad, 0U));
  TEST_END("canfd clear_status");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("canfd attach + dispatch");
  prep_w53();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_canfd_attach_handler(stub_canfd_cb, (void*)(uintptr_t)0xABU));
  ra_canfd((uint8_t)k_ra_canfd_test_channel_1)->CFDCNERFL = 0xCAFEU;
  ra_canfd_dispatch((uint8_t)k_ra_canfd_test_channel_1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_canfd_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_canfd_cb_last_mask);
  TEST_ASSERT_EQ((int32_t)k_ra_canfd_test_channel_1, (int32_t)s_canfd_cb_last_channel);

  ra_canfd_dispatch((uint8_t)k_ra_canfd_test_channel_bad);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_canfd_cb_count);
  TEST_END("canfd attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("canfd power transition");
  prep_w53();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_canfd_init((uint8_t)k_ra_canfd_test_channel_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_canfd_enter_stop((uint8_t)k_ra_canfd_test_channel_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_canfd_exit_stop((uint8_t)k_ra_canfd_test_channel_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_canfd_enter_stop((uint8_t)k_ra_canfd_test_channel_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_canfd_exit_stop((uint8_t)k_ra_canfd_test_channel_bad));
  TEST_END("canfd power transition");
}

int32_t main(void)
{
  test_init_channel0_happy();
  test_init_channel0_timeout();
  test_init_channel1();
  test_init_channel_bad();
  test_deinit_happy();
  test_deinit_bad_channel();
  test_set_bitrate_500k_happy();
  test_set_bitrate_250k_with_fd();
  test_set_bitrate_zero_rejected();
  test_set_bitrate_invalid_resolve();
  test_set_bitrate_prescaler_too_big();
  test_set_bitrate_bad_data_rate();
  test_set_bitrate_bad_channel();
  test_transmit_standard_frame_happy();
  test_transmit_extended_fd_frame();
  test_transmit_null_frame();
  test_transmit_bad_channel();
  test_transmit_bad_dlc();
  test_transmit_oversized_std_id();
  test_transmit_oversized_ext_id();
  test_transmit_brs_without_fd();
  test_receive_empty_fifo();
  test_receive_standard_frame();
  test_receive_extended_fd_frame();
  test_receive_null_out();
  test_receive_bad_channel();
  test_get_error_state_happy();
  test_get_error_state_null_tx();
  test_get_error_state_null_rx();
  test_get_error_state_bad_channel();
  test_get_status();
  test_clear_status();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra_canfd.c\n");
  return 0;
}
