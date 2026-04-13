/**
 * @file test_ra_pdm.c
 * @brief Unit tests for ra_pdm.c (PDM-IF scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_pdm_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_pdm.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_pdm_cb_count;
static uint32_t s_pdm_cb_last_mask;

static void stub_pdm_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_pdm_cb_count;
  s_pdm_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_pdm_cb_count     = 0U;
  s_pdm_cb_last_mask = 0U;
}

static void test_init(void)
{
  TEST_BEGIN("pdm init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_init());
  TEST_END("pdm init");
}

static void test_deinit(void)
{
  TEST_BEGIN("pdm deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_deinit());
  TEST_END("pdm deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("pdm status read + clear");
  prep();
  ra_pdm()->PDM_STAT = 0xDEADBEEFU;
  uint32_t mask      = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdm_get_status(nullptr));
  TEST_END("pdm status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("pdm attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdm_attach_handler(stub_pdm_cb, (void*)(uintptr_t)0x50U));
  ra_pdm()->PDM_STAT = 0xCAFEU;
  ra_pdm_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_pdm_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_pdm_cb_last_mask);
  TEST_END("pdm attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("pdm power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdm_exit_stop());
  TEST_END("pdm power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_pdm.c\n");
  return 0;
}
