/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_reset.c
 * @brief Unit tests for ra8_reset.c (Reset cause + software reset driver)
 *
 * @details
 * Exercises the reset HAL driver against the host-side ``ra8_fake_mmap``
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
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_reset.h"
#include "ra8_reset_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_reset_test_const_t
 * @brief Magic numbers used only inside this test TU.
 */
typedef enum : uint32_t {
  k_ra8_reset_test_aircr_zero    = 0UL,          /**< RA8 reset test aircr zero.    */
  k_ra8_reset_test_attribution_v = 0x0000001FUL, /**< RA8 reset test attribution v. */
  k_ra8_reset_test_clear_all_r0  = 0x000000FFUL, /**< RA8 reset test clear all r0.  */
  /** RA8 reset test clear swrf. */
  k_ra8_reset_test_clear_swrf = ((uint32_t)0x00000004UL << 8U),
  k_ra8_reset_test_clear_cwsf = 0x80000000UL, /**< Bit 31 triggers RSTSR2 write path. */
} ra8_reset_test_const_t;

/**
 * @brief Reset every state the driver caches between cases.
 *
 * @details
 * ``ra8_reset_init`` keeps a file-scope snapshot. We zero the fake mmap
 * (clears every register) **and** drop the driver's cached snapshot,
 * so each test starts with both hardware and driver state at zero.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_reset_test_only_reset_state();
}

/* ---------------------------------------------------------------------------
 * get_cause -- happy paths
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_cause_unknown_when_all_zero(void)
{
  TEST_BEGIN("reset get_cause unknown when all zero");
  prep();

  ra8_reset_cause_t cause = k_ra8_reset_cause_software;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_unknown, cause);

  TEST_END("reset get_cause unknown when all zero");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_porf(void)
{
  TEST_BEGIN("reset get_cause power-on");
  prep();

  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_porf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_power_on, cause);

  TEST_END("reset get_cause power-on");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_swrf(void)
{
  TEST_BEGIN("reset get_cause software");
  prep();

  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_swrf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_software, cause);

  TEST_END("reset get_cause software");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_iwdt(void)
{
  TEST_BEGIN("reset get_cause iwdt");
  prep();

  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_iwdtrf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_iwdt, cause);

  TEST_END("reset get_cause iwdt");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_temperature(void)
{
  TEST_BEGIN("reset get_cause temperature");
  prep();

  *ra8_reset_rstsr3() = (uint8_t)k_ra8_reset_rstsr3_temprf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_temperature, cause);

  TEST_END("reset get_cause temperature");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_warm_start(void)
{
  TEST_BEGIN("reset get_cause warm start");
  prep();

  *ra8_reset_rstsr2() = (uint8_t)k_ra8_reset_rstsr2_cwsf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_warm_start, cause);

  TEST_END("reset get_cause warm start");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_cause_priority_porf_over_iwdt(void)
{
  TEST_BEGIN("reset get_cause priority POR > IWDT");
  prep();

  /* Both flags latched simultaneously -- POR (in RSTSR0) must win. */
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_porf_msk;
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_iwdtrf_msk;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_power_on, cause);

  TEST_END("reset get_cause priority POR > IWDT");
}

