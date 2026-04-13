/**
 * @file test_ra_doc.c
 * @brief Unit tests for ra_doc.c (Data Operation Circuit driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_doc_regs.h"
#include "ra_doc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_ra_doc_test_a      = 0x1234U,
  k_ra_doc_test_b      = 0x00FFU,
  k_ra_doc_test_sum    = 0x1333U,
  k_ra_doc_test_diff   = 0x1135U,
  k_ra_doc_test_wrap_a = 0x0005U,
  k_ra_doc_test_wrap_b = 0x000AU,
  k_ra_doc_test_wrap   = 0xFFFBU,
} ra_doc_test_value_t;

static void test_init_clears_regs(void)
{
  TEST_BEGIN("doc init clears regs");
  ra_sim_mmap_reset();

  volatile r_doc_regs_t* reg = ra_doc();
  reg->DOCR                  = 0xFFU;
  reg->DODIR                 = 0xAAAAU;
  reg->DODSR                 = 0x5555U;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_init());
  TEST_ASSERT_EQ(0, (int)reg->DOCR);
  TEST_ASSERT_EQ(0, (int)reg->DODIR);
  TEST_ASSERT_EQ(0, (int)reg->DODSR);
  TEST_END("doc init clears regs");
}

static void test_add16_happy(void)
{
  TEST_BEGIN("doc add16 happy");
  ra_sim_mmap_reset();

  uint16_t sum = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_doc_add16((uint16_t)k_ra_doc_test_a, (uint16_t)k_ra_doc_test_b, &sum));
  TEST_ASSERT_EQ((int)k_ra_doc_test_sum, (int)sum);

  /* Mode bits OMS=01 should be visible in DOCR. */
  volatile r_doc_regs_t* reg = ra_doc();
  TEST_ASSERT_EQ((int)k_ra_doc_mode_add, (int)(reg->DOCR & (uint8_t)k_ra_doc_mask_oms));
  TEST_END("doc add16 happy");
}

static void test_add16_wraps(void)
{
  TEST_BEGIN("doc add16 wraps on overflow");
  ra_sim_mmap_reset();

  uint16_t sum = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_doc_add16(0xFFFFU, 0x0002U, &sum));
  TEST_ASSERT_EQ(0x0001, (int)sum);
  TEST_END("doc add16 wraps on overflow");
}

static void test_add16_null_out(void)
{
  TEST_BEGIN("doc add16 null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_doc_add16(1U, 2U, nullptr));
  TEST_END("doc add16 null out");
}

static void test_sub16_happy(void)
{
  TEST_BEGIN("doc sub16 happy");
  ra_sim_mmap_reset();

  uint16_t diff = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_doc_sub16((uint16_t)k_ra_doc_test_a, (uint16_t)k_ra_doc_test_b, &diff));
  TEST_ASSERT_EQ((int)k_ra_doc_test_diff, (int)diff);

  volatile r_doc_regs_t* reg = ra_doc();
  TEST_ASSERT_EQ((int)k_ra_doc_mode_subtract, (int)(reg->DOCR & (uint8_t)k_ra_doc_mask_oms));
  TEST_END("doc sub16 happy");
}

static void test_sub16_wraps(void)
{
  TEST_BEGIN("doc sub16 wraps on borrow");
  ra_sim_mmap_reset();

  uint16_t diff = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_doc_sub16((uint16_t)k_ra_doc_test_wrap_a, (uint16_t)k_ra_doc_test_wrap_b, &diff));
  TEST_ASSERT_EQ((int)k_ra_doc_test_wrap, (int)diff);
  TEST_END("doc sub16 wraps on borrow");
}

static void test_sub16_null_out(void)
{
  TEST_BEGIN("doc sub16 null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_doc_sub16(10U, 4U, nullptr));
  TEST_END("doc sub16 null out");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_add16_happy();
  test_add16_wraps();
  test_add16_null_out();
  test_sub16_happy();
  test_sub16_wraps();
  test_sub16_null_out();
  (void)fprintf(stderr, "[OK  ] test_ra_doc.c\n");
  return 0;
}
