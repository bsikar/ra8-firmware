/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_display_pal_policy.c
 * @brief Unit tests for the e-ink page-turn refresh-cadence policy.
 *
 * @details
 * Pure-logic coverage of ra8_display_pal_policy: init validation + clamping, the
 * INIT-on-open rule, the three selectable strategies (fast_only / quality /
 * fast_clean), the periodic-clean cadence, the chapter-boundary clean, the
 * full-rect helper, and MC/DC for the "clean turn" compound decision.
 */

#include <stdint.h>

#include "ra8_display_pal.h"
#include "ra8_display_pal_policy.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum pal_refresh_counts_t
 * @brief Partial-refresh counts either side of the policy's full-refresh threshold.
 */
typedef enum : uint8_t {
  k_pal_refreshes_past_threshold =
    10U, /**< Enough to cross it, so the policy must call for a full refresh. */
  k_pal_partial_refreshes =
    5U, /**< Partial refreshes before the policy check; under threshold, no full refresh due. */
} pal_refresh_counts_t;

/** @brief Local cadence used by the cadence/MC-DC tests. */
enum : uint16_t { k_test_clean_every = 4U /**< Test clean every. */ };

/**
 * @test test_policy_init_validates
 * @par MC/DC:
 * (no compound decisions in this test -- exercises display_policy_init's null
 * guard, the out-of-range `kind` check, and the clean_every min/max clamp, all
 * single-condition; no `&&` or `||` in the code under test that this case touches)
 */
static void test_policy_init_validates(void)
{
  TEST_BEGIN("policy init: null, bad kind, clamp");
  display_policy_t p = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_policy_init(nullptr, k_display_policy_fast_clean, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_policy_init(&p, (display_policy_kind_t)99U, 8U));

  /* clean_every clamps to [min, max]. */
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_fast_clean, 0U));
  TEST_ASSERT_EQ(k_display_policy_clean_every_min, p.clean_every);
  TEST_ASSERT_EQ(0U, p.turns_since_clean);
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_fast_clean, 9999U));
  TEST_ASSERT_EQ(k_display_policy_clean_every_max, p.clean_every);
  TEST_END("policy init: null, bad kind, clamp");
}

/**
 * @test test_policy_decide_validates
 * @par MC/DC:
 * (no compound decisions in this test -- exercises display_policy_decide's two
 * null guards and the out-of-range event check, all single-condition; the calls
 * return before reaching priv_decide_fast_clean's compound clean-turn decision;
 * no `&&` or `||` in the code under test that this case touches)
 */
static void test_policy_decide_validates(void)
{
  TEST_BEGIN("policy decide: null + bad event");
  display_policy_t          p = {};
  display_policy_decision_t d = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_fast_clean, 8U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_policy_decide(nullptr, k_display_event_turn, &d));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_policy_decide(&p, k_display_event_turn, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_policy_decide(&p, (display_turn_event_t)9U, &d));
  TEST_END("policy decide: null + bad event");
}

/**
 * @test test_policy_open_is_init
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the open-event INIT path for
 * all three strategies; the `event == k_display_event_open` check is
 * single-condition and returns before the strategy switch or the fast_clean
 * compound; no `&&` or `||` in the code under test that this case touches)
 */
static void test_policy_open_is_init(void)
{
  TEST_BEGIN("policy: open -> INIT full, resets counter (every strategy)");
  const display_policy_kind_t kinds[3] = {k_display_policy_fast_only,
                                          k_display_policy_quality,
                                          k_display_policy_fast_clean};
  for (uint32_t i = 0U; i < 3U; i++) {
    display_policy_t          p = {};
    display_policy_decision_t d = {};
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, kinds[i], k_test_clean_every));
    p.turns_since_clean = 3U; /* dirty -> open must reset it. */
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_open, &d));
    TEST_ASSERT_EQ(k_display_refresh_init, d.hint);
    TEST_ASSERT(d.full_update);
    TEST_ASSERT_EQ(0U, p.turns_since_clean);
  }
  TEST_END("policy: open -> INIT full, resets counter (every strategy)");
}