/* ---------------------------------------------------------------------------
 * get_cause -- bad arg
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_cause_null_out(void)
{
  TEST_BEGIN("reset get_cause null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reset_get_cause(nullptr));

  TEST_END("reset get_cause null out");
}

/* ---------------------------------------------------------------------------
 * init / get_raw / snapshot caching
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_snapshot_survives_clear(void)
{
  TEST_BEGIN("reset init snapshot survives subsequent register clear");
  prep();

  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_swrf_msk;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_init());

  /* External code (e.g. another driver) clears the live register. */
  *ra8_reset_rstsr1() = 0U;

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  /* The snapshot still says SOFTWARE because init captured it. */
  TEST_ASSERT_EQ(k_ra8_reset_cause_software, cause);

  TEST_END("reset init snapshot survives subsequent register clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_raw_returns_register_words(void)
{
  TEST_BEGIN("reset get_raw returns register words");
  prep();

  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd1rf_msk;
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_wdt1rf_msk;
  *ra8_reset_rstsr2() = (uint8_t)k_ra8_reset_rstsr2_cwsf_msk;
  *ra8_reset_rstsr3() = (uint8_t)k_ra8_reset_rstsr3_cvmrf_msk;

  ra8_reset_raw_t raw = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_raw(&raw));

  TEST_ASSERT_EQ(k_ra8_reset_rstsr0_lvd1rf_msk, raw.rstsr0);
  TEST_ASSERT_EQ(k_ra8_reset_rstsr1_wdt1rf_msk, raw.rstsr1);
  TEST_ASSERT_EQ(k_ra8_reset_rstsr2_cwsf_msk, raw.rstsr2);
  TEST_ASSERT_EQ(k_ra8_reset_rstsr3_cvmrf_msk, raw.rstsr3);

  TEST_END("reset get_raw returns register words");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_raw_null_out(void)
{
  TEST_BEGIN("reset get_raw null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reset_get_raw(nullptr));

  TEST_END("reset get_raw null out");
}

/* ---------------------------------------------------------------------------
 * clear_cause
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_clear_cause_rstsr0(void)
{
  TEST_BEGIN("reset clear_cause clears RSTSR0 bits");
  prep();

  *ra8_reset_rstsr0() =
    (uint8_t)((uint8_t)k_ra8_reset_rstsr0_porf_msk | (uint8_t)k_ra8_reset_rstsr0_lvd1rf_msk);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_clear_cause((uint32_t)k_ra8_reset_test_clear_all_r0));

  TEST_ASSERT_EQ(0, *ra8_reset_rstsr0());

  TEST_END("reset clear_cause clears RSTSR0 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_cause_rstsr1_swrf(void)
{
  TEST_BEGIN("reset clear_cause clears RSTSR1.SWRF only");
  prep();

  *ra8_reset_rstsr1() =
    (uint32_t)k_ra8_reset_rstsr1_swrf_msk | (uint32_t)k_ra8_reset_rstsr1_iwdtrf_msk;

  /* Mask packs RSTSR1.SWRF (bit 2) at mask[10] -- helper enum value. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_clear_cause((uint32_t)k_ra8_reset_test_clear_swrf));

  /* SWRF cleared, IWDTRF preserved. */
  TEST_ASSERT_EQ(k_ra8_reset_rstsr1_iwdtrf_msk, *ra8_reset_rstsr1());

  TEST_END("reset clear_cause clears RSTSR1.SWRF only");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_cause_zero_mask_is_noop(void)
{
  TEST_BEGIN("reset clear_cause zero mask is a no-op");
  prep();

  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_porf_msk;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_clear_cause(0U));
  TEST_ASSERT_EQ(k_ra8_reset_rstsr0_porf_msk, *ra8_reset_rstsr0());

  TEST_END("reset clear_cause zero mask is a no-op");
}

/* ---------------------------------------------------------------------------
 * RSTSAR
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_attribution(void)
{
  TEST_BEGIN("reset get_attribution returns RSTSAR");
  prep();

  *ra8_reset_rstsar() = (uint32_t)k_ra8_reset_test_attribution_v;

  uint32_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_attribution(&value));
  TEST_ASSERT_EQ(k_ra8_reset_test_attribution_v, value);

  TEST_END("reset get_attribution returns RSTSAR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_attribution_null_out(void)
{
  TEST_BEGIN("reset get_attribution null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reset_get_attribution(nullptr));

  TEST_END("reset get_attribution null out");
}

/* ---------------------------------------------------------------------------
 * Software reset
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_software_reset_writes_aircr(void)
{
  TEST_BEGIN("reset software_reset writes AIRCR with VECTKEY+SYSRESETREQ");
  prep();

  *ra8_reset_aircr() = (uint32_t)k_ra8_reset_test_aircr_zero;

  /* On RA8_OFF_TARGET this returns instead of looping. */
  ra8_reset_software_reset();

  const uint32_t expected =
    ((uint32_t)k_ra8_reset_aircr_vectkey_value << (uint32_t)k_ra8_reset_aircr_vectkey_pos) |
    (uint32_t)k_ra8_reset_aircr_sysresetreq_msk;
  TEST_ASSERT_EQ(expected, *ra8_reset_aircr());

  TEST_END("reset software_reset writes AIRCR with VECTKEY+SYSRESETREQ");
}

