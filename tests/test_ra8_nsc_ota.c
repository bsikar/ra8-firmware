/**
 * @file test_ra8_nsc_ota.c
 * @brief Unit tests + MC/DC vectors for libs/ra8_nsc/src/ra8_nsc_ota.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ota_commit.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

/**
 * @test test_mcdc_nsc_ota_commit_target_bank
 *
 * @par MC/DC:
 * Decision:
 *   ``if ((target_bank != (uint8_t)k_ra8_ota_bank_a) &&
 *         (target_bank != (uint8_t)k_ra8_ota_bank_b))``
 * (2 conditions, libs/ra8_nsc/src/ra8_nsc_ota.c line 57)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: target=k_ra8_ota_bank_a (0)  -> C1=F shorts. Decision F (forward).
 *  - V2: target=k_ra8_ota_bank_b (1)  -> C1=T, C2=F. Decision F (forward).
 *  - V3: target=99 (invalid)         -> C1=T, C2=T. Decision T -> invalid_arg.
 *
 * Independence:
 *  - V1 vs V3: C1 flips with C2 (masked, since 0 != 1 trivially);
 *    decision flips F->T.
 *  - V2 vs V3: C2 flips with C1 held T; decision flips F->T.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_nsc_ota_commit_target_bank(void)
{
  TEST_BEGIN("ra8_nsc_ota_commit MC/DC: target_bank != A && target_bank != B");

  /* Reset secure-side commit shadow so each vector sees a clean slate. */
  (void)ra8_ota_commit_reset();
  /* V1: target_bank == A -> C1=F. Decision F, forwards to swap_bank, ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_ota_commit((uint8_t)k_ra8_ota_bank_a));

  (void)ra8_ota_commit_reset();
  /* V2: target_bank == B -> C1=T, C2=F. Decision F, forwards, ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_ota_commit((uint8_t)k_ra8_ota_bank_b));

  (void)ra8_ota_commit_reset();
  /* V3: target_bank == 99 -> C1=T, C2=T. Decision T, invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_ota_commit((uint8_t)99U));

  TEST_END("ra8_nsc_ota_commit MC/DC: target_bank != A && target_bank != B");
}

/**
 * @test test_nsc_flash_bank_config_forward
 *
 * @par MC/DC:
 * ``ra8_nsc_flash_bank_config`` is a non-branching forwarder
 * (libs/ra8_nsc/src/ra8_nsc_ota.c line 89). It contains zero compound
 * decisions, so MC/DC is trivially satisfied by any single call. This
 * presence sentinel documents that fact and exercises the call so the
 * decision-free statement coverage row is non-zero.
 */
static void test_nsc_flash_bank_config_forward(void)
{
  TEST_BEGIN("ra8_nsc_flash_bank_config forwards (no compound decisions)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_flash_bank_config(0x0U));
  TEST_END("ra8_nsc_flash_bank_config forwards (no compound decisions)");
}

int32_t main(void)
{
  test_mcdc_nsc_ota_commit_target_bank();
  test_nsc_flash_bank_config_forward();
  (void)fprintf(stderr, "[OK ] test_ra8_nsc_ota.c\n");
  return 0;
}
