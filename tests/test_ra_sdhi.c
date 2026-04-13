/**
 * @file test_ra_sdhi.c
 * @brief Unit tests for ra_sdhi.c (SD/MMC Host Interface scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <sys/time.h>

#include "ra8d2_sdhi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sdhi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint8_t s_sdhi_alarm_inst;

static void sdhi_sigalarm_handler(int sig)
{
  (void)sig;
  /* Fake hardware: set RSPEND so the bounded poll in send_command
   * exits on the next iteration. */
  volatile r_sdhi_regs_t* reg = ra_sdhi(s_sdhi_alarm_inst);
  if (reg != nullptr) {
    reg->SD_INFO1 = reg->SD_INFO1 | 1UL;
  }
}

static void arm_rspend_alarm(uint8_t inst)
{
  s_sdhi_alarm_inst = inst;
  struct sigaction sa;
  sa.sa_handler = sdhi_sigalarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = 100;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = 0;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_sdhi_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

typedef enum : uint8_t {
  k_ra_sdhi_test_inst_0   = 0U,
  k_ra_sdhi_test_inst_1   = 1U,
  k_ra_sdhi_test_inst_bad = 9U,
} ra_sdhi_test_inst_t;

static uint32_t s_sdhi_cb_count;
static uint32_t s_sdhi_cb_last_mask;
static uint8_t  s_sdhi_cb_last_inst;

static void stub_sdhi_cb(void* ctx, uint8_t inst, uint32_t mask)
{
  (void)ctx;
  ++s_sdhi_cb_count;
  s_sdhi_cb_last_mask = mask;
  s_sdhi_cb_last_inst = inst;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_sdhi_cb_count     = 0U;
  s_sdhi_cb_last_mask = 0U;
  s_sdhi_cb_last_inst = 0U;
}

static void test_init_happy(void)
{
  TEST_BEGIN("sdhi init happy");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_1));
  TEST_END("sdhi init happy");
}

static void test_init_bad(void)
{
  TEST_BEGIN("sdhi init bad");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi init bad");
}

static void test_deinit(void)
{
  TEST_BEGIN("sdhi deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_deinit((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_deinit((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("sdhi status read + clear");
  prep();

  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0)->SD_INFO1 = 0xCAFEBABEUL;
  uint32_t mask                                     = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sdhi_get_status((uint8_t)k_ra_sdhi_test_inst_0, &mask));
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEU, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sdhi_clear_status((uint8_t)k_ra_sdhi_test_inst_0, 0x000000F0UL));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_get_status((uint8_t)k_ra_sdhi_test_inst_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_clear_status((uint8_t)k_ra_sdhi_test_inst_bad, 0U));
  TEST_END("sdhi status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("sdhi attach + dispatch");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sdhi_attach_handler(stub_sdhi_cb, (void*)(uintptr_t)0x5DU));
  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_1)->SD_INFO1 = 0xDEADBEEFUL;
  ra_sdhi_dispatch((uint8_t)k_ra_sdhi_test_inst_1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_sdhi_cb_count);
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)s_sdhi_cb_last_mask);
  TEST_ASSERT_EQ((int32_t)k_ra_sdhi_test_inst_1, (int32_t)s_sdhi_cb_last_inst);

  ra_sdhi_dispatch((uint8_t)k_ra_sdhi_test_inst_bad);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_sdhi_cb_count);
  TEST_END("sdhi attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("sdhi power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_init((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_enter_stop((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdhi_exit_stop((uint8_t)k_ra_sdhi_test_inst_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sdhi_enter_stop((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sdhi_exit_stop((uint8_t)k_ra_sdhi_test_inst_bad));
  TEST_END("sdhi power transition");
}

static void test_send_command_rspend_via_alarm(void)
{
  TEST_BEGIN("sdhi send_command: RSPEND via alarm");
  prep();

  /* Pre-seed response regs. */
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0);
  reg->SD_RSP10               = 0x11111111UL;
  reg->SD_RSP32               = 0x22222222UL;
  reg->SD_RSP54               = 0x33333333UL;
  reg->SD_RSP76               = 0x44444444UL;

  arm_rspend_alarm((uint8_t)k_ra_sdhi_test_inst_0);
  uint32_t       rsp[4] = {0U, 0U, 0U, 0U};
  const ra_err_t err =
    ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_0, 0x0000ABCDU, 0xDEADBEEFUL, rsp);
  disarm_sdhi_alarm();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)err);
  TEST_ASSERT_EQ((int32_t)0x11111111, (int32_t)rsp[0]);
  TEST_ASSERT_EQ((int32_t)0x22222222, (int32_t)rsp[1]);
  TEST_ASSERT_EQ((int32_t)0x33333333, (int32_t)rsp[2]);
  TEST_ASSERT_EQ((int32_t)0x44444444, (int32_t)rsp[3]);
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFUL, (int32_t)reg->SD_ARG);
  TEST_ASSERT_EQ((int32_t)0x0000ABCDU, (int32_t)reg->SD_CMD);
  TEST_END("sdhi send_command: RSPEND via alarm");
}

static void test_send_command_no_rsp_buffer(void)
{
  TEST_BEGIN("sdhi send_command: null response buffer");
  prep();

  arm_rspend_alarm((uint8_t)k_ra_sdhi_test_inst_1);
  const ra_err_t err = ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_1, 0x01U, 0U, nullptr);
  disarm_sdhi_alarm();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)err);
  TEST_END("sdhi send_command: null response buffer");
}

static void test_send_command_bad_instance(void)
{
  TEST_BEGIN("sdhi send_command: bad instance");
  prep();
  uint32_t rsp[4] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_send_command((uint8_t)k_ra_sdhi_test_inst_bad, 0U, 0U, rsp));
  TEST_END("sdhi send_command: bad instance");
}

static void test_set_clock(void)
{
  TEST_BEGIN("sdhi set_clock");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sdhi_set_clock((uint8_t)k_ra_sdhi_test_inst_0, 0x0080U));
  TEST_ASSERT_EQ((int32_t)0x0080U, (int32_t)ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0)->SD_CLK_CTRL);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sdhi_set_clock((uint8_t)k_ra_sdhi_test_inst_bad, 0U));
  TEST_END("sdhi set_clock");
}

int32_t main(void)
{
  test_init_happy();
  test_init_bad();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_send_command_rspend_via_alarm();
  test_send_command_no_rsp_buffer();
  test_send_command_bad_instance();
  test_set_clock();
  (void)fprintf(stderr, "[OK  ] test_ra_sdhi.c\n");
  return 0;
}