/* ---------------------------------------------------------------------------
 * set_source_mask / get_source_mask (SYRSTMSK0/1/2)
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * The code under test has no compound (`&&` / `||`) decisions -- each
  * guard is a single condition (range validity, null pointer) and the
  * disable/enable path is a ternary. Branch coverage is achieved with
  * valid/invalid source, disable=true/false, and null/non-null vectors.
 */

static void test_set_source_mask_disable_sets_bit(void)
{
  TEST_BEGIN("reset set_source_mask disable sets SYRSTMSK0 bit");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_sw, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_sw_msk, *ra8_reset_syrstmsk0());
  /* PRCR.PRC5 left re-locked. */
  TEST_ASSERT_EQ(0U, (*ra8_reset_prcr() & (uint16_t)k_ra8_reset_prcr_prc5_msk));

  TEST_END("reset set_source_mask disable sets SYRSTMSK0 bit");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single-condition guards + a ternary in the
 * code under test; no `&&` or `||`)
 */
static void test_set_source_mask_enable_clears_bit(void)
{
  TEST_BEGIN("reset set_source_mask enable clears SYRSTMSK0 bit");
  prep();

  *ra8_reset_syrstmsk0() =
    (uint8_t)((uint8_t)k_ra8_reset_syrstmsk0_sw_msk | (uint8_t)k_ra8_reset_syrstmsk0_bus_msk);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_sw, false));
  /* SW cleared, BUS preserved. */
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_bus_msk, *ra8_reset_syrstmsk0());

  TEST_END("reset set_source_mask enable clears SYRSTMSK0 bit");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single-condition guards + a ternary in the
 * code under test; no `&&` or `||`)
 */
static void test_set_source_mask_routes_to_syrstmsk1_and_2(void)
{
  TEST_BEGIN("reset set_source_mask routes WDT1->MSK1, PVD1->MSK2");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_wdt1, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk1_wdt1_msk, *ra8_reset_syrstmsk1());
  TEST_ASSERT_EQ(0, *ra8_reset_syrstmsk0());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_pvd1, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk2_pvd1_msk, *ra8_reset_syrstmsk2());

  TEST_END("reset set_source_mask routes WDT1->MSK1, PVD1->MSK2");
}

/**
 * @par MC/DC:
 * (no compound decisions -- single range-validity guard in the code
 * under test; no `&&` or `||`)
 */
static void test_set_source_mask_invalid_source_rejected(void)
{
  TEST_BEGIN("reset set_source_mask rejects out-of-range source");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_reset_set_source_mask(k_ra8_reset_source_count, true));
  /* No register write happened. */
  TEST_ASSERT_EQ(0, *ra8_reset_syrstmsk0());

  TEST_END("reset set_source_mask rejects out-of-range source");
}

/**
 * @par MC/DC:
 * (no compound decisions -- null-check + single range-validity guard
 * in the code under test; no `&&` or `||`)
 */
static void test_get_source_mask_reads_state(void)
{
  TEST_BEGIN("reset get_source_mask reads SYRSTMSKn bit");
  prep();

  bool disabled = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_source_mask(k_ra8_reset_source_iwdt, &disabled));
  TEST_ASSERT_EQ(false, disabled);

  *ra8_reset_syrstmsk0() = (uint8_t)k_ra8_reset_syrstmsk0_iwdt_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_source_mask(k_ra8_reset_source_iwdt, &disabled));
  TEST_ASSERT_EQ(true, disabled);

  TEST_END("reset get_source_mask reads SYRSTMSKn bit");
}

/**
 * @par MC/DC:
 * (no compound decisions -- null-check guard only in the code under
 * test; no `&&` or `||`)
 */
static void test_get_source_mask_null_rejected(void)
{
  TEST_BEGIN("reset get_source_mask rejects null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reset_get_source_mask(k_ra8_reset_source_iwdt, nullptr));

  TEST_END("reset get_source_mask rejects null out");
}

/**
 * @par MC/DC:
 * (no compound decisions -- null-check + single range-validity guard
 * in the code under test; no `&&` or `||`)
 */
static void test_get_source_mask_invalid_source_rejected(void)
{
  TEST_BEGIN("reset get_source_mask rejects out-of-range source");
  prep();

  bool disabled = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reset_get_source_mask(k_ra8_reset_source_count, &disabled));

  TEST_END("reset get_source_mask rejects out-of-range source");
}

