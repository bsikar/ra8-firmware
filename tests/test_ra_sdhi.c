/**
 * @file test_ra_sdhi.c
 * @brief Unit tests for ra_sdhi.c (SD/MMC Host Interface scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_sdhi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sdhi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_sdhi_test_inst_0   = 0U,
  k_ra_sdhi_test_inst_1   = 1U,
  k_ra_sdhi_test_inst_bad = 9U,
} ra_sdhi_test_inst_t;

static uint32_t s_sdhi_cb_count;
static uint64_t s_sdhi_cb_last_mask;
static uint8_t  s_sdhi_cb_last_inst;

static void stub_sdhi_cb(void* ctx, uint8_t inst, uint64_t mask)
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

  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_0)->SD_INFO1 = 0x00000000CAFEBABEULL;
  uint64_t mask                                     = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sdhi_get_status((uint8_t)k_ra_sdhi_test_inst_0, &mask));
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEU, (int32_t)(mask & 0xFFFFFFFFU));

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sdhi_clear_status((uint8_t)k_ra_sdhi_test_inst_0, 0x00000000000000F0ULL));
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
  ra_sdhi((uint8_t)k_ra_sdhi_test_inst_1)->SD_INFO1 = 0x00000000DEADBEEFULL;
  ra_sdhi_dispatch((uint8_t)k_ra_sdhi_test_inst_1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_sdhi_cb_count);
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)(s_sdhi_cb_last_mask & 0xFFFFFFFFU));
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

int32_t main(void)
{
  test_init_happy();
  test_init_bad();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_sdhi.c\n");
  return 0;
}
