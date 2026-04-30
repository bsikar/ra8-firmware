/**
 * @file test_ra_kint.c
 * @brief Unit tests for ra_kint.c (key-matrix interrupt driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_kint_regs.h"
#include "ra_err.h"
#include "ra_kint.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_kint_row_mask = 0xA5U, /**< Sample mask. */
  k_test_kint_state    = 0x42U, /**< Sample live row state. */
} test_kint_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_kint_close();
}

/* ---- Lifecycle ---- */

static void test_open_writes_registers(void)
{
  TEST_BEGIN("open writes KRCTL/KRM/KRF and asserts KRE");
  prep();

  const ra_kint_cfg_t cfg = {
    .edge     = k_ra_kint_edge_falling,
    .row_mask = (uint8_t)k_test_kint_row_mask,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_open(&cfg));

  volatile r_kint_regs_t* reg = ra_kint();
  TEST_ASSERT_EQ((int32_t)k_test_kint_row_mask, (int32_t)reg->KRM);
  TEST_ASSERT_EQ(0, (int)reg->KRF);
  /* falling edge -> KREG bit 0 == 0; KRE bit 7 set -> 0x80. */
  TEST_ASSERT_EQ((int32_t)0x80U, (int32_t)reg->KRCTL);
  TEST_END("open writes KRCTL/KRM/KRF and asserts KRE");
}

static void test_open_rising_edge(void)
{
  TEST_BEGIN("open with rising edge sets KREG bit");
  prep();

  const ra_kint_cfg_t cfg = {
    .edge     = k_ra_kint_edge_rising,
    .row_mask = 0xFFU,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_open(&cfg));
  /* rising edge -> KREG=1; KRE=1 -> 0x81. */
  TEST_ASSERT_EQ((int32_t)0x81U, (int32_t)ra_kint()->KRCTL);
  TEST_END("open with rising edge sets KREG bit");
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_kint_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_edge(void)
{
  TEST_BEGIN("open rejects bogus edge");
  prep();
  const ra_kint_cfg_t cfg = {
    .edge     = (ra_kint_edge_t)9U,
    .row_mask = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_kint_open(&cfg));
  TEST_END("open rejects bogus edge");
}

static void test_close_without_open(void)
{
  TEST_BEGIN("close without open returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_kint_close());
  TEST_END("close without open returns invalid_state");
}

static void test_close_clears_registers(void)
{
  TEST_BEGIN("close zeros KRCTL/KRM/KRF");
  prep();
  const ra_kint_cfg_t cfg = {
    .edge     = k_ra_kint_edge_falling,
    .row_mask = 0xFFU,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_close());

  volatile r_kint_regs_t* reg = ra_kint();
  TEST_ASSERT_EQ(0, (int)reg->KRCTL);
  TEST_ASSERT_EQ(0, (int)reg->KRM);
  TEST_ASSERT_EQ(0, (int)reg->KRF);
  TEST_END("close zeros KRCTL/KRM/KRF");
}

/* ---- read_matrix ---- */

static void test_read_matrix_pre_open(void)
{
  TEST_BEGIN("read_matrix rejects pre-open");
  prep();
  uint8_t state = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_kint_read_matrix(&state));
  TEST_END("read_matrix rejects pre-open");
}

static void test_read_matrix_null(void)
{
  TEST_BEGIN("read_matrix rejects NULL");
  prep();
  const ra_kint_cfg_t cfg = {.edge = k_ra_kint_edge_falling, .row_mask = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_kint_read_matrix(nullptr));
  TEST_END("read_matrix rejects NULL");
}

static void test_read_matrix_returns_krstr(void)
{
  TEST_BEGIN("read_matrix returns KRSTR snapshot");
  prep();
  const ra_kint_cfg_t cfg = {.edge = k_ra_kint_edge_falling, .row_mask = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_open(&cfg));

  ra_kint()->KRSTR = (uint8_t)k_test_kint_state;
  uint8_t state    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_kint_read_matrix(&state));
  TEST_ASSERT_EQ((int32_t)k_test_kint_state, (int32_t)state);
  TEST_END("read_matrix returns KRSTR snapshot");
}

int32_t main(void)
{
  test_open_writes_registers();
  test_open_rising_edge();
  test_open_null();
  test_open_bad_edge();
  test_close_without_open();
  test_close_clears_registers();
  test_read_matrix_pre_open();
  test_read_matrix_null();
  test_read_matrix_returns_krstr();
  (void)fprintf(stderr, "[OK ] test_ra_kint.c\n");
  return 0;
}
