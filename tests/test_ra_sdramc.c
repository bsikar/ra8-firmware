/**
 * @file test_ra_sdramc.c
 * @brief Unit tests for the SDRAMC driver (ra_sdramc.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_sdramc_regs.h"
#include "ra_err.h"
#include "ra_sdramc.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_sdccr_default  = 0x0011U,
  k_test_sdrfcr_default = 0x002BU,
} test_sdramc_defaults_t;

typedef enum : uint32_t {
  k_test_sdtr_default = 0x00010222UL,
} test_sdramc_timing_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_writes_defaults(void)
{
  TEST_BEGIN("ra_sdramc_init writes expected register defaults");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sdramc_init());

  volatile r_sdramc_regs_t* reg = ra_sdramc();
  TEST_ASSERT_EQ((int)k_test_sdccr_default, (int)reg->SDCCR);
  TEST_ASSERT_EQ(0, (int)reg->SDCMOD);
  TEST_ASSERT_EQ(0, (int)reg->SDAMOD);
  TEST_ASSERT_EQ((int)k_test_sdtr_default, (int)reg->SDTR);
  TEST_ASSERT_EQ((int)k_test_sdrfcr_default, (int)reg->SDRFCR);
  TEST_ASSERT_EQ(1, (int)reg->SDRFEN);
  TEST_ASSERT_EQ(1, (int)reg->SDICR);

  TEST_END("ra_sdramc_init writes expected register defaults");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_idempotent(void)
{
  TEST_BEGIN("ra_sdramc_init is idempotent");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sdramc_init());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sdramc_init());
  volatile r_sdramc_regs_t* reg = ra_sdramc();
  TEST_ASSERT_EQ((int)k_test_sdccr_default, (int)reg->SDCCR);
  TEST_END("ra_sdramc_init is idempotent");
}

/* ---- lifecycle + power ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("sdramc deinit");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_deinit());
  volatile r_sdramc_regs_t* reg = ra_sdramc();
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->SDRFEN);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->SDICR);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->SDCCR);
  TEST_END("sdramc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_refresh_interval(void)
{
  TEST_BEGIN("sdramc set_refresh_interval");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_set_refresh_interval(0x00ABU));
  TEST_ASSERT_EQ((int32_t)0x00ABU, (int32_t)ra_sdramc()->SDRFCR);
  TEST_END("sdramc set_refresh_interval");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status(void)
{
  TEST_BEGIN("sdramc get_status");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_init());

  uint8_t enabled = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_get_status(&enabled));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)enabled);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sdramc_get_status(nullptr));
  TEST_END("sdramc get_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("sdramc power transition");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_enter_stop());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_sdramc()->SDRFEN);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sdramc_exit_stop());
  TEST_ASSERT_EQ((int32_t)1, (int32_t)ra_sdramc()->SDRFEN);
  TEST_END("sdramc power transition");
}

int32_t main(void)
{
  test_init_writes_defaults();
  test_init_idempotent();
  test_deinit();
  test_set_refresh_interval();
  test_get_status();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra_sdramc.c\n");
  return 0;
}