/* ---------------------------------------------------------------------------
 * Decode paths -- RSTSR0 remaining flags (lines 152,155,158,161,164,167)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Drive each uncovered RSTSR0 flag through the decode chain.
 *
 * @details
 * Sets one RSTSR0 flag at a time (PORF clear so the chain falls through
 * to the target return). Exercises internal_decode_rstsr0 lines
 * 152 (lvd0), 155 (lvd1), 158 (lvd2), 161 (lvd4), 164 (lvd5), 167 (dpsrstf).
 *
 * @par MC/DC:
 * Single-condition guards in internal_decode_rstsr0; no && or ||.
 * One case per guard covers all uncovered return paths.
 */
static void test_get_cause_rstsr0_remaining_flags(void)
{
  TEST_BEGIN("reset get_cause RSTSR0 remaining flags");
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd0rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lvd0, cause);

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd1rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lvd1, cause);

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd2rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lvd2, cause);

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd4rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lvd4, cause);

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd5rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lvd5, cause);

  prep();
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_dpsrstf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_deep_sw_standby, cause);

  TEST_END("reset get_cause RSTSR0 remaining flags");
}

/* ---------------------------------------------------------------------------
 * Decode paths -- RSTSR1 remaining flags (lines 199,205,208,211,214,217,220,223,226)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Drive each uncovered RSTSR1 flag through the decode chain.
 *
 * @details
 * RSTSR0 and RSTSR3 remain zero (from prep) so the priority in
 * internal_decode falls through to RSTSR1. Each sub-test seeds exactly
 * one flag, covering: wdt0 (199), lockup0 (205), local_memory0 (208),
 * bus_peripheral_mpu (211), common_memory (214), wdt1 (217), lockup1 (220),
 * local_memory1 (223), network (226).
 *
 * @par MC/DC:
 * Single-condition guards in internal_decode_rstsr1; no && or ||.
 */
static void test_get_cause_rstsr1_remaining_flags(void)
{
  TEST_BEGIN("reset get_cause RSTSR1 remaining flags");
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_wdtrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_wdt0, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_clurf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lockup0, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_lm0rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_local_memory0, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_bussrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_bus_peripheral_mpu, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_cmrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_common_memory, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_wdt1rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_wdt1, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_clu1rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_lockup1, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_lm1rf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_local_memory1, cause);

  prep();
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_nwrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_network, cause);

  TEST_END("reset get_cause RSTSR1 remaining flags");
}

/* ---------------------------------------------------------------------------
 * Decode paths -- RSTSR3 remaining flags (lines 255, 258)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Drive the uncovered RSTSR3 flags (core voltage and overcurrent).
 *
 * @details
 * RSTSR0 stays clear (from prep) so the priority chain reaches RSTSR3.
 * Covers internal_decode_rstsr3 line 255 (cvmrf) and line 258 (ocprf).
 * TEMPRF (line 260) is already covered by test_get_cause_temperature.
 *
 * @par MC/DC:
 * Single-condition guards in internal_decode_rstsr3; no && or ||.
 */
static void test_get_cause_rstsr3_remaining_flags(void)
{
  TEST_BEGIN("reset get_cause RSTSR3 remaining flags");
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;

  prep();
  *ra8_reset_rstsr3() = (uint8_t)k_ra8_reset_rstsr3_cvmrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_core_voltage, cause);

  prep();
  *ra8_reset_rstsr3() = (uint8_t)k_ra8_reset_rstsr3_ocprf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_overcurrent, cause);

  TEST_END("reset get_cause RSTSR3 remaining flags");
}

/* ---------------------------------------------------------------------------
 * get_raw -- cached path (lines 365-366)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify get_raw returns the cached snapshot when init ran first.
 *
 * @details
 * When ra8_reset_init has been called the driver caches RSTSR0/1/2/3.
 * ra8_reset_get_raw must return the *snapshot*, not the live register value.
 * This exercises the cached branch at lines 365-366 of ra8_reset.c.
 *
 * @par MC/DC:
 * Decision: s_state.initialized (single condition). This case makes it
 * true; existing test_get_raw_returns_register_words keeps it false.
 * Together they provide N=1 -> N+1=2 MC/DC vectors.
 */
