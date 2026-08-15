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

#include "ra8_attributes.h"
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

/**
 * @brief Update the monitor and return only the resulting nag.
 * @details Wraps the public status return in an assertion so vectors can state
 * their expected notification directly.
 * @param[in,out] mon Battery monitor under test.
 * @param[in] soc Reported state of charge percentage.
 * @param[in] charging Whether external charging power is present.
 * @return Notification selected by the update.
 * @retval k_ra8_batt_nag_none No new warning edge was raised.
 * @pre `mon` points to an initialized monitor.
 * @pre The Unity-style assertion sink is available.
 * @post The public update returned `k_ra8_ok`.
 * @post Monitor hysteresis state reflects the supplied sample.
 * @note Test-only convenience wrapper; it does not hide update failures.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_batt_nag_t
internal_update(ra8_batt_monitor_t* mon, uint8_t soc, bool charging)
{
  ra8_batt_nag_t nag = k_ra8_batt_nag_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_update(mon, soc, charging, &nag));
  return nag;
}

/**
 * @test internal_test_batt_basic_edges
 * @brief Verify each descending threshold warns once.
 * @details Drives a healthy-to-critical sequence and observes edge-triggered
 * low and critical notifications with quiet repeated samples.
 * @pre A fresh monitor can be initialized.
 * @pre The configured low threshold exceeds the critical threshold.
 * @post Low and critical each appear on their first descending edge.
 * @post Repeated samples below each threshold produce no duplicate warning.
 * @note The detailed vectors also contribute compound-decision coverage.
 * @since 0.1.0
 * Each band warns once on the descent into it and stays quiet below.
 *
 * @par MC/DC:
 * Crosses the two raise ANDs `(soc <= threshold) && !raised` and the two re-arm
 * ORs `charging || (soc > rearm)` in `ra8_batt_update()` across a descending SOC
 * sweep (charging held false): soc 72 -> 20 -> 18 -> 10 -> 8. It observes the
 * raise-low AND flip (warn at 20 with both conditions true, quiet at 18 once
 * !low_raised is false) and the raise-critical AND flip (critical at 10, quiet at
 * 8). The minimal N+1 independence vector sets -- including the charging arm of
 * each re-arm OR, never true here -- are carried by the dedicated siblings
 * internal_test_mcdc_raise_low, internal_test_mcdc_raise_critical, internal_test_mcdc_rearm_low and
 * internal_test_mcdc_rearm_critical.
 */
RA8_INTERNAL static void internal_test_batt_basic_edges(void)
{
  TEST_BEGIN("ra8_batt basic edges");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 72U, false)); /* healthy: quiet       */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false));  /* enter low: warn      */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 18U, false)); /* still low: no re-nag */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical,
                 internal_update(&m, 10U, false));                     /* enter critical: warn  */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 8U, false)); /* still critical: quiet */
  TEST_END("ra8_batt basic edges");
}

/**
 * @test internal_test_batt_rearm_on_rise
 * @brief Verify recovery beyond hysteresis re-arms the low warning.
 * @details Compares an in-band rise with a rise beyond the re-arm margin.
 * @pre A fresh monitor can be initialized.
 * @pre Fixture values straddle the configured low re-arm boundary.
 * @post An in-band rise does not permit another low notification.
 * @post A rise beyond the margin permits the next low edge to warn.
 * @note Charging is held false to isolate the SOC recovery arm.
 * @since 0.1.0
 * A band re-warns only after SOC recovers past its hysteresis margin.
 *
 * @par MC/DC:
 * Crosses the re-arm-low OR `charging || (soc > k_ra8_batt_low_pct +
 * k_ra8_batt_rearm_margin)` in `ra8_batt_update()` with charging held false, so
 * the SOC-recovery condition is exercised both false (soc=22, below the margin,
 * no re-arm) and true (soc=24, past the margin, re-arm), observed by whether low
 * can warn again. The charging arm's independent vector and the full N+1 sets are
 * carried by the siblings internal_test_mcdc_rearm_low and internal_test_mcdc_raise_low.
 */
RA8_INTERNAL static void internal_test_batt_rearm_on_rise(void)
{
  TEST_BEGIN("ra8_batt re-arm on rise");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false)); /* warn low */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none,
                 internal_update(&m, 22U, false)); /* below margin: armed off */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 20U, false)); /* no re-nag yet */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 24U, false)); /* past margin: re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false));  /* warns again         */
  TEST_END("ra8_batt re-arm on rise");
}

