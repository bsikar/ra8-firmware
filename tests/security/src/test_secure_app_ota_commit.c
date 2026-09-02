/**
 * @file test_secure_app_ota_commit.c
 * @brief Unit + MC/DC tests for libs/ra8_secure_app/src/ota_commit.c
 *
 * @details
 * Exercises the secure-side OTA bank commit shadow + the bank-config
 * masking write path. Includes a targeted MC/DC vector set for the
 * compound boolean decision identified in docs/MCDC_GAPS.csv at
 * libs/ra8_secure_app/src/ota_commit.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ota_commit.h"
#include "ra8_err.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_ota_bank_invalid = 99U, /**< Out-of-enum bank value. */
} test_ota_bank_invalid_t;

typedef enum : uint32_t {
  k_test_ota_raw_full    = 0xFFFFFFFFU, /**< All bits set, expect masked. */
  k_test_ota_raw_partial = 0x12345003U, /**< Only low 2 bits survive.     */
  k_test_ota_raw_zero    = 0x00000000U, /**< Test ota raw zero.           */
} test_ota_raw_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_clears_pending(void)
{
  TEST_BEGIN("ota_commit: reset drops pending swap");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());

  ra8_ota_bank_t target = k_ra8_ota_bank_a;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_ota_commit_pending(&target));
  TEST_END("ota_commit: reset drops pending swap");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_swap_then_pending(void)
{
  TEST_BEGIN("ota_commit: swap_bank arms pending target");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_swap_bank(k_ra8_ota_bank_b));

  ra8_ota_bank_t target = k_ra8_ota_bank_a;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_pending(&target));
  TEST_ASSERT_EQ(k_ra8_ota_bank_b, target);
  TEST_END("ota_commit: swap_bank arms pending target");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_swap_already_pending(void)
{
  TEST_BEGIN("ota_commit: second swap while pending rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_swap_bank(k_ra8_ota_bank_a));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_commit_swap_bank(k_ra8_ota_bank_b));
  TEST_END("ota_commit: second swap while pending rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pending_arg_validation(void)
{
  TEST_BEGIN("ota_commit: pending null pointer rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ota_commit_pending(nullptr));
  TEST_END("ota_commit: pending null pointer rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bank_config_masking(void)
{
  TEST_BEGIN("ota_commit: bank-config write masked to allowed bits");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_set_bank_config((uint32_t)k_test_ota_raw_full));
  uint32_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_get_bank_config(&value));
  TEST_ASSERT_EQ(k_ra8_ota_bank_config_allowed, value);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_set_bank_config((uint32_t)k_test_ota_raw_partial));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_get_bank_config(&value));
  TEST_ASSERT_EQ(((uint32_t)k_test_ota_raw_partial & (uint32_t)k_ra8_ota_bank_config_allowed),
                 value);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ota_commit_get_bank_config(nullptr));
  TEST_END("ota_commit: bank-config write masked to allowed bits");
}

/**
 * @test test_mcdc_swap_bank_target_validation
 *
 * @par MC/DC:
 * Decision: `if ((target != k_ra8_ota_bank_a) && (target != k_ra8_ota_bank_b))`
 * (2 conditions, libs/ra8_secure_app/src/ota_commit.c)
 *  - C1 = (target != k_ra8_ota_bank_a)
 *  - C2 = (target != k_ra8_ota_bank_b)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: target=k_ra8_ota_bank_a -> C1=F (short-circuits).
 *    Decision F. swap_bank arms the pending request, returns ok.
 *  - Vector 2: target=k_ra8_ota_bank_b -> C1=T, C2=F. Decision F.
 *    swap_bank arms the pending request, returns ok.
 *  - Vector 3: target=99 (out of enum) -> C1=T, C2=T. Decision T.
 *    swap_bank rejects with invalid_arg.
 *
 * Vectors 1+3 vary C1 with C2 implicitly held (when C1=F C2 is not
 * evaluated; this is masked-condition MC/DC, accepted by DO-178C
 * 6.4.4.3 for short-circuit operators). Vectors 2+3 vary C2 with C1
 * held T. N+1=3 satisfies minimal MC/DC. Each vector requires its
 * own reset because swap_bank is single-shot until reset.
 */
static void test_mcdc_swap_bank_target_validation(void)
{
  TEST_BEGIN("ota_commit MC/DC: bank-target validation (ota_commit.c:65)");

  /* Vector 1: bank A. C1=F short-circuits, decision F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_swap_bank(k_ra8_ota_bank_a));

  /* Vector 2: bank B. C1=T, C2=F, decision F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_swap_bank(k_ra8_ota_bank_b));

  /* Vector 3: out-of-enum value. C1=T, C2=T, decision T -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_reset());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ota_commit_swap_bank((ra8_ota_bank_t)k_test_ota_bank_invalid));

  TEST_END("ota_commit MC/DC: bank-target validation (ota_commit.c:65)");
}

int main(void)
{
  test_reset_clears_pending();
  test_swap_then_pending();
  test_swap_already_pending();
  test_pending_arg_validation();
  test_bank_config_masking();
  test_mcdc_swap_bank_target_validation();
  return 0;
}