static void test_get_raw_cached_path(void)
{
  TEST_BEGIN("reset get_raw returns cached snapshot");
  prep();

  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_lvd2rf_msk;
  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_swrf_msk;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_init());

  /* Wipe live registers -- cached path must not re-read hardware. */
  *ra8_reset_rstsr0() = 0U;
  *ra8_reset_rstsr1() = 0U;

  ra8_reset_raw_t raw = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_raw(&raw));

  /* Snapshot values, not the post-clear zeros. */
  TEST_ASSERT_EQ(k_ra8_reset_rstsr0_lvd2rf_msk, raw.rstsr0);
  TEST_ASSERT_EQ(k_ra8_reset_rstsr1_swrf_msk, raw.rstsr1);

  TEST_END("reset get_raw returns cached snapshot");
}

/* ---------------------------------------------------------------------------
 * clear_cause -- RSTSR2 write path (lines 396-397) and snapshot refresh
 * (lines 402-404)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify that bit 31 in the mask triggers the RSTSR2.CWSF write.
 *
 * @details
 * ra8_reset_clear_cause bit 31 (k_ra8_reset_test_clear_cwsf) maps to
 * k_ra8_reset_mask_rstsr2_cwsf inside ra8_reset.c.  When set, the driver
 * writes 1 to RSTSR2 (HUM Ch 6.2.4 Note 2, p 261: CWSF is set by
 * writing 1).  Exercises lines 396-397.
 *
 * @par MC/DC:
 * Decision: (mask & k_ra8_reset_mask_rstsr2_cwsf) != 0U (single condition).
 * True path exercised here; false path exercised by every other clear test.
 */
static void test_clear_cause_rstsr2_cwsf_path(void)
{
  TEST_BEGIN("reset clear_cause RSTSR2 CWSF write path");
  prep();

  /* Bit 31 in the mask selects the RSTSR2 write branch. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_clear_cause((uint32_t)k_ra8_reset_test_clear_cwsf));

  /* CWSF is set (not cleared) by writing 1 to RSTSR2 -- per the Resets note. */
  TEST_ASSERT_EQ(k_ra8_reset_rstsr2_cwsf_msk, *ra8_reset_rstsr2());

  TEST_END("reset clear_cause RSTSR2 CWSF write path");
}

/**
 * @brief Verify that clear_cause refreshes the cached snapshot.
 *
 * @details
 * When ra8_reset_init has already been called, ra8_reset_clear_cause must
 * re-read and re-decode the registers into the cached snapshot so that
 * the next ra8_reset_get_cause call sees the post-clear state.
 * Exercises lines 402-404 of ra8_reset.c.
 *
 * @par MC/DC:
 * Decision: s_state.initialized in clear_cause (single condition).
 * True path exercised here (init called before clear);
 * false path exercised by test_clear_cause_rstsr0 (no init call).
 */
static void test_clear_cause_refreshes_cached_snapshot(void)
{
  TEST_BEGIN("reset clear_cause refreshes cached snapshot when initialized");
  prep();

  *ra8_reset_rstsr1() = (uint32_t)k_ra8_reset_rstsr1_swrf_msk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_init());

  /* Confirm the snapshot sees SOFTWARE before the clear. */
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_software, cause);

  /* Clear SWRF; the driver must re-read and re-decode into s_state. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_clear_cause((uint32_t)k_ra8_reset_test_clear_swrf));

  /* After the clear the live register is 0, so decoded cause is unknown. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_get_cause(&cause));
  TEST_ASSERT_EQ(k_ra8_reset_cause_unknown, cause);

  TEST_END("reset clear_cause refreshes cached snapshot when initialized");
}

/* ---------------------------------------------------------------------------
 * internal_source_loc remaining switch cases
 * (lines 484-486, 492-518, 524-526)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Cover the WDT0 case in internal_source_loc (lines 484-486).
 *
 * @details
 * The existing tests exercise sw, wdt1, pvd1, and iwdt (via get_source_mask).
 * This case exercises k_ra8_reset_source_wdt0 which routes to SYRSTMSK0.
 *
 * @par MC/DC:
 * (switch dispatch; single case per test; no compound decisions)
 */
