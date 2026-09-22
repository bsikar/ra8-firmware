/**
 * @file test_ra8_regs_umbrella.c
 * @brief Unit tests for the RA8D2 register umbrella header
 *
 * @details
 * This suite exists to give `ra8_regs.h` a consumer. Until #1389 nothing in
 * the tree included it, so no build ever preprocessed it: the umbrella could
 * (and did) drift to re-exporting half the chip, and nothing proved it still
 * compiled at all. `scripts/checks/check_umbrella_regs.py` answers the first
 * question statically; only a compiler answers the second, so this TU includes
 * the umbrella and NOTHING else from the HAL, then names one symbol from each
 * domain group the umbrella declares.
 *
 * Every assertion is a compile-time `static_assert` on a base address or a
 * register-window size, so the suite touches no MMIO and needs no fake
 * register pages: it is the include graph, not the hardware, under test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_regs.h"
#include "unity_minimal.h"

/* Core system: the umbrella reaches the blocks the boot path programmes. */
static_assert(k_ra8_mstp_base_addr != 0U, "umbrella lost the MSTP window");
static_assert(k_ra8_vreg_sysc_base_addr != 0U, "umbrella lost the VREG window");

/* Timers, serial and audio. */
static_assert(k_ra8_ulpt0_base_addr != k_ra8_ulpt1_base_addr, "ULPT bases collided");
static_assert(k_ra8_i2c0_base_addr != 0U, "umbrella lost the IIC window");
static_assert(k_ra8_ssie0_base_addr != k_ra8_ssie1_base_addr, "SSIE bases collided");

/* Analog, storage and graphics. */
static_assert(k_ra8_acmphs0_base_addr != 0U, "umbrella lost the ACMPHS window");
static_assert(k_ra8_sdhi0_base_addr != k_ra8_sdhi1_base_addr, "SDHI bases collided");
static_assert(k_ra8_drw_base_addr != k_ra8_drw_ns_base_addr, "DRW S/NS aliases collided");

/* The register windows the umbrella re-exports are real layouts, not stubs. */
static_assert(sizeof(r_i2c_regs_t) > 0U, "IIC window is empty");
static_assert(sizeof(r_sdhi_regs_t) > 0U, "SDHI window is empty");
static_assert(sizeof(r_ssie_regs_t) > 0U, "SSIE window is empty");
static_assert(sizeof(r_ulpt_regs_t) > 0U, "ULPT window is empty");
static_assert(sizeof(r_acmphs_regs_t) > 0U, "ACMPHS window is empty");

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the contract under test is the
 * umbrella's include graph, asserted at compile time; the runtime body only
 * reports that the translation unit built)
 */
static void test_umbrella_compiles_standalone(void)
{
  TEST_BEGIN("ra8_regs.h: one include reaches every re-exported domain");
  /* Reaching this line means every static_assert above held and the umbrella
   * compiled on its own, with no per-peripheral include beside it. */
  TEST_ASSERT(sizeof(r_i2c_regs_t) > 0U);
  TEST_END("ra8_regs.h: one include reaches every re-exported domain");
}

int main(void)
{
  test_umbrella_compiles_standalone();
  return 0;
}
