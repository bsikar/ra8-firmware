/**
 * @file test_ra_ulpt.c
 * @brief Unit tests for ra_ulpt.c (Ultra-Low-Power Timer driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ulpt_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_ulpt.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_ulpt_test_ch_0   = 0U,
  k_ra_ulpt_test_ch_1   = 1U,
  k_ra_ulpt_test_ch_bad = 2U,
  k_ra_ulpt_test_ch_way = 200U,
} ra_ulpt_test_ch_t;

typedef enum : uint32_t {
  k_ra_ulpt_test_period = 0x00ABCDEFUL,
} ra_ulpt_test_period_t;

static void test_init_clears_regs(void)
{
  TEST_BEGIN("ulpt init clears regs");
  ra_sim_mmap_reset();

  volatile r_ulpt_regs_t* reg = ra_ulpt((uint8_t)k_ra_ulpt_test_ch_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->ULPTCR = 0xFFU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ulpt_init());
  TEST_ASSERT_EQ(0, (int)reg->ULPTCR);
  TEST_END("ulpt init clears regs");
}

static void test_start_channel_0(void)
{
  TEST_BEGIN("ulpt start channel 0");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ulpt_start((uint8_t)k_ra_ulpt_test_ch_0, (uint32_t)k_ra_ulpt_test_period));

  volatile r_ulpt_regs_t* reg = ra_ulpt((uint8_t)k_ra_ulpt_test_ch_0);
  TEST_ASSERT_EQ((int)k_ra_ulpt_test_period, (int)reg->ULPTCNT);
  TEST_ASSERT_EQ((int)k_ra_ulpt_mask_tstart, (int)reg->ULPTCR);
  TEST_END("ulpt start channel 0");
}

static void test_start_channel_1(void)
{
  TEST_BEGIN("ulpt start channel 1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ulpt_start((uint8_t)k_ra_ulpt_test_ch_1, (uint32_t)k_ra_ulpt_test_period));
  TEST_END("ulpt start channel 1");
}

static void test_start_bad_channel(void)
{
  TEST_BEGIN("ulpt start bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_ulpt_start((uint8_t)k_ra_ulpt_test_ch_bad, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_ulpt_start((uint8_t)k_ra_ulpt_test_ch_way, 0U));
  TEST_END("ulpt start bad channel");
}

static void test_stop_happy(void)
{
  TEST_BEGIN("ulpt stop happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ulpt_start((uint8_t)k_ra_ulpt_test_ch_0, (uint32_t)k_ra_ulpt_test_period));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ulpt_stop((uint8_t)k_ra_ulpt_test_ch_0));

  volatile r_ulpt_regs_t* reg = ra_ulpt((uint8_t)k_ra_ulpt_test_ch_0);
  TEST_ASSERT_EQ(0, (int)reg->ULPTCR);
  TEST_END("ulpt stop happy");
}

static void test_stop_bad_channel(void)
{
  TEST_BEGIN("ulpt stop bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_ulpt_stop((uint8_t)k_ra_ulpt_test_ch_bad));
  TEST_END("ulpt stop bad channel");
}

/* ---- full build-out ---- */

static uint32_t s_ulpt_cb_count;
static uint8_t  s_ulpt_cb_last_ch;

static void stub_ulpt_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_ulpt_cb_count;
  s_ulpt_cb_last_ch = ch;
}

static void prep_w43(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_ulpt_cb_count   = 0U;
  s_ulpt_cb_last_ch = 0U;
}

static void test_deinit(void)
{
  TEST_BEGIN("ulpt deinit");
  prep_w43();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_deinit((uint8_t)k_ra_ulpt_test_ch_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ulpt_deinit((uint8_t)k_ra_ulpt_test_ch_bad));
  TEST_END("ulpt deinit");
}

static void test_set_period_and_status(void)
{
  TEST_BEGIN("ulpt set_period + status");
  prep_w43();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ulpt_set_period((uint8_t)k_ra_ulpt_test_ch_0, 0x1234U));
  TEST_ASSERT_EQ((int32_t)0x1234U, (int32_t)ra_ulpt((uint8_t)k_ra_ulpt_test_ch_0)->ULPTCNT);

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ulpt_get_status((uint8_t)k_ra_ulpt_test_ch_0, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ulpt_get_status((uint8_t)k_ra_ulpt_test_ch_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ulpt_set_period((uint8_t)k_ra_ulpt_test_ch_bad, 0U));
  TEST_END("ulpt set_period + status");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("ulpt attach + dispatch");
  prep_w43();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ulpt_attach_handler(stub_ulpt_cb, (void*)(uintptr_t)0x77U));
  ra_ulpt_dispatch((uint8_t)k_ra_ulpt_test_ch_1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ulpt_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_ulpt_test_ch_1, (int32_t)s_ulpt_cb_last_ch);

  ra_ulpt_dispatch((uint8_t)k_ra_ulpt_test_ch_bad);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ulpt_cb_count);
  TEST_END("ulpt attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("ulpt power transition");
  prep_w43();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_enter_stop((uint8_t)k_ra_ulpt_test_ch_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ulpt_exit_stop((uint8_t)k_ra_ulpt_test_ch_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ulpt_enter_stop((uint8_t)k_ra_ulpt_test_ch_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ulpt_exit_stop((uint8_t)k_ra_ulpt_test_ch_bad));
  TEST_END("ulpt power transition");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_start_channel_0();
  test_start_channel_1();
  test_start_bad_channel();
  test_stop_happy();
  test_stop_bad_channel();
  test_deinit();
  test_set_period_and_status();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra_ulpt.c\n");
  return 0;
}
