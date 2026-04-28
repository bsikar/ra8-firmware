/**
 * @file test_ra_reset.c
 * @brief Unit tests for ra_reset.c (Reset cause + software reset driver)
 *
 * @details
 * Exercises the reset HAL driver against the host-side ``ra_sim_mmap``
 * shim. The shim maps both the SYSC peripheral window (0x4001E000)
 * and the Cortex-M85 SCB AIRCR (0xE000ED0C), so writes from the
 * driver land in ordinary host RAM and the test can read them back.
 *
 * Cited HUM register ranges (per ``CHAPTER_MAP.md`` Ch 6 = pages 244..277):
 *   - HUM Ch 6.1 p 244 -- reset taxonomy
 *   - HUM Ch 6.2.1 p 256 -- RSTSAR
 *   - HUM Ch 6.2.2 p 257 -- RSTSR0
 *   - HUM Ch 6.2.3 p 258 -- RSTSR1
 *   - HUM Ch 6.2.4 p 261 -- RSTSR2
 *   - HUM Ch 6.2.5 p 261 -- RSTSR3
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_reset_regs.h"
#include "ra_err.h"
#include "ra_reset.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_reset_test_const_t
 * @brief Magic numbers used only inside this test TU.
 */
typedef enum : uint32_t {
  k_ra_reset_test_aircr_zero    = 0UL,
  k_ra_reset_test_attribution_v = 0x0000001FUL,
  k_ra_reset_test_clear_all_r0  = 0x000000FFUL,
  k_ra_reset_test_clear_swrf    = ((uint32_t)0x00000004UL << 8U),
} ra_reset_test_const_t;

/**
 * @brief Test-only hook implemented in ``libs/ra_hal/src/ra_reset.c``
 *        when ``RA_SIMULATOR_MODE`` is defined. Drops the cached
 *        snapshot so every case starts with the driver in its
 *        just-loaded state.
 */
void ra_reset_test_only_reset_state(void);

/**
 * @brief Reset every state the driver caches between cases.
 *
 * @details
 * ``ra_reset_init`` keeps a file-scope snapshot. We zero the sim mmap
 * (clears every register) **and** drop the driver's cached snapshot,
 * so each test starts with both hardware and driver state at zero.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  ra_reset_test_only_reset_state();
}

/* ---------------------------------------------------------------------------
 * get_cause -- happy paths
 * ---------------------------------------------------------------------------
 */

static void test_get_cause_unknown_when_all_zero(void)
{
  TEST_BEGIN("reset get_cause unknown when all zero");
  prep();

  ra_reset_cause_t cause = k_ra_reset_cause_software;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_unknown, (int32_t)cause);

  TEST_END("reset get_cause unknown when all zero");
}

static void test_get_cause_porf(void)
{
  TEST_BEGIN("reset get_cause power-on");
  prep();

  *ra_reset_rstsr0() = (uint8_t)k_ra_reset_rstsr0_porf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_power_on, (int32_t)cause);

  TEST_END("reset get_cause power-on");
}

static void test_get_cause_swrf(void)
{
  TEST_BEGIN("reset get_cause software");
  prep();

  *ra_reset_rstsr1() = (uint32_t)k_ra_reset_rstsr1_swrf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_software, (int32_t)cause);

  TEST_END("reset get_cause software");
}

static void test_get_cause_iwdt(void)
{
  TEST_BEGIN("reset get_cause iwdt");
  prep();

  *ra_reset_rstsr1() = (uint32_t)k_ra_reset_rstsr1_iwdtrf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_iwdt, (int32_t)cause);

  TEST_END("reset get_cause iwdt");
}

static void test_get_cause_temperature(void)
{
  TEST_BEGIN("reset get_cause temperature");
  prep();

  *ra_reset_rstsr3() = (uint8_t)k_ra_reset_rstsr3_temprf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_temperature, (int32_t)cause);

  TEST_END("reset get_cause temperature");
}

static void test_get_cause_warm_start(void)
{
  TEST_BEGIN("reset get_cause warm start");
  prep();

  *ra_reset_rstsr2() = (uint8_t)k_ra_reset_rstsr2_cwsf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_warm_start, (int32_t)cause);

  TEST_END("reset get_cause warm start");
}

static void test_get_cause_priority_porf_over_iwdt(void)
{
  TEST_BEGIN("reset get_cause priority POR > IWDT");
  prep();

  /* Both flags latched simultaneously -- POR (in RSTSR0) must win. */
  *ra_reset_rstsr0() = (uint8_t)k_ra_reset_rstsr0_porf_msk;
  *ra_reset_rstsr1() = (uint32_t)k_ra_reset_rstsr1_iwdtrf_msk;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_power_on, (int32_t)cause);

  TEST_END("reset get_cause priority POR > IWDT");
}

/* ---------------------------------------------------------------------------
 * get_cause -- bad arg
 * ---------------------------------------------------------------------------
 */

static void test_get_cause_null_out(void)
{
  TEST_BEGIN("reset get_cause null out");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_reset_get_cause(nullptr));

  TEST_END("reset get_cause null out");
}

/* ---------------------------------------------------------------------------
 * init / get_raw / snapshot caching
 * ---------------------------------------------------------------------------
 */

static void test_init_snapshot_survives_clear(void)
{
  TEST_BEGIN("reset init snapshot survives subsequent register clear");
  prep();

  *ra_reset_rstsr1() = (uint32_t)k_ra_reset_rstsr1_swrf_msk;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_init());

  /* External code (e.g. another driver) clears the live register. */
  *ra_reset_rstsr1() = 0U;

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_cause(&cause));
  /* The snapshot still says SOFTWARE because init captured it. */
  TEST_ASSERT_EQ((int32_t)k_ra_reset_cause_software, (int32_t)cause);

  TEST_END("reset init snapshot survives subsequent register clear");
}

