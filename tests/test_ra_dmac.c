/**
 * @file test_ra_dmac.c
 * @brief Unit tests for ra_dmac.c (Direct Memory Access Controller)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_dmac_regs.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_ra_dmac_test_src    = 0x22000100UL,
  k_ra_dmac_test_dst    = 0x22000200UL,
  k_ra_dmac_test_count  = 0x0040U,
  k_ra_dmac_test_enable = 0x01U,
} ra_dmac_test_const_t;

typedef enum : uint8_t {
  k_ra_dmac_test_channel_valid = 0U,
  k_ra_dmac_test_channel_last  = 7U,
  k_ra_dmac_test_channel_bad   = 8U,
} ra_dmac_test_channel_t;

static void test_start_null_cfg(void)
{
  TEST_BEGIN("dmac start null cfg");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_valid, nullptr));
  TEST_END("dmac start null cfg");
}

static void test_start_bad_channel(void)
{
  TEST_BEGIN("dmac start bad channel");
  ra_sim_mmap_reset();

  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra_dmac_test_src,
    .dst     = (uint32_t)k_ra_dmac_test_dst,
    .count   = (uint16_t)k_ra_dmac_test_count,
    .width   = k_ra_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_bad, &cfg));
  TEST_END("dmac start bad channel");
}

static void test_start_happy_both_inc(void)
{
  TEST_BEGIN("dmac start happy both inc");
  ra_sim_mmap_reset();

  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra_dmac_test_src,
    .dst     = (uint32_t)k_ra_dmac_test_dst,
    .count   = (uint16_t)k_ra_dmac_test_count,
    .width   = k_ra_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_valid, &cfg));

  volatile r_dmac_channel_regs_t* reg = ra_dmac((uint8_t)k_ra_dmac_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_dmac_test_src, (int)reg->DMSAR);
  TEST_ASSERT_EQ((int)k_ra_dmac_test_dst, (int)reg->DMDAR);
  TEST_ASSERT_EQ((int)k_ra_dmac_test_count, (int)reg->DMCRA);
  TEST_ASSERT_EQ((int)k_ra_dmac_test_enable, (int)reg->DMCNT);
  TEST_END("dmac start happy both inc");
}

static void test_start_no_src_inc(void)
{
  TEST_BEGIN("dmac start no src inc");
  ra_sim_mmap_reset();

  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra_dmac_test_src,
    .dst     = (uint32_t)k_ra_dmac_test_dst,
    .count   = (uint16_t)k_ra_dmac_test_count,
    .width   = k_ra_dmac_width_byte,
    .src_inc = false,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_valid, &cfg));
  TEST_END("dmac start no src inc");
}

static void test_start_no_dst_inc(void)
{
  TEST_BEGIN("dmac start no dst inc");
  ra_sim_mmap_reset();

  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra_dmac_test_src,
    .dst     = (uint32_t)k_ra_dmac_test_dst,
    .count   = (uint16_t)k_ra_dmac_test_count,
    .width   = k_ra_dmac_width_half,
    .src_inc = true,
    .dst_inc = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_valid, &cfg));
  TEST_END("dmac start no dst inc");
}

static void test_start_neither_inc(void)
{
  TEST_BEGIN("dmac start neither inc");
  ra_sim_mmap_reset();

  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra_dmac_test_src,
    .dst     = (uint32_t)k_ra_dmac_test_dst,
    .count   = (uint16_t)k_ra_dmac_test_count,
    .width   = k_ra_dmac_width_byte,
    .src_inc = false,
    .dst_inc = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dmac_start((uint8_t)k_ra_dmac_test_channel_last, &cfg));
  TEST_END("dmac start neither inc");
}

static void test_stop_happy(void)
{
  TEST_BEGIN("dmac stop happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dmac_stop((uint8_t)k_ra_dmac_test_channel_valid));

  volatile r_dmac_channel_regs_t* reg = ra_dmac((uint8_t)k_ra_dmac_test_channel_valid);
  TEST_ASSERT_EQ(0, (int)reg->DMCNT);
  TEST_END("dmac stop happy");
}

static void test_stop_bad_channel(void)
{
  TEST_BEGIN("dmac stop bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_dmac_stop((uint8_t)k_ra_dmac_test_channel_bad));
  TEST_END("dmac stop bad channel");
}

int32_t main(void)
{
  test_start_null_cfg();
  test_start_bad_channel();
  test_start_happy_both_inc();
  test_start_no_src_inc();
  test_start_no_dst_inc();
  test_start_neither_inc();
  test_stop_happy();
  test_stop_bad_channel();
  (void)fprintf(stderr, "[OK  ] test_ra_dmac.c\n");
  return 0;
}
