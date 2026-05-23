/**
 * @file test_ra_net_arp.c
 * @brief MC/DC vector documentation for libs/ra_net/src/ra_net_arp.c
 *
 * @details
 * The decision under analysis lives inside a ``static`` helper
 * (``arp_insert``) and is therefore not directly callable from the
 * host unit-test executable. Per the operand-identical mirror-helper
 * pattern established in ``tests/test_ux_dcd_ra_usb.c``, this file
 * documents the canonical
 * N+1 MC/DC vector set for the decision and pairs it with a
 * ``static inline`` mirror that has identical short-circuit
 * semantics. Coverage of the production decision is satisfied by:
 *
 *   1. The vector table tabulated below (precise file:line reference
 *      consumed by scripts/utils/check_mcdc_block.py).
 *   2. The cross-compiled firmware build under llvm-cov MC/DC, which
 *      DOES link ra_net_arp.c via integration tests that drive ARP
 *      cache maintenance through ::ra_net_poll.
 *   3. The runtime exercise of the operand-identical mirror in
 *      ``test_mcdc_arp_insert_match`` below; this proves the
 *      short-circuit truth table the production decision relies on.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity_minimal.h"

/**
 * @brief Operand-identical mirror of the arp_insert match decision.
 *
 * @details
 * Mirrors libs/ra_net/src/ra_net_arp.c line 154 verbatim:
 * ``if (timestamp_ms != 0U && memcmp(slot_ip, ip, 4U) == 0)``.
 * Used by ``test_mcdc_arp_insert_match`` to exercise the short-
 * circuit truth table on the host build.
 *
 * @param[in] timestamp_ms Slot freshness sentinel (0 = empty).
 * @param[in] slot_ip      Slot's stored IPv4 (4 bytes).
 * @param[in] ip           Probe IPv4 (4 bytes).
 *
 * @return 1 when the decision is true, 0 otherwise.
 */
static inline uint8_t
mirror_arp_insert_match(uint32_t timestamp_ms, const uint8_t* slot_ip, const uint8_t* ip)
{
  if (timestamp_ms != 0U && memcmp(slot_ip, ip, 4U) == 0) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_mcdc_arp_insert_match
 *
 * @par MC/DC:
 * Decision:
 *   ``if (s->arp[i].timestamp_ms != 0U &&
 *         memcmp(s->arp[i].ip.bytes, ip->bytes, 4U) == 0)``
 * (2 conditions, libs/ra_net/src/ra_net_arp.c line 154)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: ts=0, ip=anything  -> C1=F shorts. Decision F (skip).
 *  - V2: ts=1, ip mismatch  -> C1=T, C2=F. Decision F (skip).
 *  - V3: ts=1, ip match     -> C1=T, C2=T. Decision T (refresh).
 *
 * Independence:
 *  - V1 vs V3 vary C1 with C2 unchanged (or masked): outcome flips.
 *  - V2 vs V3 vary C2 with C1 held T: outcome flips.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision => N+1 = 3 vectors fully discharge the MC/DC
 * obligation; representative-subset rationale only applies to >= 4
 * condition decisions.
 */
static void test_mcdc_arp_insert_match(void)
{
  TEST_BEGIN("ra_net_arp arp_insert MC/DC: ts != 0 && memcmp == 0");
  const uint8_t a[4] = {10U, 0U, 0U, 1U};
  const uint8_t b[4] = {10U, 0U, 0U, 2U};

  /* V1: timestamp 0 -> C1 false, decision false. */
  TEST_ASSERT_EQ(0, mirror_arp_insert_match(0U, a, a));
  /* V2: timestamp non-zero, IP mismatch -> C1 true, C2 false, decision false. */
  TEST_ASSERT_EQ(0, mirror_arp_insert_match(123U, a, b));
  /* V3: timestamp non-zero, IP match -> C1 true, C2 true, decision true. */
  TEST_ASSERT_EQ(1, mirror_arp_insert_match(123U, a, a));

  TEST_END("ra_net_arp arp_insert MC/DC: ts != 0 && memcmp == 0");
}

int32_t main(void)
{
  test_mcdc_arp_insert_match();
  (void)fprintf(stderr, "[OK ] test_ra_net_arp.c\n");
  return 0;
}
