/**
 * @file test_ra_dtc.c
 * @brief Unit tests for ra_dtc.c (Data Transfer Controller)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_dtc_regs.h"
#include "ra_dtc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_dtc_test_dtcst_enable = 1U,
} ra_dtc_test_bits_t;

typedef enum : uintptr_t {
  k_ra_dtc_test_vector_addr = 0x22000400UL, /**< Arbitrary SRAM-region pointer. */
} ra_dtc_test_addr_t;

static void test_init_null_vector(void)
{
  TEST_BEGIN("dtc init null vector");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dtc_init(nullptr));
  TEST_END("dtc init null vector");
}

static void test_init_happy(void)
{
  TEST_BEGIN("dtc init happy");
  ra_sim_mmap_reset();

  void*          vec = (void*)(uintptr_t)k_ra_dtc_test_vector_addr;
  const ra_err_t err = ra_dtc_init(vec);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ(0, (int)reg->DTCCR);
  TEST_ASSERT_EQ(0, (int)reg->DTCST);
  TEST_ASSERT_EQ((int)k_ra_dtc_test_vector_addr, (int)reg->DTCVBR);
  TEST_END("dtc init happy");
}

static void test_enable_then_disable(void)
{
  TEST_BEGIN("dtc enable then disable");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_enable());
  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ((int)k_ra_dtc_test_dtcst_enable, (int)reg->DTCST);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_disable());
  TEST_ASSERT_EQ(0, (int)reg->DTCST);
  TEST_END("dtc enable then disable");
}

int32_t main(void)
{
  test_init_null_vector();
  test_init_happy();
  test_enable_then_disable();
  (void)fprintf(stderr, "[OK  ] test_ra_dtc.c\n");
  return 0;
}