/**
 * @test test_policy_fast_only
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the fast_only arm of
 * display_policy_decide's `kind` switch on turn and chapter events; a switch case
 * is not a compound boolean; no `&&` or `||` in the code under test that this
 * case touches)
 */
static void test_policy_fast_only(void)
{
  TEST_BEGIN("policy fast_only: always A2 partial on turns + chapters");
  display_policy_t          p = {};
  display_policy_decision_t d = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_fast_only, k_test_clean_every));
  for (uint32_t i = 0U; i < k_pal_refreshes_past_threshold; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
    TEST_ASSERT_EQ(k_display_refresh_fast, d.hint);
    TEST_ASSERT(!d.full_update);
  }
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_chapter, &d));
  TEST_ASSERT_EQ(k_display_refresh_fast, d.hint); /* fast_only ignores chapter. */
  TEST_END("policy fast_only: always A2 partial on turns + chapters");
}

/**
 * @test test_policy_quality
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the quality arm of
 * display_policy_decide's `kind` switch on turn events; a switch case is not a
 * compound boolean; no `&&` or `||` in the code under test that this case touches)
 */
static void test_policy_quality(void)
{
  TEST_BEGIN("policy quality: always GC16 full");
  display_policy_t          p = {};
  display_policy_decision_t d = {};
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_init(&p, k_display_policy_quality, k_test_clean_every));
  for (uint32_t i = 0U; i < k_pal_partial_refreshes; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
    TEST_ASSERT_EQ(k_display_refresh_quality, d.hint);
    TEST_ASSERT(d.full_update);
  }
  TEST_END("policy quality: always GC16 full");
}

/**
 * @test test_policy_fast_clean_cadence
 * @par MC/DC:
 * Decision: `clean = (turns_since_clean + 1 >= clean_every) || (event == chapter)`
 * (2 conditions, function `priv_decide_fast_clean`,
 * libs/ra8_display_pal/src/ra8_display_pal_policy.c). With clean_every = 4 this
 * test drives only turn events, so the chapter condition stays false throughout:
 * - V1 next=1 (>=4 false), event=turn (chapter false) -> clean=false -> A2 partial.
 * - V2 next=4 (>=4 true),  event=turn (chapter false) -> clean=true  -> GC16 full.
 * V1+V2 prove the cadence condition independently flips the outcome. The chapter
 * condition's independent vector (next=1 false, chapter true -> clean=true) is
 * carried by the sibling test_mcdc_clean_turn_decision (its V3). N+1 = 3 across
 * both tests.
 */
static void test_policy_fast_clean_cadence(void)
{
  TEST_BEGIN("policy fast_clean: A2 x (N-1) then GC16 clean, repeating");
  display_policy_t          p = {};
  display_policy_decision_t d = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 display_policy_init(&p, k_display_policy_fast_clean, k_test_clean_every));
  for (uint32_t cycle = 0U; cycle < 2U; cycle++) {
    for (uint32_t i = 0U; i < (uint32_t)k_test_clean_every - 1U; i++) {
      TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
      TEST_ASSERT_EQ(k_display_refresh_fast, d.hint);
      TEST_ASSERT(!d.full_update);
    }
    /* The Nth turn is the clean GC16 full, and resets the counter. */
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
    TEST_ASSERT_EQ(k_display_refresh_quality, d.hint);
    TEST_ASSERT(d.full_update);
    TEST_ASSERT_EQ(0U, p.turns_since_clean);
  }
  TEST_END("policy fast_clean: A2 x (N-1) then GC16 clean, repeating");
}