/**
 * @test internal_test_batt_charging
 * @brief Verify charging suppresses and re-arms battery warnings.
 * @details Raises low, applies a charging sample at critical SOC, then unplugs.
 * @pre A fresh monitor can be initialized.
 * @pre The charging flag is the only state changed between the final samples.
 * @post The charging sample emits no warning and re-arms both bands.
 * @post The following unplugged critical sample raises a critical warning.
 * @note Isolates the charging arm of both re-arm decisions.
 * @since 0.1.0
 * Charging suppresses warnings and re-arms the bands for the next unplug.
 *
 * @par MC/DC:
 * Crosses both re-arm ORs `charging || (soc > rearm)` in `ra8_batt_update()` with
 * the charging arm true (soc=5, charging=true -> both bands re-arm and the
 * !charging raise block is skipped), then unplugs (charging=false) to re-warn.
 * This supplies the charging-true arm the descending-SOC tests cannot; the full
 * N+1 independence sets are carried by the siblings internal_test_mcdc_rearm_low,
 * internal_test_mcdc_rearm_critical, internal_test_mcdc_raise_low and internal_test_mcdc_raise_critical.
 */
RA8_INTERNAL static void internal_test_batt_charging(void)
{
  TEST_BEGIN("ra8_batt charging");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false));     /* warn low            */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 5U, true));      /* charging: no warn   */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, internal_update(&m, 5U, false)); /* unplug low: re-warn */
  TEST_END("ra8_batt charging");
}

/**
 * @test internal_test_batt_clamp_and_str
 * @brief Verify SOC clamping and every notification label.
 * @details Supplies over-range and empty SOC values, then checks the complete
 * public string mapping including an unknown enumerator.
 * @pre A fresh monitor can be initialized for each clamp arm.
 * @pre Published notification enumerators retain their documented labels.
 * @post SOC above 100 is treated as healthy and zero raises critical.
 * @post Every known label and the unknown fallback match exactly.
 * @note String checks pin user-visible diagnostic spelling.
 * @since 0.1.0
 * Out-of-range SOC is clamped; the nag label map covers every value.
 *
 * @par MC/DC:
 * The SOC clamp is a single-condition ternary and `ra8_batt_nag_str` is a switch
 * (neither is a compound boolean). The compound decisions crossed are the raise
 * ANDs `(soc <= threshold) && !raised` in `ra8_batt_update()`, exercised at the
 * clamped extremes (soc 200->100 stays quiet, soc 0 raises both low and
 * critical). The N+1 independence vector sets are carried by the siblings
 * internal_test_mcdc_raise_low and internal_test_mcdc_raise_critical.
 */
RA8_INTERNAL static void internal_test_batt_clamp_and_str(void)
{
  TEST_BEGIN("ra8_batt clamp + str");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 200U, false)); /* clamp 200 -> 100 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, internal_update(&m, 0U, false)); /* empty -> critical */
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_none), "OK") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_low), "LOW") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str(k_ra8_batt_nag_critical), "CRITICAL") == 0);
  TEST_ASSERT(strcmp(ra8_batt_nag_str((ra8_batt_nag_t)200U), "?") == 0);
  TEST_END("ra8_batt clamp + str");
}

/**
 * @test internal_test_batt_nullguards
 * @brief Verify monitor and output pointer null guards.
 * @details Exercises initialization and update rejection without dereferencing
 * absent caller storage.
 * @pre The public battery policy implementation is linked.
 * @pre A valid monitor is available for the null-output arm.
 * @post Each invalid call returns `k_ra8_err_null_ptr`.
 * @post The valid monitor remains usable after the rejected call.
 * @note Covers both public entry points' trust boundaries.
 * @since 0.1.0
 * Both entry points reject null pointers without writing through them.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- both calls return at the
 * RA8_CHECK_NULL_PTR guard, before the `(soc <= low) && !low_raised` raise
 * decision; no `&&` or `||` in the code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_batt_nullguards(void)
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
 * @test internal_test_mcdc_raise_low
 * @brief Prove both conditions independently control the low-warning decision.
 * @details Uses the minimal three-vector set for threshold and armed state.
 * @pre Each vector starts from explicitly initialized monitor state.
 * @pre Fixture values lie on both sides of the low threshold.
 * @post The baseline vector raises low.
 * @post Flipping either condition independently suppresses that outcome.
 * @note Supplies the documented N+1 MC/DC set.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision in `ra8_batt_update()`:
 * `(soc <= k_ra8_batt_low_pct) && !mon->low_raised`  (C1=soc<=20, C2=!low_raised)
 * - V1 soc=20, fresh        -> C1=T, C2=T -> LOW   (baseline, both true).
 * - V2 soc=21, fresh        -> C1=F, C2=T -> none  (C1 flips the outcome).
 * - V3 soc=20, low already   -> C1=T, C2=F -> none  (C2 flips the outcome).
 * V1+V2 prove C1 independent; V1+V3 prove C2 independent. N+1 = 3 vectors.
 */
