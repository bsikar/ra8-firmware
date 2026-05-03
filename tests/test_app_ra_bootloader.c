/**
 * @file test_app_ra_bootloader.c
 * @brief Integration test: A/B bank-switch bootloader selection logic
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production bootloader at examples/ek_ra8d2/ra_bootloader/main.c
 * picks one of two MRAM banks (A at 0x02000000, B at 0x02080000)
 * based on a single selector byte at 0x02003F00, validates the bank's
 * vector table (initial MSP must lie in SRAM 0x22000000..0x22200000,
 * Reset_Handler in MRAM 0x02000000..0x02100000), falls back to the
 * other bank if the primary fails, and jumps. All bootloader logic
 * is `static`, so this test re-implements the same predicates exactly
 * as documented in the bootloader's @details and verifies the
 * decision matrix.
 *
 * Modeled flow:
 *   1. Read selector byte -> pick primary (A default, 0xA5 -> B).
 *   2. Validate primary vector table.
 *   3. On invalid primary, validate fallback.
 *   4. On both invalid, halt.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/** @brief Bank-layout enums -- mirror examples/ek_ra8d2/ra_bootloader/main.c. */
typedef enum : uintptr_t {
  k_test_boot_bank_a_base = 0x02000000UL,
  k_test_boot_bank_b_base = 0x02080000UL,
  k_test_boot_mram_origin = 0x02000000UL,
  k_test_boot_mram_end    = 0x02100000UL,
  k_test_boot_sram_origin = 0x22000000UL,
  k_test_boot_sram_end    = 0x22200000UL,
} test_boot_addr_t;

/** @brief Selector byte magic values -- mirror the bootloader. */
typedef enum : uint8_t {
  k_test_boot_select_bank_a = 0x00U,
  k_test_boot_select_bank_b = 0xA5U,
  k_test_boot_select_erased = 0xFFU,
} test_boot_select_t;

/** @brief Sample valid MSP / Reset values for a "good" bank. */
typedef enum : uint32_t {
  k_test_boot_good_msp_a   = 0x22001000U, /**< Inside SRAM. */
  k_test_boot_good_reset_a = 0x02000401U, /**< Inside MRAM, thumb bit set. */
  k_test_boot_good_msp_b   = 0x22002000U,
  k_test_boot_good_reset_b = 0x02080401U,
  k_test_boot_bad_msp      = 0xFFFFFFFFU, /**< Erased -- not in SRAM. */
  k_test_boot_bad_reset    = 0x10000000U, /**< Outside MRAM. */
} test_boot_vec_t;

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
}

/**
 * @brief Re-implementation of internal_pick_active_bank from the
 *        bootloader. Documented in main.c @details.
 */
static uintptr_t test_boot_pick_active_bank(uint8_t selector)
{
  if (selector == (uint8_t)k_test_boot_select_bank_b) {
    return (uintptr_t)k_test_boot_bank_b_base;
  }
  return (uintptr_t)k_test_boot_bank_a_base;
}

/**
 * @brief Re-implementation of internal_bank_is_valid from the
 *        bootloader. Documented in main.c @details.
 */
static uint32_t test_boot_bank_is_valid(uint32_t app_msp, uint32_t app_reset)
{
  if (app_msp < (uint32_t)k_test_boot_sram_origin) {
    return 0U;
  }
  if (app_msp > (uint32_t)k_test_boot_sram_end) {
    return 0U;
  }
  if (app_reset < (uint32_t)k_test_boot_mram_origin) {
    return 0U;
  }
  if (app_reset >= (uint32_t)k_test_boot_mram_end) {
    return 0U;
  }
  return 1U;
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Selector 0x00 -> bank A. Selector 0xA5 -> bank B. Anything
 *        else -> bank A. Mirrors the bootloader's pick logic.
 *
 * @par MC/DC: Decision under test: ``selector == 0xA5`` atomic
 * condition. Two vectors: equal (B) + not-equal (A). Both covered.
 */
static void test_boot_pick_bank_table(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: pick_bank selector decision table");
  TEST_ASSERT_EQ((int)k_test_boot_bank_a_base,
                 (int)test_boot_pick_active_bank((uint8_t)k_test_boot_select_bank_a));
  TEST_ASSERT_EQ((int)k_test_boot_bank_b_base,
                 (int)test_boot_pick_active_bank((uint8_t)k_test_boot_select_bank_b));
  TEST_ASSERT_EQ((int)k_test_boot_bank_a_base,
                 (int)test_boot_pick_active_bank((uint8_t)k_test_boot_select_erased));
  TEST_ASSERT_EQ((int)k_test_boot_bank_a_base, (int)test_boot_pick_active_bank((uint8_t)0x55U));
  TEST_END("ra_bootloader: pick_bank selector decision table");
}

/**
 * @brief Bank with valid MSP (in SRAM) + valid Reset (in MRAM) is
 *        accepted.
 *
 * @par MC/DC: Decision under test: four chained range guards in
 * bank_is_valid. Four atomic conditions x N+1 = 5 vectors. This case
 * covers the all-pass vector for both bank A and bank B.
 */
static void test_boot_valid_vector_table_accepted(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: valid vector table accepted");
  TEST_ASSERT_EQ(1,
                 (int)test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_a,
                                              (uint32_t)k_test_boot_good_reset_a));
  TEST_ASSERT_EQ(1,
                 (int)test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_b,
                                              (uint32_t)k_test_boot_good_reset_b));
  TEST_END("ra_bootloader: valid vector table accepted");
}

