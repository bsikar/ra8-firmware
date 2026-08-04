/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_nsc_eth.c
 * @brief MC/DC unit test for libs/ra8_nsc/src/ra8_nsc_eth.c
 *
 * @details
 * Targets the single 2-condition compound decision identified in
 * docs/MCDC_GAPS.csv at libs/ra8_nsc/src/ra8_nsc_eth.c
 * (``ra8_nsc_eth_send`` length validator).  No other unit-test
 * coverage exists for this file -- the broader NSC-veneer harnesses
 * focus on ra8_nsc_ota and ra8_nsc_xspi.
 *
 * The veneer body is short:
 * @code
 *   if ((len == 0U) || (len > (uint16_t)k_ra8_nsc_eth_frame_max)) {
 *       return k_ra8_err_invalid_arg;
 *   }
 *   RA8_NSC_CHECK_NS_RANGE_R(ns_frame, len);
 *   return ra8_net_pal_send_frame(ns_frame, len);
 * @endcode
 *
 * In the host build ``RA8_NSC_CHECK_NS_RANGE_R`` is a no-op; the
 * tail call lands in ``ra8_net_pal_send_frame``.  We avoid pulling
 * the PAL into a known state by NOT initialising it -- the tail
 * call then returns ``k_ra8_err_invalid_state`` (decision F path)
 * which we accept as the "not invalid_arg" outcome that proves the
 * validator did not reject the input.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_net_pal.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_eth_len_ok   = 64U,   /**< Valid Ethernet frame length.  */
  k_test_eth_len_zero = 0U,    /**< Forces C1=T (short-circuits). */
  k_test_eth_len_max  = 1518U, /**< Last accepted length.         */
  k_test_eth_len_over = 1519U, /**< First rejected length.        */
} test_eth_len_t;

/**
 * @test test_mcdc_ra8_nsc_eth
 *
 * @par MC/DC:
 * Decision: ``ra8_nsc_eth_send`` line 53,
 * ``if ((len == 0U) || (len > (uint16_t)k_ra8_nsc_eth_frame_max))``
 * (libs/ra8_nsc/src/ra8_nsc_eth.c). 2 conditions, ``||``.
 * N+1 = 3 vectors:
 * - V1: len=64   -> C1=F, C2=F -> dec F (proceeds; tail call fails
 *                                         with not_initialized -- the
 *                                         validator did NOT reject)
 * - V2: len=0    -> C1=T (short-circuits) -> dec T -> invalid_arg
 * - V3: len=1519 -> C1=F, C2=T -> dec T -> invalid_arg
 * V1+V3 vary C2 only (decision flips); V1+V2 vary C1 only.
 * DO-178C 6.4.4.3 minimal MC/DC subset.
 */
static void test_mcdc_ra8_nsc_eth(void)
{
  TEST_BEGIN("ra8_nsc_eth_send MC/DC: length validator OR");
  static const uint8_t s_frame[1600] = {};

  /* V1: valid length -> validator dec F.  PAL is not initialized in
   * this TU, so the tail call returns not_initialized -- that proves
   * the validator did not short-circuit out with invalid_arg. */
  const ra8_err_t v1 = ra8_nsc_eth_send(s_frame, (uint16_t)k_test_eth_len_ok);
  TEST_ASSERT(v1 != k_ra8_err_invalid_arg);

  /* V2: len=0 -> dec T via C1 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_send(s_frame, (uint16_t)k_test_eth_len_zero));

  /* V3: len=1519 (one past max) -> dec T via C2 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_send(s_frame, (uint16_t)k_test_eth_len_over));

  /* Bonus: maximum still accepted (not part of the N+1 set, but
   * confirms the ``>`` is strict). */
  const ra8_err_t v_max = ra8_nsc_eth_send(s_frame, (uint16_t)k_test_eth_len_max);
  TEST_ASSERT(v_max != k_ra8_err_invalid_arg);

  TEST_END("ra8_nsc_eth_send MC/DC: length validator OR");
}

int32_t main(void)
{
  test_mcdc_ra8_nsc_eth();
  (void)fprintf(stderr, "[OK ] test_ra8_nsc_eth.c\n");
  return 0;
}
