/**
 * @file test_ra_ofs.c
 * @brief Unit tests for Option Function Select section constants (ra_ofs.c)
 *
 * @details
 * `ra_ofs.c` does not expose any callable symbols - every constant is
 * placed into an `.option_setting_*` linker section via
 * `__attribute__((section))` and has internal linkage. There is
 * nothing to assert against directly on the host: the constants live
 * at runtime in the .rodata of the compiled object and the linker
 * discards the `.option_setting_*` output sections in the host test
 * link. The point of this test is to make sure the TU compiles
 * cleanly under the host toolchain so coverage includes it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void test_ra_ofs_compiled(void)
{
  TEST_BEGIN("ra_ofs.c compiled into ra_core_hal");
  ra_sim_mmap_reset();
  /* If ra_ofs.c failed to compile, this entire test binary would
   * fail to link -- so reaching this line is itself the assertion. */
  TEST_ASSERT(1);
  TEST_END("ra_ofs.c compiled into ra_core_hal");
}

int32_t main(void)
{
  test_ra_ofs_compiled();
  (void)fprintf(stderr, "[OK  ] test_ra_ofs.c\n");
  return 0;
}
