/**
 * @file test_ra_i3c.c
 * @brief Unit tests for ra_i3c.c (I3C Bus Interface scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_i3c_regs.h"
#include "ra_err.h"
#include "ra_i3c.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_i3c_cb_count;
static uint32_t s_i3c_cb_last_mask;

static void stub_i3c_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_i3c_cb_count;
  s_i3c_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_i3c_cb_count     = 0U;
  s_i3c_cb_last_mask = 0U;
}

static void test_init(void)
{
  TEST_BEGIN("i3c init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_END("i3c init");
}

static void test_deinit(void)
{
  TEST_BEGIN("i3c deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_deinit());
  TEST_END("i3c deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("i3c status read + clear");
  prep();
  ra_i3c()->INST = 0xDEADBEEFU;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_i3c_get_status(nullptr));
  TEST_END("i3c status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("i3c attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_i3c_attach_handler(stub_i3c_cb, (void*)(uintptr_t)0x13U));
  ra_i3c()->INST = 0xCAFEU;
  ra_i3c_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_i3c_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_i3c_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_attach_handler(nullptr, nullptr));
  ra_i3c()->INST = 0xBEEFU;
  ra_i3c_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_i3c_cb_count);
  TEST_END("i3c attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("i3c power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_i3c_exit_stop());
  TEST_END("i3c power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_i3c.c\n");
  return 0;
}
