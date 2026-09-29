/**
 * @file test_ra8_fpu_probe.c
 * @brief Unit tests for the double-precision FPU probe (ra8_fpu_probe.c)
 *
 * @details
 * `ra8_fpu_dp_madd()` is the tiny `double` product-sum whose ARM object
 * code witnesses the FPU width of the build target (soft-float calls on
 * every default build, RA8D2 and RA8P1 alike; hardware `.f64` opcodes
 * only on an opt-in `RA8P1_DP_FPU=ON` build -- see ra8_fpu_probe.h and
 * issue #225). That codegen contrast is checked out-of-band by
 * disassembling the cross-compiled objects. This host test is the
 * complementary numeric check: it runs the identical source on the
 * host's native binary64 hardware and asserts the arithmetic result, so
 * a refactor that broke the math would fail here regardless of the
 * target FPU.
 *
 * Every input triple below has a product-sum that is exactly
 * representable in IEEE-754 binary64, so exact equality is used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_fpu_probe.h"
#include "unity_minimal.h"

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_0p25 = 0.25;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_0p5 = 0.5;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_1p5 = 1.5;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_2p0 = 2.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_3p0 = 3.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_4p0 = 4.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_ra8_fpu_dp_madd_6p0 = 6.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_test_assert_2199023255553p0 = 2199023255553.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_test_assert_42p0 = 42.0;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_test_assert_5p5 = 5.5;

/** @brief Named double constant used by this file. */
static const double k_fpu_probe_test_assert_7p0 = 7.0;

/**
 * @test test_ra8_fpu_dp_madd_values
 *
 * @par MC/DC:
 * (no compound decisions -- ra8_fpu_dp_madd() is a single arithmetic
 * expression `a * b + c` with no `&&` / `||`; each assertion is one
 * exact-equality comparison.)
 */
static void test_ra8_fpu_dp_madd_values(void)
{
  TEST_BEGIN("ra8_fpu_dp_madd computes a*b+c in double precision");

  /* 2*3 + 1 == 7 (all operands and result exactly representable). */
  TEST_ASSERT(ra8_fpu_dp_madd(k_fpu_probe_ra8_fpu_dp_madd_2p0,
                              k_fpu_probe_ra8_fpu_dp_madd_3p0,
                              1.0) == k_fpu_probe_test_assert_7p0);

  /* Negative multiplicand: -1.5*4 + 0.5 == -5.5. */
  TEST_ASSERT(ra8_fpu_dp_madd(-k_fpu_probe_ra8_fpu_dp_madd_1p5,
                              k_fpu_probe_ra8_fpu_dp_madd_4p0,
                              k_fpu_probe_ra8_fpu_dp_madd_0p5) == -k_fpu_probe_test_assert_5p5);

  /* Sub-integer, exactly representable: 0.5*0.5 + 0.25 == 0.5. */
  TEST_ASSERT(ra8_fpu_dp_madd(k_fpu_probe_ra8_fpu_dp_madd_0p5,
                              k_fpu_probe_ra8_fpu_dp_madd_0p5,
                              k_fpu_probe_ra8_fpu_dp_madd_0p25) == k_fpu_probe_ra8_fpu_dp_madd_0p5);

  /* Additive identity: a*b + 0 == a*b. */
  TEST_ASSERT(ra8_fpu_dp_madd(k_fpu_probe_ra8_fpu_dp_madd_6p0, k_fpu_probe_test_assert_7p0, 0.0) ==
              k_fpu_probe_test_assert_42p0);

  TEST_END("ra8_fpu_dp_madd computes a*b+c in double precision");
}

/**
 * @test test_ra8_fpu_dp_madd_wide_magnitude
 *
 * @par MC/DC:
 * (no compound decisions -- single equality comparisons only.)
 *
 * A magnitude that overflows a 32-bit and a single-precision mantissa
 * but is exact in binary64, guarding that the computation is genuinely
 * double-width and not silently narrowed.
 */
static void test_ra8_fpu_dp_madd_wide_magnitude(void)
{
  TEST_BEGIN("ra8_fpu_dp_madd preserves binary64 magnitude");

  /* 2^40 * 2 + 1 == 2199023255553, exact in binary64, NOT in binary32. */
  const double two_pow_40 = 1099511627776.0;
  TEST_ASSERT(ra8_fpu_dp_madd(two_pow_40, k_fpu_probe_ra8_fpu_dp_madd_2p0, 1.0) ==
              k_fpu_probe_test_assert_2199023255553p0);

  TEST_END("ra8_fpu_dp_madd preserves binary64 magnitude");
}

/**
 * @test test_ra8_fpu_dp_selected_off_target
 *
 * @par MC/DC:
 * RA8_FPU_DP_SELECTED is a compile-time decision on
 * `defined(__ARM_FP) && ((__ARM_FP) & 0x8) != 0`. A host build takes the
 * first condition false (no `__ARM_FP`), which is the only branch
 * reachable here; the target branches are covered out-of-band by
 * cross-compiling `ra8_fpu_probe.c` under both `-mfpu` settings, where
 * the header's own `#error` guard fails the build if the macro and the
 * `RA8_FPU_DP_ENABLED` switch disagree.
 *
 * Pins two things the #225 resolution turns on. First, an off-target
 * build claims no RA8 FPU selection at all, so nothing can read a host
 * run as evidence about silicon. Second, the numeric contract is
 * independent of that selection: the same source, compiled soft-float or
 * DP, must return the same value, which is what makes the default
 * single-precision build (`fpv5-sp-d16` on both parts) correctness-safe
 * rather than a downgrade.
 */
static void test_ra8_fpu_dp_selected_off_target(void)
{
  TEST_BEGIN("RA8_FPU_DP_SELECTED is 0 off-target and does not change the result");

  /* Host builds define no __ARM_FP, so no DP FPU is claimed. */
  TEST_ASSERT(RA8_FPU_DP_SELECTED == 0);

  /* Same arithmetic, asserted again against a selection-independent
   * expectation: 1.5*2 + 0.25 == 3.25, exact in binary64 either way. */
  const double expected_3p25 = 3.25;
  TEST_ASSERT(ra8_fpu_dp_madd(k_fpu_probe_ra8_fpu_dp_madd_1p5,
                              k_fpu_probe_ra8_fpu_dp_madd_2p0,
                              k_fpu_probe_ra8_fpu_dp_madd_0p25) == expected_3p25);

  TEST_END("RA8_FPU_DP_SELECTED is 0 off-target and does not change the result");
}

int main(void)
{
  test_ra8_fpu_dp_madd_values();
  test_ra8_fpu_dp_madd_wide_magnitude();
  test_ra8_fpu_dp_selected_off_target();
  return 0;
}
