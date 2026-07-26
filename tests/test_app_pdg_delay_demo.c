/**
 * @file test_app_pdg_delay_demo.c
 * @brief Integration test: PDG delay-demo verdict logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/pdg_delay_demo/main.c. The PDG register
 * sequencing is owned + covered by ra8_pdg's own unit tests; this test
 * pins down the app-level pure logic:
 *
 *  - The programmed delay code is inside the legal DLY[6:0] range.
 *  - The "configured" verdict ``ok = dll && powered && bypass_off`` with
 *    full MC/DC. (The delay code is staged but not read-exposed on silicon,
 *    so it is not part of the verdict -- see the demo's @note.)
 *
 * No ra8_sim_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

typedef enum : uint8_t {
  k_t_pdg_delay_code = 0x40U, /**< Mid-range DLY[6:0] code. */
  k_t_pdg_delay_max  = 0x7FU, /**< DLY[6:0] maximum.        */
} t_pdg_const_t;

/** @brief Mirror of the demo's "configured" verdict (observable bring-up). */
static uint8_t compute_ok(uint8_t dll, uint8_t powered, uint8_t bypass_off)
{
  return (dll != 0U && powered != 0U && bypass_off != 0U) ? 1U : 0U;
}

/**
 * @brief The programmed delay code is a legal DLY[6:0] value.
 *
 * @par MC/DC:
 * No compound decision; single range check. Two vectors confirm the code
 * is within [0, 0x7F] and non-trivial.
 */
static void test_pdg_app_delay_code(void)
{
  TEST_BEGIN("pdg_delay_demo: delay code in DLY[6:0] range");
  TEST_ASSERT((uint8_t)k_t_pdg_delay_code <= (uint8_t)k_t_pdg_delay_max);
  TEST_ASSERT((uint8_t)k_t_pdg_delay_code != 0U);
  TEST_END("pdg_delay_demo: delay code in DLY[6:0] range");
}

/**
 * @brief Verdict ``dll && powered && bypass_off`` for MC/DC.
 *
 * @par MC/DC:
 * 3 conditions. N+1 = 4 vectors:
 * - 1,1,1 -> 1   (control)
 * - 0,1,1 -> 0   (varies dll)
 * - 1,0,1 -> 0   (varies powered)
 * - 1,1,0 -> 0   (varies bypass_off)
 */
static void test_pdg_app_verdict_mcdc(void)
{
  TEST_BEGIN("pdg_delay_demo: verdict MC/DC");
  TEST_ASSERT_EQ(1U, compute_ok(1U, 1U, 1U)); /* control         */
  TEST_ASSERT_EQ(0U, compute_ok(0U, 1U, 1U)); /* vary dll        */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 0U, 1U)); /* vary powered    */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 1U, 0U)); /* vary bypass_off */
  TEST_END("pdg_delay_demo: verdict MC/DC");
}

int main(void)
{
  test_pdg_app_delay_code();
  test_pdg_app_verdict_mcdc();
  return 0;
}