RA8_INTERNAL static void internal_test_mcdc_raise_low(void)
{
  TEST_BEGIN("ra8_batt raise-low MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false)); /* V1 T,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 21U, false)); /* V2 F,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_low, false); /* raise -> low_raised=true */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 20U, false)); /* V3 T,F */
  TEST_END("ra8_batt raise-low MC/DC");
}

/**
 * @test internal_test_mcdc_raise_critical
 * @brief Prove both conditions independently control the critical decision.
 * @details Uses threshold and already-raised arms around the critical boundary.
 * @pre Each vector starts from explicitly initialized monitor state.
 * @pre Fixture values lie on both sides of the critical threshold.
 * @post The baseline vector raises critical.
 * @post Flipping either condition independently suppresses critical.
 * @note A low result still demonstrates the critical branch was not taken.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `(soc <= k_ra8_batt_critical_pct) && !mon->critical_raised`
 * (C1=soc<=10, C2=!critical_raised)
 * - V1 soc=10, fresh         -> C1=T, C2=T -> CRITICAL (baseline).
 * - V2 soc=11, fresh         -> C1=F, C2=T -> LOW (not critical; C1 flips).
 * - V3 soc=10, crit already  -> C1=T, C2=F -> none (C2 flips).
 */
RA8_INTERNAL static void internal_test_mcdc_raise_critical(void)
{
  TEST_BEGIN("ra8_batt raise-critical MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, internal_update(&m, 10U, false)); /* V1 T,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 11U, false)); /* V2 F,T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_critical, false); /* raise -> critical_raised=true */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 10U, false)); /* V3 T,F */
  TEST_END("ra8_batt raise-critical MC/DC");
}

/**
 * @test internal_test_mcdc_rearm_low
 * @brief Prove both conditions independently control low-band re-arming.
 * @details Seeds low, applies each OR-vector, then probes for a renewed warning.
 * @pre Every vector initializes and seeds the low-raised state.
 * @pre Fixture values bracket the low re-arm margin.
 * @post Either charging or sufficient recovery re-arms low.
 * @post With both conditions false, the probe remains quiet.
 * @note Uses outcome observability rather than private-state inspection.
 * @since 0.1.0
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
RA8_INTERNAL static void internal_test_mcdc_rearm_low(void)
{
  TEST_BEGIN("ra8_batt re-arm-low MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_low, false);               /* seed low               */
  (void)internal_update(&m, k_batt_mv_above_low, true);                /* V1 C1=T,C2=F -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_low, false);               /* seed low               */
  (void)internal_update(&m, k_batt_mv_clear_low, false);               /* V2 C1=F,C2=T -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_low, internal_update(&m, 20U, false)); /* probe warns            */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_low, false); /* seed low                  */
  (void)internal_update(&m, k_batt_mv_above_low, false); /* V3 C1=F,C2=F -> no re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 20U, false)); /* probe quiet */
  TEST_END("ra8_batt re-arm-low MC/DC");
}

/**
 * @test internal_test_mcdc_rearm_critical
 * @brief Prove both conditions independently control critical re-arming.
 * @details Seeds critical, applies each OR-vector, then probes for rewarning.
 * @pre Every vector initializes and seeds the critical-raised state.
 * @pre Fixture values bracket the critical re-arm margin.
 * @post Either charging or sufficient recovery re-arms critical.
 * @post With both conditions false, the probe remains quiet.
 * @note Completes the critical re-arm MC/DC pair set.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `charging || (soc > k_ra8_batt_critical_pct + k_ra8_batt_rearm_margin)`
 * (C1=charging, C2=soc>13). Observed by whether critical can warn again.
 * - V1 charge@12 (C1=T, C2=F) -> re-arm  -> probe warns CRITICAL.
 * - V2 rise@14  (C1=F, C2=T)  -> re-arm  -> probe warns CRITICAL.
 * - V3 dip@12   (C1=F, C2=F)  -> no re-arm -> probe stays quiet.
 */
RA8_INTERNAL static void internal_test_mcdc_rearm_critical(void)
{
  TEST_BEGIN("ra8_batt re-arm-critical MC/DC");
  ra8_batt_monitor_t m;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_critical, false); /* seed critical          */
  (void)internal_update(&m, k_batt_mv_above_critical, true);  /* V1 C1=T,C2=F -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, internal_update(&m, 10U, false)); /* probe warns */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_critical, false); /* seed critical          */
  (void)internal_update(&m, k_batt_mv_clear_critical, false); /* V2 C1=F,C2=T -> re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_critical, internal_update(&m, 10U, false)); /* probe warns */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_batt_monitor_init(&m));
  (void)internal_update(&m, k_batt_mv_below_critical, false); /* seed critical             */
  (void)internal_update(&m, k_batt_mv_above_critical, false); /* V3 C1=F,C2=F -> no re-arm */
  TEST_ASSERT_EQ(k_ra8_batt_nag_none, internal_update(&m, 10U, false)); /* probe quiet */
  TEST_END("ra8_batt re-arm-critical MC/DC");
}

int main(void)
{
  internal_test_batt_basic_edges();
  internal_test_batt_rearm_on_rise();
  internal_test_batt_charging();
  internal_test_batt_clamp_and_str();
  internal_test_batt_nullguards();
  internal_test_mcdc_raise_low();
  internal_test_mcdc_raise_critical();
  internal_test_mcdc_rearm_low();
  internal_test_mcdc_rearm_critical();
  return 0;
}
