/**
 * @file test_ra_fpu_probe.c
 * @brief Unit tests for the double-precision FPU probe (ra_fpu_probe.c)
 *
 * @details
 * `ra_fpu_dp_madd()` is the tiny `double` product-sum whose ARM object
 * code witnesses the FPU width of the build target (hardware `.f64`
 * opcodes on the RA8P1 DP-FPU build; soft-float calls on the RA8D2
 * SP-FPU build -- see ra_fpu_probe.h). That codegen contrast is checked
 * out-of-band by disassembling the cross-compiled objects. This host
 * test is the complementary numeric check: it runs the identical source
 * on the host's native binary64 hardware and asserts the arithmetic
 * result, so a refactor that broke the math would fail here regardless
 * of the target FPU.
 *
 * Every input triple below has a product-sum that is exactly
 * representable in IEEE-754 binary64, so exact equality is used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_fpu_probe.h"
#include "unity_minimal.h"

/**
 * @test test_ra_fpu_dp_madd_values
 *
 * @par MC/DC:
 * (no compound decisions -- ra_fpu_dp_madd() is a single arithmetic
 * expression `a * b + c` with no `&&` / `||`; each assertion is one
 * exact-equality comparison.)
 */
static void test_ra_fpu_dp_madd_values(void)
{
  TEST_BEGIN("ra_fpu_dp_madd computes a*b+c in double precision");

  /* 2*3 + 1 == 7 (all operands and result exactly representable). */
  TEST_ASSERT(ra_fpu_dp_madd(2.0, 3.0, 1.0) == 7.0);

  /* Negative multiplicand: -1.5*4 + 0.5 == -5.5. */
  TEST_ASSERT(ra_fpu_dp_madd(-1.5, 4.0, 0.5) == -5.5);

  /* Sub-integer, exactly representable: 0.5*0.5 + 0.25 == 0.5. */
  TEST_ASSERT(ra_fpu_dp_madd(0.5, 0.5, 0.25) == 0.5);

  /* Additive identity: a*b + 0 == a*b. */
  TEST_ASSERT(ra_fpu_dp_madd(6.0, 7.0, 0.0) == 42.0);

  TEST_END("ra_fpu_dp_madd computes a*b+c in double precision");
}

/**
 * @test test_ra_fpu_dp_madd_wide_magnitude
 *
 * @par MC/DC:
 * (no compound decisions -- single equality comparisons only.)
 *
 * A magnitude that overflows a 32-bit and a single-precision mantissa
 * but is exact in binary64, guarding that the computation is genuinely
 * double-width and not silently narrowed.
 */
static void test_ra_fpu_dp_madd_wide_magnitude(void)
{
  TEST_BEGIN("ra_fpu_dp_madd preserves binary64 magnitude");

  /* 2^40 * 2 + 1 == 2199023255553, exact in binary64, NOT in binary32. */
  const double two_pow_40 = 1099511627776.0;
  TEST_ASSERT(ra_fpu_dp_madd(two_pow_40, 2.0, 1.0) == 2199023255553.0);

  TEST_END("ra_fpu_dp_madd preserves binary64 magnitude");
}

int32_t main(void)
{
  test_ra_fpu_dp_madd_values();
  test_ra_fpu_dp_madd_wide_magnitude();
  (void)fprintf(stderr, "[OK  ] test_ra_fpu_probe.c\n");
  return 0;
}
