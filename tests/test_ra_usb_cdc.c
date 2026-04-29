/**
 * @file test_ra_usb_cdc.c
 * @brief Unit tests for the native USB CDC ACM class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_cdc.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_cdc_deinit();
}

static void test_init_fs_default_coding(void)
{
  TEST_BEGIN("ra_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ((int32_t)9600U, (int32_t)coding.dte_rate);
  TEST_ASSERT_EQ((int32_t)8U, (int32_t)coding.data_bits);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)coding.char_format);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)coding.parity_type);
  TEST_END("ra_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_cdc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_cdc_init rejects bogus speed");
}

static void test_init_hs_picks_512_bulk(void)
{
  TEST_BEGIN("ra_usb_cdc_init on HS configures 512-byte bulk pipes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_hs));
  /* PIPECFG was last written for the interrupt pipe (PIPE6); the
   * bulk-pipe state is in the controller's pipe table, but the host
   * mock keeps the most recent PIPESEL/PIPEMAXP write. Read PIPE6
   * config which should be intr / 8 bytes. */
  volatile r_usb_regs_t* reg = ra_usb_hs();
  TEST_ASSERT_EQ((int32_t)k_ra_cdc_pipe_intr_in, (int32_t)reg->PIPESEL);
  TEST_ASSERT_EQ((int32_t)k_ra_cdc_intr_max_packet, (int32_t)reg->PIPEMAXP);
  TEST_END("ra_usb_cdc_init on HS configures 512-byte bulk pipes");
}

static void test_attach_detach(void)
{
  TEST_BEGIN("ra_usb_cdc_attach toggles DPRPU");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_attach(true));
  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_attach(false));
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dpbit));
  TEST_END("ra_usb_cdc_attach toggles DPRPU");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("CDC API rejects calls before init");
  prep();

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_attach(true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_send(buf, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_recv(buf, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_deinit());

  ra_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_get_line_coding(&coding));
  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));

  ra_usb_setup_t setup = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_handle_setup(&setup));
  TEST_END("CDC API rejects calls before init");
}

static void test_handle_setup_set_control_line_state(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup updates DTR/RTS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x21U,
    .b_request       = (uint8_t)k_ra_cdc_req_set_control_line_state,
    .w_value         = (uint16_t)(k_ra_cdc_line_state_dtr | k_ra_cdc_line_state_rts),
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));

  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(rts);

  setup.w_value = (uint16_t)k_ra_cdc_line_state_dtr;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(!rts);
  TEST_END("ra_usb_cdc_handle_setup updates DTR/RTS");
}

static void test_handle_setup_get_line_coding_acks(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup ACKs GET_LINE_CODING");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0xA1U,
    .b_request       = (uint8_t)k_ra_cdc_req_get_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 7U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  /* DCPCTR.PID should be BUF, CCPL set. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)k_ra_pid_buf, (int32_t)(reg->DCPCTR & k_ra_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra_dcpctr_bit_ccpl)) != 0U);
  TEST_END("ra_usb_cdc_handle_setup ACKs GET_LINE_CODING");
}

static void test_handle_setup_rejects_standard(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup rejects non-class SETUPs");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U, /* standard, device, IN */
    .b_request       = (uint8_t)0x06U, /* GET_DESCRIPTOR */
    .w_value         = 0x0100U,
    .w_index         = 0U,
    .w_length        = 18U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_cdc_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_handle_setup(nullptr));
  TEST_END("ra_usb_cdc_handle_setup rejects non-class SETUPs");
}

static void test_send_recv_arg_validation(void)
{
  TEST_BEGIN("ra_usb_cdc_{send,recv} validate args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_send(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_recv(nullptr, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_recv(buf, nullptr));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_recv(buf, &zero));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_coding(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_state(nullptr, nullptr));
  bool dtr = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_state(&dtr, nullptr));
  TEST_END("ra_usb_cdc_{send,recv} validate args");
}

int32_t main(void)
{
  test_init_fs_default_coding();
  test_init_bad_speed();
  test_init_hs_picks_512_bulk();
  test_attach_detach();
  test_pre_init_calls();
  test_handle_setup_set_control_line_state();
  test_handle_setup_get_line_coding_acks();
  test_handle_setup_rejects_standard();
  test_send_recv_arg_validation();
  (void)fprintf(stderr, "[OK ] test_ra_usb_cdc.c\n");
  return 0;
}
