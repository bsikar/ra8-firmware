/**
 * @file test_ra_dsmif.c
 * @brief Unit tests for ra_dsmif.c (DSMIF placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_dsmif.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_dsmif_mask = 0x07U, /**< Channels 0..2 enabled. */
} test_dsmif_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_dsmif_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dsmif_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_mask(void)
{
  TEST_BEGIN("open rejects zero channel_mask");
  prep();
  const ra_dsmif_cfg_t cfg = {.channel_mask = 0U, .decimation_ratio = 16U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_dsmif_open(&cfg));
  TEST_END("open rejects zero channel_mask");
}

static void test_open_close_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle ok");
  prep();
  const ra_dsmif_cfg_t cfg = {.channel_mask = (uint8_t)k_test_dsmif_mask, .decimation_ratio = 64U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_dsmif_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_dsmif_close());
  TEST_END("open / close lifecycle ok");
}

static void test_scan_pre_open(void)
{
  TEST_BEGIN("scan_start pre-open rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_dsmif_scan_start());
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_dsmif_scan_stop());
  TEST_END("scan_start pre-open rejected");
}

static void test_scan_flow(void)
{
  TEST_BEGIN("scan_start / status / scan_stop flow");
  prep();
  const ra_dsmif_cfg_t cfg = {.channel_mask = 0x01U, .decimation_ratio = 16U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_scan_start());

  uint8_t op = 0U;
  uint8_t sc = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_status_get(&op, &sc));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)op);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)sc);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_scan_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_status_get(&op, &sc));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)sc);
  TEST_END("scan_start / status / scan_stop flow");
}

static void test_read_validation(void)
{
  TEST_BEGIN("read / read32 validate args");
  prep();
  uint16_t v16 = 0xFFFFU;
  uint32_t v32 = 0xFFFFFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dsmif_read(k_ra_dsmif_channel_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_dsmif_read32(k_ra_dsmif_channel_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_dsmif_read((ra_dsmif_channel_t)9U, &v16));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_dsmif_read(k_ra_dsmif_channel_0, &v16));

  const ra_dsmif_cfg_t cfg = {.channel_mask = 0x01U, .decimation_ratio = 16U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_read(k_ra_dsmif_channel_0, &v16));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)v16);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dsmif_read32(k_ra_dsmif_channel_0, &v32));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)v32);
  TEST_END("read / read32 validate args");
}

static void test_status_null(void)
{
  TEST_BEGIN("status_get rejects NULL");
  prep();
  uint8_t op = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dsmif_status_get(nullptr, &op));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dsmif_status_get(&op, nullptr));
  TEST_END("status_get rejects NULL");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_mask();
  test_open_close_lifecycle();
  test_scan_pre_open();
  test_scan_flow();
  test_read_validation();
  test_status_null();
  (void)fprintf(stderr, "[OK ] test_ra_dsmif.c\n");
  return 0;
}
