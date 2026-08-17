/**
 * @file test_coverage_compile_all.c
 * @brief Coverage baseline test -- forces uniform coverage measurement
 *
 * @details
 * This test executable exists solely to ensure every first-party
 * source file in libs/ is compiled exactly once
 * with the active coverage instrumentation flags. The per-test
 * executables in this directory each link against the same
 * `ra8_core_hal` OBJECT library, so the .gcno / .profraw files
 * produced here cover identically the same TUs -- which is exactly
 * the property gcovr / llvm-cov want when computing repository-wide
 * MC/DC and line/branch coverage.
 *
 * The CMakeLists.txt only adds this TU to the build when
 * `RA8_MCDC=ON`. The fast `make test` flow ignores it so unit-test
 * iteration stays cheap.
 *
 * No test cases are registered. The test "passes" if and only if the
 * link succeeds against the coverage-instrumented OBJECT library.
 *
 * @note Mirrors the same-named file in star-rx72n-firmware/tests/.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#include "unity_minimal.h"

/**
 * @brief Aggregator entry point -- runs zero test cases.
 *
 * @details The sole purpose of this executable is to force every
 * first-party source file to be compiled and linked with the active
 * coverage flags. No test cases are registered.
 *
 * @return int Always 0 (success).
 * @retval 0 Link succeeded; coverage baseline established.
 *
 * @pre  All firmware source files linked into ra8_core_hal compile cleanly.
 * @pre  The unity_minimal.h shim is on the include path.
 * @post No firmware state is mutated; the aggregator runs nothing.
 * @post Coverage profile data (.gcda / .profraw) is emitted on exit.
 */
int main(void)
{
  /* No test cases: this target exists for coverage baseline only. */
  return 0;
}
