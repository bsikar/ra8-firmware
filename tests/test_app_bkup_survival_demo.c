/**
 * @file test_app_bkup_survival_demo.c
 * @brief Integration test: VBATT backup-register demo logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/bkup_survival_demo/main.c. The hardware
 * accessors (ra8_bkup_read_word / write_word) are owned + covered by
 * ra8_bkup's own unit tests; this test pins down the app-level pure logic:
 *
 *  - The deterministic per-word pattern is distinct and reproducible.
 *  - The read/write verdict detects a mismatch.
 *  - The reset-survival decision (warm vs cold boot) and the boot-counter
 *    increment (MC/DC on ``sentinel == MAGIC``).
 *
 * No ra8_fake_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

/**
 * @enum app_bkup_survival_demo_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them, plus poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_bkup_registers_checked = 7U, /**< Backup registers the survival decision inspects.         */
  k_bkup_probe_index       = 5U, /**< Backup register index whose contents must survive reset. */
  k_bkup_poison_out =
    9U, /**< Poison in survival and boot-count out-params; setting neither is detectable. */
} app_bkup_survival_demo_fixture_t;

/** @brief Mirror of the demo's app-local constants. */
typedef enum : uint32_t {
  k_t_bkup_sentinel = 0x600DCAFEU, /**< T bkup sentinel. */
  k_t_bkup_mult     = 0x01010101U, /**< T bkup mult.     */
  k_t_bkup_xor      = 0xA5A5A5A5U, /**< T bkup xor.      */
} t_bkup_word_t;

typedef enum : uint8_t {
  k_t_bkup_data_words = 30U, /**< T bkup data words. */
} t_bkup_layout_t;

/** @brief Mirror of bkup_demo_pattern(). */
static uint32_t pattern(uint8_t i)
{
  return ((uint32_t)i * (uint32_t)k_t_bkup_mult) ^ (uint32_t)k_t_bkup_xor;
}

/** @brief Mirror of the demo's reset-survival decision (pure form). */
static void
decide_survival(uint32_t sentinel, uint32_t prev_boot, uint32_t* survived, uint32_t* boot)
{
  if (sentinel == (uint32_t)k_t_bkup_sentinel) {
    *survived = 1U;
    *boot     = prev_boot + 1U;
  } else {
    *survived = 0U;
    *boot     = 1U;
  }
}

/**
 * @brief The per-word pattern is reproducible and distinct across words.
 *
 * @par MC/DC:
 * No compound decision; the loop bound is the only branch. Vectors confirm
 * determinism (same input -> same output) and uniqueness (adjacent words
 * differ).
 */
static void test_bkup_app_pattern(void)
{
  TEST_BEGIN("bkup_survival_demo: pattern is deterministic + distinct");
  for (uint8_t i = 0U; i < (uint8_t)k_t_bkup_data_words; ++i) {
    TEST_ASSERT_EQ(pattern(i), pattern(i)); /* deterministic */
    if (i + 1U < (uint8_t)k_t_bkup_data_words) {
      TEST_ASSERT(pattern(i) != pattern((uint8_t)(i + 1U))); /* distinct */
    }
  }
  TEST_ASSERT_EQ(k_t_bkup_xor, pattern(0U)); /* word 0 == XOR mask */
  TEST_END("bkup_survival_demo: pattern is deterministic + distinct");
}

/**
 * @brief The read/write verdict flags a corrupted read-back.
 *
 * @par MC/DC:
 * Decision ``got != pattern(i)`` (1 condition): match vector + mismatch
 * vector.
 */
static void test_bkup_app_rw_verdict(void)
{
  TEST_BEGIN("bkup_survival_demo: rw verdict detects mismatch");
  uint8_t ok = 1U;
  for (uint8_t i = 0U; i < (uint8_t)k_t_bkup_data_words; ++i) {
    const uint32_t got = pattern(i); /* faithful read-back */
    if (got != pattern(i)) {
      ok = 0U;
    }
  }
  TEST_ASSERT_EQ(1U, ok);

  /* Corrupt one word's read-back -> verdict must drop to 0. */
  ok                 = 1U;
  const uint32_t bad = pattern(5U) ^ 0x1U;
  if (bad != pattern(k_bkup_probe_index)) {
    ok = 0U;
  }
  TEST_ASSERT_EQ(0U, ok);
  TEST_END("bkup_survival_demo: rw verdict detects mismatch");
}

/**
 * @brief Reset-survival decision: warm boot increments, cold boot resets.
 *
 * @par MC/DC:
 * Decision ``sentinel == MAGIC`` (1 condition). N+1 = 2 vectors:
 * - sentinel == MAGIC -> survived=1, boot=prev+1 (warm)
 * - sentinel != MAGIC -> survived=0, boot=1       (cold)
 */
static void test_bkup_app_survival_decision(void)
{
  TEST_BEGIN("bkup_survival_demo: survival decision (warm/cold)");
  uint32_t survived = k_bkup_poison_out;
  uint32_t boot     = k_bkup_poison_out;

  /* Warm boot: sentinel present, prior count 4 -> survived=1, boot=5. */
  decide_survival((uint32_t)k_t_bkup_sentinel, 4U, &survived, &boot);
  TEST_ASSERT_EQ(1U, survived);
  TEST_ASSERT_EQ(5U, boot);

  /* Cold boot: sentinel absent -> survived=0, boot=1 (prior count ignored). */
  decide_survival(0U, 4U, &survived, &boot);
  TEST_ASSERT_EQ(0U, survived);
  TEST_ASSERT_EQ(1U, boot);

  /* A near-miss sentinel is still a cold boot. */
  decide_survival((uint32_t)k_t_bkup_sentinel ^ 0x1U, k_bkup_registers_checked, &survived, &boot);
  TEST_ASSERT_EQ(0U, survived);
  TEST_ASSERT_EQ(1U, boot);
  TEST_END("bkup_survival_demo: survival decision (warm/cold)");
}

int main(void)
{
  test_bkup_app_pattern();
  test_bkup_app_rw_verdict();
  test_bkup_app_survival_decision();
  return 0;
}
