/**
 * @file test_ra_trng.c
 * @brief Unit tests for ra_trng.c (True Random Number Generator driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_trng_regs.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_trng.h"
#include "unity_minimal.h"

static void test_init_enables_collector(void)
{
  TEST_BEGIN("trng init enables collector");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_init());
  volatile r_trng_regs_t* reg = ra_trng();
  TEST_ASSERT_EQ((int)k_ra_trng_mask_en, (int)(reg->TRNGCR & (uint8_t)k_ra_trng_mask_en));
  TEST_END("trng init enables collector");
}

static void test_read_happy(void)
{
  TEST_BEGIN("trng read happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_init());

  uint32_t value = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_read(&value));
  /* Simulator LCG: seed * 1103515245 + 12345 is never 0 for seed != 0. */
  TEST_ASSERT(value != 0U);
  TEST_END("trng read happy");
}

static void test_read_advances_state(void)
{
  TEST_BEGIN("trng read advances state");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_init());

  uint32_t a = 0U;
  uint32_t b = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_read(&a));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_read(&b));
  TEST_ASSERT(a != b);
  TEST_END("trng read advances state");
}

static void test_read_null_out(void)
{
  TEST_BEGIN("trng read null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_trng_init());
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_trng_read(nullptr));
  TEST_END("trng read null out");
}

int32_t main(void)
{
  test_init_enables_collector();
  test_read_happy();
  test_read_advances_state();
  test_read_null_out();
  (void)fprintf(stderr, "[OK  ] test_ra_trng.c\n");
  return 0;
}
