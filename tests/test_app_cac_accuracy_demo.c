/**
 * @file test_app_cac_accuracy_demo.c
 * @brief Integration test: CAC clock-accuracy demo logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/cac_accuracy_demo/main.c. The CAC register
 * sequencing is owned + covered by ra8_cac's own unit tests; this test
 * pins down the app-level pure logic:
 *
 *  - The expected MAIN-vs-LOCO/32 edge count (24e6 / 1024 = 23437).
 *  - The +/- 6 % window bracketing that count.
 *  - The verdict ``ok = meas_ok && !ferrf && !ovff`` with full MC/DC.
 *  - The CACR1 / CACR2 clock-select encodings.
 *
 * No ra8_fake_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "unity_minimal.h"

/** @brief Mirror of the demo's app-local clock-pair constants. */
typedef enum : uint32_t {
  k_t_cac_main_hz   = 24000000U, /**< T cac main Hz.     */
  k_t_cac_loco_hz   = 32768U,    /**< T cac loco Hz.     */
  k_t_cac_ref_div   = 32U,       /**< T cac ref div.     */
  k_t_cac_tol_shift = 4U,        /**< T cac tol shift.   */
  k_t_cac_expected  = 23437U,    /**< 24e6 * 32 / 32768. */
} t_cac_calc_t;

typedef enum : uint8_t {
  k_t_cac_cacr1_main = 0x00U, /**< T cac cacr1 main. */
  k_t_cac_cacr2_loco = 0x09U, /**< T cac cacr2 loco. */
  k_t_cac_ferrf      = 0x01U, /**< CASTR FERRF bit.  */
  k_t_cac_mendf      = 0x02U, /**< CASTR MENDF bit.  */
  k_t_cac_ovff       = 0x04U, /**< CASTR OVFF bit.   */
} t_cac_bits_t;

/** @brief Mirror of cac_demo_expected(). */
static uint32_t expected(void)
{
  return ((uint32_t)k_t_cac_main_hz * (uint32_t)k_t_cac_ref_div) / (uint32_t)k_t_cac_loco_hz;
}

/** @brief Mirror of the demo's healthy-measurement verdict. */
static uint8_t compute_ok(uint8_t meas_ok, uint8_t ferrf, uint8_t ovff)
{
  return (meas_ok != 0U && ferrf == 0U && ovff == 0U) ? 1U : 0U;
}

/**
 * @brief The expected count + window match the MAIN-vs-LOCO/32 arithmetic.
 *
 * @par MC/DC:
 * Straight-line arithmetic; no compound decision. Vectors confirm the
 * count and that the window brackets it.
 */
static void test_cac_app_window(void)
{
  TEST_BEGIN("cac_accuracy_demo: expected count + window");
  const uint32_t exp = expected();
  TEST_ASSERT_EQ(k_t_cac_expected, exp);

  const uint32_t tol   = exp >> (uint32_t)k_t_cac_tol_shift;
  const uint16_t upper = (uint16_t)(exp + tol);
  const uint16_t lower = (uint16_t)(exp - tol);
  TEST_ASSERT(upper > (uint16_t)exp);
  TEST_ASSERT(lower < (uint16_t)exp);
  /* The (pre-truncation) upper bound must fit the 16-bit limit register.
   * Check the uint32_t sum -- `upper` is already uint16_t, so testing it
   * against 0xFFFF is always true (-Werror=type-limits). */
  TEST_ASSERT((exp + tol) <= 0xFFFFU);
  TEST_END("cac_accuracy_demo: expected count + window");
}

/**
 * @brief Verdict ``meas_ok && !ferrf && !ovff`` exercised for MC/DC.
 *
 * @par MC/DC:
 * 3 conditions. N+1 = 4 vectors:
 * - meas_ok=1, ferrf=0, ovff=0 -> 1   (control)
 * - meas_ok=0, ferrf=0, ovff=0 -> 0   (varies meas_ok)
 * - meas_ok=1, ferrf=1, ovff=0 -> 0   (varies ferrf)
 * - meas_ok=1, ferrf=0, ovff=1 -> 0   (varies ovff)
 */
static void test_cac_app_verdict_mcdc(void)
{
  TEST_BEGIN("cac_accuracy_demo: verdict MC/DC");
  TEST_ASSERT_EQ(1U, compute_ok(1U, 0U, 0U)); /* control      */
  TEST_ASSERT_EQ(0U, compute_ok(0U, 0U, 0U)); /* vary meas_ok */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 1U, 0U)); /* vary ferrf   */
  TEST_ASSERT_EQ(0U, compute_ok(1U, 0U, 1U)); /* vary ovff    */
  TEST_END("cac_accuracy_demo: verdict MC/DC");
}

/**
 * @brief CACR1 / CACR2 encode target = MAIN and reference = LOCO /32.
 *
 * @par MC/DC:
 * Straight-line field extraction; no compound decision.
 */
static void test_cac_app_cacr_encoding(void)
{
  TEST_BEGIN("cac_accuracy_demo: CACR1/CACR2 clock select");
  /* CACR1.FMCS[3:1] = 000 -> main osc target; no division, rising edge. */
  TEST_ASSERT_EQ(0x00U, ((uint8_t)k_t_cac_cacr1_main & 0x0EU));

  /* CACR2.RPS (bit0) = 1 -> internal reference. */
  TEST_ASSERT_EQ(0x01U, ((uint8_t)k_t_cac_cacr2_loco & 0x01U));
  /* CACR2.RSCS[3:1] = 100b (= 4) -> LOCO reference. */
  TEST_ASSERT_EQ(0x04U, (((uint8_t)k_t_cac_cacr2_loco >> 1U) & 0x07U));
  /* CACR2.RCDS[5:4] = 00 -> reference / 32. */
  TEST_ASSERT_EQ(0x00U, (((uint8_t)k_t_cac_cacr2_loco >> 4U) & 0x03U));
  TEST_END("cac_accuracy_demo: CACR1/CACR2 clock select");
}

int main(void)
{
  test_cac_app_window();
  test_cac_app_verdict_mcdc();
  test_cac_app_cacr_encoding();
  return 0;
}
