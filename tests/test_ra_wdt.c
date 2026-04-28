/**
 * @file test_ra_wdt.c
 * @brief Unit tests for ra_wdt.c (software WDT driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_wdt_regs.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_wdt.h"
#include "unity_minimal.h"

static void test_wdt_init_ok(void)
{
  TEST_BEGIN("ra_wdt_init returns ok");
  ra_sim_mmap_reset();
  const ra_wdt_cfg_t cfg = {
    .timeout       = k_ra_wdt_timeout_16384,
    .clock_div     = k_ra_wdt_clkdiv_8192,
    .window_start  = k_ra_wdt_window_start_100,
    .window_end    = k_ra_wdt_window_end_0,
    .on_expiry     = k_ra_wdt_on_expiry_reset,
    .stop_in_sleep = k_ra_wdt_sleep_keep_count,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_wdt_init(&cfg));
  TEST_END("ra_wdt_init returns ok");
}

static void test_wdt_refresh_writes_sequence(void)
{
  TEST_BEGIN("ra_wdt_refresh_deferred writes 0xFF to WDTRR");
  ra_sim_mmap_reset();

  volatile r_wdt_regs_t* reg = ra_wdt();
  reg->WDTRR                 = 0x42U;

  ra_wdt_refresh_deferred();
  /* Last byte of the unlock sequence should be 0xFF. */
  TEST_ASSERT_EQ((int)k_ra_wdt_refresh_b, (int)reg->WDTRR);

  TEST_END("ra_wdt_refresh_deferred writes 0xFF to WDTRR");
}

static void test_wdt_refresh_many(void)
{
  TEST_BEGIN("ra_wdt_refresh_deferred is safe to loop");
  ra_sim_mmap_reset();
  for (uint8_t i = 0U; i < 8U; ++i) {
    ra_wdt_refresh_deferred();
  }
  TEST_ASSERT_EQ((int)k_ra_wdt_refresh_b, (int)ra_wdt()->WDTRR);
  TEST_END("ra_wdt_refresh_deferred is safe to loop");
}

/* ---- full build-out ---- */

static uint32_t s_wdt_cb_count;
static uint16_t s_wdt_cb_last_mask;

static void stub_wdt_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_wdt_cb_count;
  s_wdt_cb_last_mask = mask;
}

static void test_wdt_get_status(void)
{
  TEST_BEGIN("wdt get_status");
  ra_sim_mmap_reset();
  ra_wdt()->WDTSR = (uint16_t)k_ra_wdt_status_underflow | (uint16_t)k_ra_wdt_status_refresh;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_wdt_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)((uint16_t)k_ra_wdt_status_underflow | (uint16_t)k_ra_wdt_status_refresh),
                 (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_wdt_get_status(nullptr));
  TEST_END("wdt get_status");
}

static void test_wdt_clear_status(void)
{
  TEST_BEGIN("wdt clear_status");
  ra_sim_mmap_reset();
  ra_wdt()->WDTSR = (uint16_t)k_ra_wdt_status_underflow;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_wdt_clear_status());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_wdt()->WDTSR);
  TEST_END("wdt clear_status");
}

static void test_wdt_attach_and_dispatch(void)
{
  TEST_BEGIN("wdt attach + dispatch");
  ra_sim_mmap_reset();
  s_wdt_cb_count     = 0U;
  s_wdt_cb_last_mask = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_wdt_attach_handler(stub_wdt_cb, (void*)(uintptr_t)0x99U));
  ra_wdt()->WDTSR = (uint16_t)k_ra_wdt_status_underflow;
  ra_wdt_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_wdt_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_wdt_status_underflow, (int32_t)s_wdt_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_wdt_attach_handler(nullptr, nullptr));
  ra_wdt()->WDTSR = (uint16_t)k_ra_wdt_status_refresh;
  ra_wdt_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_wdt_cb_count);
  TEST_END("wdt attach + dispatch");
}

int32_t main(void)
{
  test_wdt_init_ok();
  test_wdt_refresh_writes_sequence();
  test_wdt_refresh_many();
  test_wdt_get_status();
  test_wdt_clear_status();
  test_wdt_attach_and_dispatch();
  (void)fprintf(stderr, "[OK ] test_ra_wdt.c\n");
  return 0;
}
