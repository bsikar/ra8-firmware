/**
 * @file test_ra8_wdt.c
 * @brief Unit tests for ra8_wdt.c (software WDT driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_wdt.h"
#include "ra8_wdt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_wdt_t
 * @brief Sentinel written into WDTRR before the refresh under test.
 */
typedef enum : uint8_t {
  k_t_wdtrr_sentinel = 0x42U, /**< Any value the refresh must overwrite; proves
                                   the driver wrote the register at all.         */
} t_wdt_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_init_ok(void)
{
  TEST_BEGIN("ra8_wdt_init returns ok");
  ra8_fake_mmap_reset();
  const ra8_wdt_cfg_t cfg = {
    .timeout       = k_ra8_wdt_timeout_16384,
    .clock_div     = k_ra8_wdt_clkdiv_8192,
    .window_start  = k_ra8_wdt_window_start_100,
    .window_end    = k_ra8_wdt_window_end_0,
    .on_expiry     = k_ra8_wdt_on_expiry_reset,
    .stop_in_sleep = k_ra8_wdt_sleep_keep_count,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_init(&cfg));
  TEST_END("ra8_wdt_init returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_refresh_writes_sequence(void)
{
  TEST_BEGIN("ra8_wdt_refresh_deferred writes 0xFF to WDTRR");
  ra8_fake_mmap_reset();

  volatile r_wdt_regs_t* reg = ra8_wdt();
  reg->WDTRR                 = k_t_wdtrr_sentinel;

  ra8_wdt_refresh_deferred();
  /* Last byte of the unlock sequence should be 0xFF. */
  TEST_ASSERT_EQ(k_ra8_wdt_refresh_b, reg->WDTRR);

  TEST_END("ra8_wdt_refresh_deferred writes 0xFF to WDTRR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_refresh_many(void)
{
  TEST_BEGIN("ra8_wdt_refresh_deferred is safe to loop");
  ra8_fake_mmap_reset();
  for (uint8_t i = 0U; i < 8U; ++i) {
    ra8_wdt_refresh_deferred();
  }
  TEST_ASSERT_EQ(k_ra8_wdt_refresh_b, ra8_wdt()->WDTRR);
  TEST_END("ra8_wdt_refresh_deferred is safe to loop");
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_get_status(void)
{
  TEST_BEGIN("wdt get_status");
  ra8_fake_mmap_reset();
  ra8_wdt()->WDTSR = (uint16_t)k_ra8_wdt_status_underflow | (uint16_t)k_ra8_wdt_status_refresh;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_get_status(&mask));
  TEST_ASSERT_EQ(((uint16_t)k_ra8_wdt_status_underflow | (uint16_t)k_ra8_wdt_status_refresh), mask);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_get_status(nullptr));
  TEST_END("wdt get_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_clear_status(void)
{
  TEST_BEGIN("wdt clear_status");
  ra8_fake_mmap_reset();
  ra8_wdt()->WDTSR = (uint16_t)k_ra8_wdt_status_underflow;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_clear_status());
  TEST_ASSERT_EQ(0, ra8_wdt()->WDTSR);
  TEST_END("wdt clear_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wdt_attach_and_dispatch(void)
{
  TEST_BEGIN("wdt attach + dispatch");
  ra8_fake_mmap_reset();
  s_wdt_cb_count     = 0U;
  s_wdt_cb_last_mask = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_attach_handler(stub_wdt_cb, (void*)(uintptr_t)0x99U));
  ra8_wdt()->WDTSR = (uint16_t)k_ra8_wdt_status_underflow;
  ra8_wdt_dispatch();
  TEST_ASSERT_EQ(1, s_wdt_cb_count);
  TEST_ASSERT_EQ(k_ra8_wdt_status_underflow, s_wdt_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_attach_handler(nullptr, nullptr));
  ra8_wdt()->WDTSR = (uint16_t)k_ra8_wdt_status_refresh;
  ra8_wdt_dispatch();
  TEST_ASSERT_EQ(1, s_wdt_cb_count);
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
  (void)fprintf(stderr, "[OK ] test_ra8_wdt.c\n");
  return 0;
}
