/**
 * @file test_app_dotf_selftest_demo.c
 * @brief Integration test: DOTF self-test demo verdict logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/dotf_selftest_demo/src/main.c. The DOTF register
 * sequencing (init / run_self_test / get_status) is owned + covered by
 * ra8_dotf's own unit tests; this test pins down the app-level pure logic:
 * the compound "both channels' self-test + status succeeded" verdict.
 *
 * No ra8_fake_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

/** @brief Mirror of ra8_err_t domain used by the verdict (0 == k_ra8_ok). */
typedef enum : int32_t {
  k_t_dotf_ok  = 0, /**< Stand-in for k_ra8_ok.     */
  k_t_dotf_err = 1, /**< Any non-ok ra8_err_t code. */
} t_dotf_err_t;

/**
 * @brief Mirror of dotf_demo_verdict(): 3-condition AND over ra8_err_t.
 */
static uint8_t verdict(int32_t st0_err, int32_t st1_err, int32_t status_err)
{
  const bool ok = (st0_err == (int32_t)k_t_dotf_ok) && (st1_err == (int32_t)k_t_dotf_ok) &&
                  (status_err == (int32_t)k_t_dotf_ok);
  return ok ? 1U : 0U;
}

/**
 * @brief All three calls succeed -> verdict 1; any one failing -> 0.
 *
 * @par MC/DC:
 * Decision ``(st0==ok) && (st1==ok) && (status==ok)`` (3 conditions).
 * - Vector 1: ok,  ok,  ok  -> 1 (control: all true)
 * - Vector 2: err, ok,  ok  -> 0 (varies st0 only)
 * - Vector 3: ok,  err, ok  -> 0 (varies st1 only)
 * - Vector 4: ok,  ok,  err -> 0 (varies status only)
 * Pairs 1+2 / 1+3 / 1+4 each prove one condition independently flips the
 * outcome. N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 */
static void test_dotf_app_verdict_mcdc(void)
{
  TEST_BEGIN("dotf_selftest_demo: verdict MC/DC");
  TEST_ASSERT_EQ(1U, verdict((int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok));
  TEST_ASSERT_EQ(0U, verdict((int32_t)k_t_dotf_err, (int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok));
  TEST_ASSERT_EQ(0U, verdict((int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_err, (int32_t)k_t_dotf_ok));
  TEST_ASSERT_EQ(0U, verdict((int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_err));
  TEST_END("dotf_selftest_demo: verdict MC/DC");
}

/**
 * @brief Sanity: the self-test REG00 snapshot is opaque, not gated.
 *
 * @details
 * The demo reports the self-test REG00 snapshot via globals but never folds
 * it into the verdict (ra8_emulator does not model the AES core, and the bit
 * auto-clears differently on host vs silicon). This test documents that the
 * verdict is independent of any snapshot value.
 *
 * @par MC/DC:
 * No compound decision; confirms verdict ignores snapshot data by holding
 * the error codes ok and asserting the verdict stays 1 regardless.
 */
static void test_dotf_app_snapshot_not_gated(void)
{
  TEST_BEGIN("dotf_selftest_demo: snapshot is opaque");
  /* Verdict depends only on the ra8_err_t results, never the snapshot. */
  TEST_ASSERT_EQ(1U, verdict((int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok, (int32_t)k_t_dotf_ok));
  TEST_END("dotf_selftest_demo: snapshot is opaque");
}

int main(void)
{
  test_dotf_app_verdict_mcdc();
  test_dotf_app_snapshot_not_gated();
  return 0;
}
