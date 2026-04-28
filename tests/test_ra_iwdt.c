/**
 * @file test_ra_iwdt.c
 * @brief Unit tests for the IWDT driver (ra_iwdt.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_iwdt_regs.h"
#include "ra_err.h"
#include "ra_iwdt.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void test_init_returns_ok(void)
{
  TEST_BEGIN("ra_iwdt_init returns ok");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iwdt_init());
  TEST_END("ra_iwdt_init returns ok");
}

static void test_refresh_writes_sequence(void)
{
  TEST_BEGIN("ra_iwdt_refresh_deferred writes 0x00,0xFF to IWDTRR");
  ra_sim_mmap_reset();

  volatile r_iwdt_regs_t* reg = ra_iwdt();
  reg->IWDTRR                 = 0x5AU;

  /* The deferred refresh writes the second byte last. After it returns
   * IWDTRR should hold 0xFF. */
  ra_iwdt_refresh_deferred();
  TEST_ASSERT_EQ((int)k_ra_iwdt_refresh_b, (int)reg->IWDTRR);

  TEST_END("ra_iwdt_refresh_deferred writes 0x00,0xFF to IWDTRR");
}

static void test_repeated_refresh_is_safe(void)
{
  TEST_BEGIN("ra_iwdt_refresh_deferred multiple calls");
  ra_sim_mmap_reset();
  for (uint8_t i = 0U; i < 5U; ++i) {
    ra_iwdt_refresh_deferred();
  }
  TEST_ASSERT_EQ((int)k_ra_iwdt_refresh_b, (int)ra_iwdt()->IWDTRR);
  TEST_END("ra_iwdt_refresh_deferred multiple calls");
}

/* ---- full build-out ---- */

static uint32_t s_iwdt_cb_count;
static uint16_t s_iwdt_cb_last_mask;

static void stub_iwdt_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_iwdt_cb_count;
  s_iwdt_cb_last_mask = mask;
}

static void test_get_status(void)
{
  TEST_BEGIN("iwdt get_status");
  ra_sim_mmap_reset();
  ra_iwdt()->IWDTSR = (uint16_t)k_ra_iwdt_status_underflow | (uint16_t)k_ra_iwdt_status_refresh;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iwdt_get_status(&mask));
  TEST_ASSERT_EQ(
    (int32_t)((uint16_t)k_ra_iwdt_status_underflow | (uint16_t)k_ra_iwdt_status_refresh),
    (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iwdt_get_status(nullptr));
  TEST_END("iwdt get_status");
}

static void test_clear_status(void)
{
  TEST_BEGIN("iwdt clear_status");
  ra_sim_mmap_reset();
  ra_iwdt()->IWDTSR = (uint16_t)k_ra_iwdt_status_underflow;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iwdt_clear_status());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_iwdt()->IWDTSR);
  TEST_END("iwdt clear_status");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("iwdt attach + dispatch");
  ra_sim_mmap_reset();
  s_iwdt_cb_count     = 0U;
  s_iwdt_cb_last_mask = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iwdt_attach_handler(stub_iwdt_cb, (void*)(uintptr_t)0x55U));
  ra_iwdt()->IWDTSR = (uint16_t)k_ra_iwdt_status_underflow;
  ra_iwdt_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_iwdt_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_iwdt_status_underflow, (int32_t)s_iwdt_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iwdt_attach_handler(nullptr, nullptr));
  ra_iwdt()->IWDTSR = (uint16_t)k_ra_iwdt_status_refresh;
  ra_iwdt_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_iwdt_cb_count);
  TEST_END("iwdt attach + dispatch");
}

int32_t main(void)
{
  test_init_returns_ok();
  test_refresh_writes_sequence();
  test_repeated_refresh_is_safe();
  test_get_status();
  test_clear_status();
  test_attach_and_dispatch();
  (void)fprintf(stderr, "[OK ] test_ra_iwdt.c\n");
  return 0;
}
