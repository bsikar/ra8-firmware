/**
 * @file test_ra8_decomp_limits.c
 * @brief Tests for the unified decompression-limits policy engine.
 *
 * @details
 * The policy record + budget tracker in `libs/ra8_core/src/ra8_decomp_limits.c`
 * is the one enforcement seam every archive decoder charges, so this suite
 * proves each axis independently and adversarially:
 *
 *   1. the default policy is fully populated and validates,
 *   2. binding rejects a policy with ANY zero field (six vectors),
 *   3. every charge axis fails closed exactly at its bound (output cap,
 *      ratio + grace, entries, iterations, depth),
 *   4. the saturating arithmetic cannot be wrapped by hostile 64-bit values
 *      (output-counter wrap, ratio-product wrap, grace-sum wrap),
 *   5. the header-level declared-size check rejects lying headers with the
 *      same bounds at O(1) cost.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum td_dim_t
 * @brief Tightened bounds used across the suite (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_td_cap     = 1000U, /**< Small output cap.       */
  k_td_ratio   = 4U,    /**< Small ratio bound.      */
  k_td_grace   = 10U,   /**< Small additive grace.   */
  k_td_entries = 3U,    /**< Small entry cap.        */
  k_td_iters   = 2U,    /**< Small iteration budget. */
  k_td_depth   = 1U,    /**< Single decode layer.    */
} td_dim_t;

/** @brief A fully-tightened valid policy for the breach vectors. */
static ra8_decomp_limits_t td_tight(void)
{
  ra8_decomp_limits_t lim = {};
  lim.max_output_bytes    = (uint64_t)k_td_cap;
  lim.max_ratio           = (uint32_t)k_td_ratio;
  lim.ratio_grace_bytes   = (uint32_t)k_td_grace;
  lim.max_entries         = (uint32_t)k_td_entries;
  lim.max_iterations      = (uint32_t)k_td_iters;
  lim.max_depth           = (uint8_t)k_td_depth;
  return lim;
}

/**
 * @test test_decomp_default_policy
 * @brief The default policy is fully populated and binds cleanly.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the validator is six independent
 * single-condition checks, each driven by test_decomp_zero_field_rejected.)
 */
static void test_decomp_default_policy(void)
{
  TEST_BEGIN("decomp: default policy populated + binds");
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  TEST_ASSERT(lim.max_output_bytes == (uint64_t)k_ra8_decomp_def_output_bytes);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_ratio, lim.max_ratio);
  TEST_ASSERT_EQ(k_ra8_decomp_def_ratio_grace, lim.ratio_grace_bytes);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_entries, lim.max_entries);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_iters, lim.max_iterations);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_depth, lim.max_depth);

  ra8_decomp_budget_t b = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(0U, b.entries);
  /* NULL limits selects the same default policy. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, nullptr));
  TEST_ASSERT(b.limits.max_output_bytes == (uint64_t)k_ra8_decomp_def_output_bytes);
  /* NULL budget is refused. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_init(nullptr, &lim));
  TEST_END("decomp: default policy populated + binds");
}

/**
 * @test test_decomp_zero_field_rejected
 * @brief Binding rejects a policy with any single zero field.
 *
 * @par MC/DC:
 * (no compound decisions under test -- `s_limits_usable` is six independent
 * single-condition early returns; each vector below drives exactly one.)
 */
static void test_decomp_zero_field_rejected(void)
{
  TEST_BEGIN("decomp: zero-field policies rejected");
  ra8_decomp_budget_t b = {};

  ra8_decomp_limits_t lim = td_tight();
  lim.max_output_bytes    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim           = td_tight();
  lim.max_ratio = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim                   = td_tight();
  lim.ratio_grace_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim             = td_tight();
  lim.max_entries = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim                = td_tight();
  lim.max_iterations = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim           = td_tight();
  lim.max_depth = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  TEST_END("decomp: zero-field policies rejected");
}

/**
 * @test test_decomp_output_and_ratio_axes
 * @brief The output cap and the ratio + grace bound each fire exactly.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the cap and ratio checks are
 * independent single-condition guards exercised in both directions here.)
 */