static void test_set_source_mask_syrstmsk0_wdt0(void)
{
  TEST_BEGIN("reset set_source_mask WDT0 routes to SYRSTMSK0");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_wdt0, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_wdt0_msk, *ra8_reset_syrstmsk0());

  TEST_END("reset set_source_mask WDT0 routes to SYRSTMSK0");
}

/**
 * @brief Cover remaining SYRSTMSK0 cases in internal_source_loc (lines 492-506).
 *
 * @details
 * Exercises k_ra8_reset_source_clu0, k_ra8_reset_source_lm0,
 * k_ra8_reset_source_cm, and k_ra8_reset_source_bus -- all four route to
 * SYRSTMSK0 with different bit masks. Each sub-test uses the disable path
 * (disable=true) so the mask bit lands in the register.
 *
 * @par MC/DC:
 * (switch dispatch; single case per sub-test; no compound decisions)
 */
static void test_set_source_mask_syrstmsk0_remaining(void)
{
  TEST_BEGIN("reset set_source_mask SYRSTMSK0 remaining sources");

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_clu0, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_clu0_msk, *ra8_reset_syrstmsk0());

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_lm0, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_lm0_msk, *ra8_reset_syrstmsk0());

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_cm, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_cm_msk, *ra8_reset_syrstmsk0());

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_bus, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk0_bus_msk, *ra8_reset_syrstmsk0());

  TEST_END("reset set_source_mask SYRSTMSK0 remaining sources");
}

/**
 * @brief Cover SYRSTMSK1 clu1/lm1 and SYRSTMSK2 pvd2 cases (lines 511-526).
 *
 * @details
 * Exercises k_ra8_reset_source_clu1 and k_ra8_reset_source_lm1 (both route
 * to SYRSTMSK1) and k_ra8_reset_source_pvd2 (routes to SYRSTMSK2).
 * Also re-exercises wdt1 to ensure lines 507-510 are explicitly driven.
 *
 * @par MC/DC:
 * (switch dispatch; single case per sub-test; no compound decisions)
 */
static void test_set_source_mask_syrstmsk1_and_2_remaining(void)
{
  TEST_BEGIN("reset set_source_mask SYRSTMSK1 clu1/lm1 and SYRSTMSK2 pvd2");

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_clu1, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk1_clu1_msk, *ra8_reset_syrstmsk1());

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_lm1, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk1_lm1_msk, *ra8_reset_syrstmsk1());

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reset_set_source_mask(k_ra8_reset_source_pvd2, true));
  TEST_ASSERT_EQ(k_ra8_reset_syrstmsk2_pvd2_msk, *ra8_reset_syrstmsk2());

  TEST_END("reset set_source_mask SYRSTMSK1 clu1/lm1 and SYRSTMSK2 pvd2");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_get_cause_unknown_when_all_zero,
  test_get_cause_porf,
  test_get_cause_swrf,
  test_get_cause_iwdt,
  test_get_cause_temperature,
  test_get_cause_warm_start,
  test_get_cause_priority_porf_over_iwdt,
  test_get_cause_null_out,
  test_init_snapshot_survives_clear,
  test_get_raw_returns_register_words,
  test_get_raw_null_out,
  test_clear_cause_rstsr0,
  test_clear_cause_rstsr1_swrf,
  test_clear_cause_zero_mask_is_noop,
  test_get_attribution,
  test_get_attribution_null_out,
  test_software_reset_writes_aircr,
  test_set_source_mask_disable_sets_bit,
  test_set_source_mask_enable_clears_bit,
  test_set_source_mask_routes_to_syrstmsk1_and_2,
  test_set_source_mask_invalid_source_rejected,
  test_get_source_mask_reads_state,
  test_get_source_mask_null_rejected,
  test_get_source_mask_invalid_source_rejected,
  test_get_cause_rstsr0_remaining_flags,
  test_get_cause_rstsr1_remaining_flags,
  test_get_cause_rstsr3_remaining_flags,
  test_get_raw_cached_path,
  test_clear_cause_rstsr2_cwsf_path,
  test_clear_cause_refreshes_cached_snapshot,
  test_set_source_mask_syrstmsk0_wdt0,
  test_set_source_mask_syrstmsk0_remaining,
  test_set_source_mask_syrstmsk1_and_2_remaining,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_reset.c\n");
  return 0;
}