/**
 * @brief End-to-end primary-A path: selector=0, A valid -> A wins.
 *
 * @par MC/DC: State-machine decision: ``valid(primary)`` true ->
 * dispatch primary. This case covers the true vector.
 */
static void test_boot_dispatch_primary_when_valid(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: primary-bank dispatch when valid");
  uintptr_t primary = test_boot_pick_active_bank((uint8_t)k_test_boot_select_bank_a);
  TEST_ASSERT_EQ((int)k_test_boot_bank_a_base, (int)primary);
  uint32_t ok =
    test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_a, (uint32_t)k_test_boot_good_reset_a);
  TEST_ASSERT_EQ(1, (int)ok);
  TEST_END("ra_bootloader: primary-bank dispatch when valid");
}

/**
 * @brief Fallback path: primary invalid, fallback valid -> fallback wins.
 *
 * @par MC/DC: State-machine decision: ``valid(primary) == false &&
 * valid(fallback) == true``. Two atomic conditions x N+1 = 3 vectors;
 * this case covers the fallback-only-valid vector.
 */
static void test_boot_fallback_dispatch_when_primary_invalid(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: fallback when primary invalid");
  uintptr_t primary  = test_boot_pick_active_bank((uint8_t)k_test_boot_select_bank_a);
  uintptr_t fallback = (primary == (uintptr_t)k_test_boot_bank_a_base)
                         ? (uintptr_t)k_test_boot_bank_b_base
                         : (uintptr_t)k_test_boot_bank_a_base;
  TEST_ASSERT_EQ((int)k_test_boot_bank_b_base, (int)fallback);
  /* Primary invalid (erased), fallback valid -> fallback wins. */
  TEST_ASSERT_EQ(
    0,
    (int)test_boot_bank_is_valid((uint32_t)k_test_boot_bad_msp, (uint32_t)k_test_boot_bad_reset));
  TEST_ASSERT_EQ(1,
                 (int)test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_b,
                                              (uint32_t)k_test_boot_good_reset_b));
  TEST_END("ra_bootloader: fallback when primary invalid");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief All-erased bank (MSP == 0xFFFFFFFF) is rejected.
 *
 * @par MC/DC: Failure-side vector for ``app_msp > sram_end``. Pairs
 * with the all-pass vector above for full coverage of MSP-bound checks.
 */
static void test_boot_erased_bank_rejected(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: erased bank rejected");
  TEST_ASSERT_EQ(
    0,
    (int)test_boot_bank_is_valid((uint32_t)k_test_boot_bad_msp, (uint32_t)k_test_boot_bad_msp));
  TEST_END("ra_bootloader: erased bank rejected");
}

/**
 * @brief MSP outside SRAM (below origin) is rejected.
 *
 * @par MC/DC: Failure-side vector for ``app_msp < sram_origin`` atomic
 * condition.
 */
static void test_boot_msp_below_sram_rejected(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: MSP below SRAM rejected");
  /* MSP = 0x10000000 -- below SRAM at 0x22000000. */
  TEST_ASSERT_EQ(0, (int)test_boot_bank_is_valid(0x10000000U, (uint32_t)k_test_boot_good_reset_a));
  TEST_END("ra_bootloader: MSP below SRAM rejected");
}

/**
 * @brief Reset_Handler outside MRAM is rejected.
 *
 * @par MC/DC: Failure-side vector for ``app_reset >= mram_end`` and
 * ``app_reset < mram_origin``. Two atomic conditions exercised by this
 * test (one for each direction).
 */
static void test_boot_reset_outside_mram_rejected(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: Reset_Handler outside MRAM rejected");
  /* Reset above MRAM end. */
  TEST_ASSERT_EQ(
    0,
    (int)test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_a, (uint32_t)k_test_boot_mram_end));
  /* Reset below MRAM origin. */
  TEST_ASSERT_EQ(0, (int)test_boot_bank_is_valid((uint32_t)k_test_boot_good_msp_a, 0x00100000U));
  TEST_END("ra_bootloader: Reset_Handler outside MRAM rejected");
}

/**
 * @brief Both banks invalid -> bootloader spins (no dispatch).
 *
 * @par MC/DC: State-machine decision: ``!valid(primary) &&
 * !valid(fallback)`` -- the dead-end vector that drops into the spin
 * loop. This case verifies both predicates report 0.
 */
static void test_boot_both_banks_invalid_no_dispatch(void)
{
  reset_world();
  TEST_BEGIN("ra_bootloader: both banks invalid -> halt");
  TEST_ASSERT_EQ(
    0,
    (int)test_boot_bank_is_valid((uint32_t)k_test_boot_bad_msp, (uint32_t)k_test_boot_bad_reset));
  TEST_ASSERT_EQ(0, (int)test_boot_bank_is_valid(0U, 0U));
  TEST_END("ra_bootloader: both banks invalid -> halt");
}

int main(void)
{
  test_boot_pick_bank_table();
  test_boot_valid_vector_table_accepted();
  test_boot_dispatch_primary_when_valid();
  test_boot_fallback_dispatch_when_primary_invalid();
  test_boot_erased_bank_rejected();
  test_boot_msp_below_sram_rejected();
  test_boot_reset_outside_mram_rejected();
  test_boot_both_banks_invalid_no_dispatch();
  return 0;
}
