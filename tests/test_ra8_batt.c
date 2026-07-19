/**
 * @file test_ra8_batt.c
 * @brief Unit tests for the ra8_batt low-battery nag policy.
 *
 * @details
 * Pure host tests -- ra8_batt is allocation-free policy logic. Covers the
 * edge-triggered low / critical warnings, hysteresis re-arm on rise, re-arm
 * while charging, the SOC clamp, the nag label map, and null guards, plus
 * MC/DC vector sets for the four compound decisions in ra8_batt_update (the two
 * re-arm ORs and the two raise ANDs).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_batt.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum batt_fixture_t
 * @brief The physical quantities the configuration declares.
 */
typedef enum : uint8_t {
  k_batt_mv_below_critical = 10U, /**< Below the critical threshold: raises the critical alarm. */
  k_batt_mv_above_critical = 12U, /**< Back above it, but still inside the hysteresis band.     */
  k_batt_mv_clear_critical = 14U, /**< Far enough above to clear the critical alarm outright.   */
  k_batt_mv_below_low      = 20U, /**< Below the low threshold: raises the low alarm.           */
  k_batt_mv_above_low      = 22U, /**< Back above it, inside the hysteresis band.               */
  k_batt_mv_clear_low      = 24U, /**< Far enough above to clear the low alarm.                 */
} batt_fixture_t;

/** @brief Update the monitor and return just the nag (test convenience). */
static ra8_batt_nag_t upd(ra8_batt_monitor_t* mon, uint8_t soc, bool charging)
{
  ra8_batt_nag_t nag = k_ra8_batt_nag_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_update(mon, soc, charging, &nag));
  return nag;
}

/**
 * @test test_batt_basic_edges
 * Each band warns once on the descent into it and stays quiet below.
 */
static void test_batt_basic_edges(void)
{
  TEST_BEGIN("ra8_batt basic edges");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 72U, false));     /* healthy: quiet        */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false));      /* enter low: warn       */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 18U, false));     /* still low: no re-nag  */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 10U, false)); /* enter critical: warn  */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 8U, false));      /* still critical: quiet */
  TEST_END("ra8_batt basic edges");
}

/**
 * @test test_batt_rearm_on_rise
 * A band re-warns only after SOC recovers past its hysteresis margin.
 */
static void test_batt_rearm_on_rise(void)
{
  TEST_BEGIN("ra8_batt re-arm on rise");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false));  /* warn low                */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 22U, false)); /* below margin: armed off */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 20U, false)); /* no re-nag yet           */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 24U, false)); /* past margin: re-arm     */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false));  /* warns again             */
  TEST_END("ra8_batt re-arm on rise");
}

/**
 * @test test_batt_charging
 * Charging suppresses warnings and re-arms the bands for the next unplug.
 */
static void test_batt_charging(void)
{
  TEST_BEGIN("ra8_batt charging");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false));     /* warn low            */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 5U, true));      /* charging: no warn   */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 5U, false)); /* unplug low: re-warn */
  TEST_END("ra8_batt charging");
}

/**
 * @test test_batt_clamp_and_str
 * Out-of-range SOC is clamped; the nag label map covers every value.
 */
static void test_batt_clamp_and_str(void)
{
  TEST_BEGIN("ra8_batt clamp + str");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 200U, false)); /* clamp 200 -> 100 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 0U, false)); /* empty -> critical */
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_none), "OK") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_low), "LOW") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_critical), "CRITICAL") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str((ra8_batt_nag_t)200U), "?") == 0);
  TEST_END("ra8_batt clamp + str");
}

/**
 * @test test_batt_nullguards
 * Both entry points reject null pointers without writing through them.
 */
static void test_batt_nullguards(void)
{
  TEST_BEGIN("ra8_batt null guards");
  ra8_batt_monitor_t m;
  ra8_batt_nag_t     nag = k_ra8_batt_nag_none;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_batt_monitor_init(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_batt_update(nullptr, 5U, false, &nag));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_batt_update(&m, 5U, false, nullptr));
  TEST_END("ra8_batt null guards");
}

/**
 * @test test_mcdc_raise_low
 *
 * @par MC/DC:
 * Decision in `ra8_batt_update()`:
 * `(soc <= k_ra8_batt_low_pct) && !mon->low_raised`  (C1=soc<=20, C2=!low_raised)
 * - V1 soc=20, fresh        -> C1=T, C2=T -> LOW   (baseline, both true).
 * - V2 soc=21, fresh        -> C1=F, C2=T -> none  (C1 flips the outcome).
 * - V3 soc=20, low already   -> C1=T, C2=F -> none  (C2 flips the outcome).
 * V1+V2 prove C1 independent; V1+V3 prove C2 independent. N+1 = 3 vectors.
 */