static void test_get_raw_returns_register_words(void)
{
  TEST_BEGIN("reset get_raw returns register words");
  prep();

  *ra_reset_rstsr0() = (uint8_t)k_ra_reset_rstsr0_lvd1rf_msk;
  *ra_reset_rstsr1() = (uint32_t)k_ra_reset_rstsr1_wdt1rf_msk;
  *ra_reset_rstsr2() = (uint8_t)k_ra_reset_rstsr2_cwsf_msk;
  *ra_reset_rstsr3() = (uint8_t)k_ra_reset_rstsr3_cvmrf_msk;

  ra_reset_raw_t raw = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_raw(&raw));

  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr0_lvd1rf_msk, (int32_t)raw.rstsr0);
  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr1_wdt1rf_msk, (int32_t)raw.rstsr1);
  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr2_cwsf_msk, (int32_t)raw.rstsr2);
  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr3_cvmrf_msk, (int32_t)raw.rstsr3);

  TEST_END("reset get_raw returns register words");
}

static void test_get_raw_null_out(void)
{
  TEST_BEGIN("reset get_raw null out");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_reset_get_raw(nullptr));

  TEST_END("reset get_raw null out");
}

/* ---------------------------------------------------------------------------
 * clear_cause
 * ---------------------------------------------------------------------------
 */

static void test_clear_cause_rstsr0(void)
{
  TEST_BEGIN("reset clear_cause clears RSTSR0 bits");
  prep();

  *ra_reset_rstsr0() =
    (uint8_t)((uint8_t)k_ra_reset_rstsr0_porf_msk | (uint8_t)k_ra_reset_rstsr0_lvd1rf_msk);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_reset_clear_cause((uint32_t)k_ra_reset_test_clear_all_r0));

  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_reset_rstsr0());

  TEST_END("reset clear_cause clears RSTSR0 bits");
}

static void test_clear_cause_rstsr1_swrf(void)
{
  TEST_BEGIN("reset clear_cause clears RSTSR1.SWRF only");
  prep();

  *ra_reset_rstsr1() =
    (uint32_t)k_ra_reset_rstsr1_swrf_msk | (uint32_t)k_ra_reset_rstsr1_iwdtrf_msk;

  /* Mask packs RSTSR1.SWRF (bit 2) at mask[10] -- helper enum value. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_reset_clear_cause((uint32_t)k_ra_reset_test_clear_swrf));

  /* SWRF cleared, IWDTRF preserved. */
  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr1_iwdtrf_msk, (int32_t)*ra_reset_rstsr1());

  TEST_END("reset clear_cause clears RSTSR1.SWRF only");
}

static void test_clear_cause_zero_mask_is_noop(void)
{
  TEST_BEGIN("reset clear_cause zero mask is a no-op");
  prep();

  *ra_reset_rstsr0() = (uint8_t)k_ra_reset_rstsr0_porf_msk;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_clear_cause(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_rstsr0_porf_msk, (int32_t)*ra_reset_rstsr0());

  TEST_END("reset clear_cause zero mask is a no-op");
}

/* ---------------------------------------------------------------------------
 * RSTSAR
 * ---------------------------------------------------------------------------
 */

static void test_get_attribution(void)
{
  TEST_BEGIN("reset get_attribution returns RSTSAR");
  prep();

  *ra_reset_rstsar() = (uint32_t)k_ra_reset_test_attribution_v;

  uint32_t value = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_reset_get_attribution(&value));
  TEST_ASSERT_EQ((int32_t)k_ra_reset_test_attribution_v, (int32_t)value);

  TEST_END("reset get_attribution returns RSTSAR");
}

static void test_get_attribution_null_out(void)
{
  TEST_BEGIN("reset get_attribution null out");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_reset_get_attribution(nullptr));

  TEST_END("reset get_attribution null out");
}

/* ---------------------------------------------------------------------------
 * Software reset
 * ---------------------------------------------------------------------------
 */

static void test_software_reset_writes_aircr(void)
{
  TEST_BEGIN("reset software_reset writes AIRCR with VECTKEY+SYSRESETREQ");
  prep();

  *ra_reset_aircr() = (uint32_t)k_ra_reset_test_aircr_zero;

  /* On RA_SIMULATOR_MODE this returns instead of looping. */
  ra_reset_software_reset();

  const uint32_t expected =
    ((uint32_t)k_ra_reset_aircr_vectkey_value << (uint32_t)k_ra_reset_aircr_vectkey_pos) |
    (uint32_t)k_ra_reset_aircr_sysresetreq_msk;
  TEST_ASSERT_EQ((int64_t)expected, (int64_t)*ra_reset_aircr());

  TEST_END("reset software_reset writes AIRCR with VECTKEY+SYSRESETREQ");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

int32_t main(void)
{
  test_get_cause_unknown_when_all_zero();
  test_get_cause_porf();
  test_get_cause_swrf();
  test_get_cause_iwdt();
  test_get_cause_temperature();
  test_get_cause_warm_start();
  test_get_cause_priority_porf_over_iwdt();
  test_get_cause_null_out();
  test_init_snapshot_survives_clear();
  test_get_raw_returns_register_words();
  test_get_raw_null_out();
  test_clear_cause_rstsr0();
  test_clear_cause_rstsr1_swrf();
  test_clear_cause_zero_mask_is_noop();
  test_get_attribution();
  test_get_attribution_null_out();
  test_software_reset_writes_aircr();

  (void)fprintf(stderr, "[OK  ] test_ra_reset.c\n");
  return 0;
}