/**
 * @test test_mcdc_clean_turn_decision
 *
 * @par MC/DC:
 * Decision: `clean = (turns_since_clean + 1 >= clean_every) || (event == chapter)`
 * (2 conditions), in priv_decide_fast_clean (libs/ra8_display_pal/src/ra8_display_pal_policy.c).
 * With clean_every = 4:
 * - V1: next=1 (>=4 false), event=turn  (chapter false) -> clean=false -> A2 partial.
 * - V2: next=4 (>=4 true),  event=turn  (chapter false) -> clean=true  -> GC16 full.
 * - V3: next=1 (>=4 false), event=chapter (chapter true) -> clean=true -> GC16 full.
 * V1+V2 prove the cadence condition independently flips the outcome; V1+V3 prove
 * the same for the chapter condition. N+1 = 3 vectors for N=2: minimal MC/DC.
 */
static void test_mcdc_clean_turn_decision(void)
{
  TEST_BEGIN("policy fast_clean: MC/DC clean-turn decision");
  display_policy_t          p = {};
  display_policy_decision_t d = {};

  /* V1: both conditions false -> not clean. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 display_policy_init(&p, k_display_policy_fast_clean, k_test_clean_every));
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
  TEST_ASSERT_EQ(k_display_refresh_fast, d.hint);
  TEST_ASSERT(!d.full_update);

  /* V2: cadence condition true (3 turns then the 4th), chapter false -> clean. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 display_policy_init(&p, k_display_policy_fast_clean, k_test_clean_every));
  for (uint32_t i = 0U; i < (uint32_t)k_test_clean_every - 1U; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
  }
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_turn, &d));
  TEST_ASSERT_EQ(k_display_refresh_quality, d.hint);
  TEST_ASSERT(d.full_update);

  /* V3: cadence condition false (first turn), chapter true -> clean. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 display_policy_init(&p, k_display_policy_fast_clean, k_test_clean_every));
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_decide(&p, k_display_event_chapter, &d));
  TEST_ASSERT_EQ(k_display_refresh_quality, d.hint);
  TEST_ASSERT(d.full_update);
  TEST_END("policy fast_clean: MC/DC clean-turn decision");
}

/**
 * @test test_policy_full_rect
 * @par MC/DC:
 * Decision: `if ((w == 0U) || (h == 0U))` (2 conditions, function
 * `display_policy_full_rect`, libs/ra8_display_pal/src/ra8_display_pal_policy.c).
 * - V1 w=100, h=50 -> C1=false, C2=false -> false (rect filled, k_ra8_ok).
 * - V2 w=0,   h=50 -> C1=true shorts     -> true  (k_ra8_err_invalid_arg).
 * - V3 w=100, h=0  -> C1=false, C2=true  -> true  (k_ra8_err_invalid_arg).
 * V1+V2 prove w independent; V1+V3 prove h independent. N+1 = 3 vectors.
 * (The leading null-out call returns at the RA8_CHECK_NULL_PTR guard, before this
 * decision.)
 */
static void test_policy_full_rect(void)
{
  TEST_BEGIN("policy full_rect: null, zero dims, valid");
  display_rect_t r = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, display_policy_full_rect(100U, 50U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_policy_full_rect(0U, 50U, &r));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, display_policy_full_rect(100U, 0U, &r));
  TEST_ASSERT_EQ(k_ra8_ok, display_policy_full_rect(100U, 50U, &r));
  TEST_ASSERT_EQ(0U, r.x);
  TEST_ASSERT_EQ(0U, r.y);
  TEST_ASSERT_EQ(100U, r.w);
  TEST_ASSERT_EQ(50U, r.h);
  TEST_END("policy full_rect: null, zero dims, valid");
}

int main(void)
{
  test_policy_init_validates();
  test_policy_decide_validates();
  test_policy_open_is_init();
  test_policy_fast_only();
  test_policy_quality();
  test_policy_fast_clean_cadence();
  test_mcdc_clean_turn_decision();
  test_policy_full_rect();
  return 0;
}