static void test_mcdc_raise_low(void)
{
  TEST_BEGIN("ra8_batt raise-low MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false)); /* V1 T,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 21U, false)); /* V2 F,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_low, false);                /* raise -> low_raised=true */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 20U, false)); /* V3 T,F                   */
  TEST_END("ra8_batt raise-low MC/DC");
}

/**
 * @test test_mcdc_raise_critical
 *
 * @par MC/DC:
 * Decision: `(soc <= k_ra8_batt_critical_pct) && !mon->critical_raised`
 * (C1=soc<=10, C2=!critical_raised)
 * - V1 soc=10, fresh         -> C1=T, C2=T -> CRITICAL (baseline).
 * - V2 soc=11, fresh         -> C1=F, C2=T -> LOW (not critical; C1 flips).
 * - V3 soc=10, crit already  -> C1=T, C2=F -> none (C2 flips).
 */
static void test_mcdc_raise_critical(void)
{
  TEST_BEGIN("ra8_batt raise-critical MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 10U, false)); /* V1 T,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 11U, false)); /* V2 F,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_critical, false);           /* raise -> critical_raised=true */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 10U, false)); /* V3 T,F                        */
  TEST_END("ra8_batt raise-critical MC/DC");
}

/**
 * @test test_mcdc_rearm_low
 *
 * @par MC/DC:
 * Decision: `charging || (soc > k_ra8_batt_low_pct + k_ra8_batt_rearm_margin)`
 * (C1=charging, C2=soc>23). Observed by whether low can warn again afterward.
 * Each vector seeds a low warning first, applies the re-arm step, then probes
 * at soc=20.
 * - V1 charge@22 (C1=T, C2=F) -> re-arm  -> probe warns LOW.
 * - V2 rise@24  (C1=F, C2=T)  -> re-arm  -> probe warns LOW.
 * - V3 dip@22   (C1=F, C2=F)  -> no re-arm -> probe stays quiet.
 * V1+V3 prove C1 independent; V2+V3 prove C2 independent.
 */
static void test_mcdc_rearm_low(void)
{
  TEST_BEGIN("ra8_batt re-arm-low MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_low, false);               /* seed low               */
  (void)upd(&m, k_batt_mv_above_low, true);                /* V1 C1=T,C2=F -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_low, false);               /* seed low               */
  (void)upd(&m, k_batt_mv_clear_low, false);               /* V2 C1=F,C2=T -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, upd(&m, 20U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_low, false);                /* seed low                  */
  (void)upd(&m, k_batt_mv_above_low, false);                /* V3 C1=F,C2=F -> no re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 20U, false)); /* probe quiet               */
  TEST_END("ra8_batt re-arm-low MC/DC");
}

/**
 * @test test_mcdc_rearm_critical
 *
 * @par MC/DC:
 * Decision: `charging || (soc > k_ra8_batt_critical_pct + k_ra8_batt_rearm_margin)`
 * (C1=charging, C2=soc>13). Observed by whether critical can warn again.
 * - V1 charge@12 (C1=T, C2=F) -> re-arm  -> probe warns CRITICAL.
 * - V2 rise@14  (C1=F, C2=T)  -> re-arm  -> probe warns CRITICAL.
 * - V3 dip@12   (C1=F, C2=F)  -> no re-arm -> probe stays quiet.
 */
static void test_mcdc_rearm_critical(void)
{
  TEST_BEGIN("ra8_batt re-arm-critical MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_critical, false);               /* seed critical          */
  (void)upd(&m, k_batt_mv_above_critical, true);                /* V1 C1=T,C2=F -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 10U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_critical, false);               /* seed critical          */
  (void)upd(&m, k_batt_mv_clear_critical, false);               /* V2 C1=F,C2=T -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, upd(&m, 10U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)upd(&m, k_batt_mv_below_critical, false);           /* seed critical             */
  (void)upd(&m, k_batt_mv_above_critical, false);           /* V3 C1=F,C2=F -> no re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, upd(&m, 10U, false)); /* probe quiet               */
  TEST_END("ra8_batt re-arm-critical MC/DC");
}

int main(void)
{
  test_batt_basic_edges();
  test_batt_rearm_on_rise();
  test_batt_charging();
  test_batt_clamp_and_str();
  test_batt_nullguards();
  test_mcdc_raise_low();
  test_mcdc_raise_critical();
  test_mcdc_rearm_low();
  test_mcdc_rearm_critical();
  return 0;
}
