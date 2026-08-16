/**
 * @file test_ra8_nsc_ota.c
 * @brief Unit tests + MC/DC vectors for libs/ra8_nsc/src/ra8_nsc_ota.c
 *
 * @details Verifies target-bank validation and the branch-free flash-bank
 *          configuration forwarder against the host OTA commit seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ota_commit.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

/**
 * @test internal_test_mcdc_nsc_ota_commit_target_bank
 * @brief Verify MC/DC coverage of OTA target-bank validation.
 *
 * @details Resets the commit shadow before each target-bank vector and checks
 *          forwarding for both valid banks plus rejection of an invalid bank.
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
 *
 * @pre The host OTA commit shadow can be reset.
 * @pre Both declared OTA bank identifiers are valid production values.
 * @post Bank A and bank B requests return k_ra8_ok.
 * @post The invalid bank request returns k_ra8_err_invalid_arg.
 *
 * @note Each vector starts with an independent commit shadow.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_mcdc_nsc_ota_commit_target_bank(void)
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
 * @test internal_test_nsc_flash_bank_config_forward
 * @brief Verify the branch-free flash bank configuration forwarder.
 *
 * @details Calls the NSC bank-configuration veneer with a valid scalar and
 *          checks the result from the secure-side implementation.
 *
 * @par MC/DC:
 * ``ra8_nsc_flash_bank_config`` is a non-branching forwarder
 * (libs/ra8_nsc/src/ra8_nsc_ota.c line 89). It contains zero compound
 * decisions, so MC/DC is trivially satisfied by any single call. This
 * presence sentinel documents that fact and exercises the call so the
 * decision-free statement coverage row is non-zero.
 *
 * @pre The host flash bank configuration seam is available.
 * @pre The supplied zero configuration is accepted by the host seam.
 * @post The forwarding call returns k_ra8_ok.
 * @post No caller-owned memory is modified.
 *
 * @note This sentinel covers a function with no decisions.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_nsc_flash_bank_config_forward(void)
{
  TEST_BEGIN("ra8_nsc_flash_bank_config forwards (no compound decisions)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_flash_bank_config(0x0U));
  TEST_END("ra8_nsc_flash_bank_config forwards (no compound decisions)");
}

int main(void)
{
  internal_test_mcdc_nsc_ota_commit_target_bank();
  internal_test_nsc_flash_bank_config_forward();
  return 0;
}