static void test_decomp_output_and_ratio_axes(void)
{
  TEST_BEGIN("decomp: output cap + ratio bound fire exactly");
  const ra8_decomp_limits_t lim = td_tight();
  ra8_decomp_budget_t       b   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_output(nullptr, 0U, 0U));

  /* in=100: ratio bound admits 100*4+10 = 410 output bytes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 100U, 410U));
  /* One more byte breaches the ratio, well under the absolute cap. */
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, ra8_decomp_budget_charge_output(&b, 100U, 1U));

  /* Fresh budget: the absolute cap fires first when input is plentiful. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 500U, (uint64_t)k_td_cap));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, ra8_decomp_budget_charge_output(&b, 500U, 1U));
  TEST_END("decomp: output cap + ratio bound fire exactly");
}

/**
 * @test test_decomp_saturating_arithmetic
 * @brief Hostile 64-bit values saturate instead of wrapping any bound.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each overflow guard is a
 * single-condition check driven in both directions across this suite.)
 */
static void test_decomp_saturating_arithmetic(void)
{
  TEST_BEGIN("decomp: hostile 64-bit values saturate, never wrap");
  const ra8_decomp_limits_t lim = td_tight();
  ra8_decomp_budget_t       b   = {};

  /* Output-counter wrap attempt: a near-UINT64_MAX delta saturates and is
   * reported as an output-cap breach, not wrapped back under the cap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 8U, 8U));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_decomp_budget_charge_output(&b, 8U, UINT64_MAX - 2U));

  /* Ratio-product wrap attempt: in * ratio would exceed UINT64_MAX, so the
   * ratio test saturates (disabled) and only the absolute cap governs. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_budget_charge_output(&b, UINT64_MAX / 2U, (uint64_t)k_td_cap));

  /* Grace-sum wrap attempt: ratio 1 with in near UINT64_MAX makes
   * product + grace overflow; the bound saturates and the cap governs. */
  ra8_decomp_limits_t one = td_tight();
  one.max_ratio           = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &one));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_budget_charge_output(&b, UINT64_MAX - 2U, (uint64_t)k_td_cap));
  TEST_END("decomp: hostile 64-bit values saturate, never wrap");
}

/**
 * @test test_decomp_entry_iter_depth_axes
 * @brief Entry, iteration, and depth budgets fire exactly at their bounds.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each axis is one single-condition
 * threshold check exercised in both directions.)
 */
static void test_decomp_entry_iter_depth_axes(void)
{
  TEST_BEGIN("decomp: entry / iteration / depth budgets fire");
  const ra8_decomp_limits_t lim = td_tight();
  ra8_decomp_budget_t       b   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_entry(nullptr));
  for (uint32_t i = 0U; i < (uint32_t)k_td_entries; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_entry(&b));
  }
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, ra8_decomp_budget_charge_entry(&b));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_iter(nullptr));
  for (uint32_t i = 0U; i < (uint32_t)k_td_iters; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_iter(&b));
  }
  TEST_ASSERT_EQ(k_ra8_err_decomp_iterations, ra8_decomp_budget_charge_iter(&b));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_enter(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_enter(&b));
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, ra8_decomp_budget_enter(&b));
  ra8_decomp_budget_leave(&b);
  TEST_ASSERT_EQ(0U, b.depth);
  ra8_decomp_budget_leave(&b); /* unbalanced leave: ignored, no wrap */
  TEST_ASSERT_EQ(0U, b.depth);
  ra8_decomp_budget_leave(nullptr); /* NULL-safe teardown */
  TEST_END("decomp: entry / iteration / depth budgets fire");
}

/**
 * @test test_decomp_check_declared
 * @brief Lying header-declared sizes are rejected at O(1), honest ones pass.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the declared-size check reuses the
 * same two single-condition bounds as the running charge.)
 */
static void test_decomp_check_declared(void)
{
  TEST_BEGIN("decomp: declared-size header check");
  const ra8_decomp_limits_t lim = td_tight();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_check_declared(nullptr, 1U, 1U));
  /* Honest pair: 100 packed -> 300 unpacked (ratio 3 < 4). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_check_declared(&lim, 100U, 300U));
  /* Declared output over the absolute cap. */
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_decomp_check_declared(&lim, 100U, (uint64_t)k_td_cap + 1U));
  /* Declared output over the ratio bound (needs in*4+10 < out <= cap). */
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, ra8_decomp_check_declared(&lim, 10U, 60U));
  TEST_END("decomp: declared-size header check");
}

/**
 * @brief Test entry point -- runs the decompression-limits suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_decomp_default_policy();
  test_decomp_zero_field_rejected();
  test_decomp_output_and_ratio_axes();
  test_decomp_saturating_arithmetic();
  test_decomp_entry_iter_depth_axes();
  test_decomp_check_declared();
  (void)fprintf(stderr, "[OK  ] test_ra8_decomp_limits.c\n");
  return 0;
}
