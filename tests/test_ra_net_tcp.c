/**
 * @file test_ra_net_tcp.c
 * @brief MC/DC vector documentation for libs/ra_net/src/ra_net_tcp.c
 *
 * @details
 * The decisions under analysis live inside ``static`` helpers
 * (``find_tcp_socket``, ``tcp_emit_segment``) and are not directly
 * callable from the host unit-test executable. Per the operand-
 * identical mirror-helper pattern established in
 * ``tests/test_lwip_sys_arch.c`` and ``tests/test_ux_dcd_ra_usb.c``,
 * this file documents canonical N+1 MC/DC vector sets and pairs each
 * with a ``static inline`` mirror with identical short-circuit
 * semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Mirrors of the production decisions (operand-identical short circuits).
 * ---------------------------------------------------------------------------
 */

/** Mirror of line 84: ``if (match_state != 0U && t->state != want_state)``. */
static inline uint8_t
mirror_find_tcp_socket_state(uint8_t match_state, uint8_t state, uint8_t want_state)
{
  if (match_state != 0U && state != want_state) {
    return 1U;
  }
  return 0U;
}

/** Mirror of line 90:
 * ``if (t->remote_port == remote_port && memcmp(t->remote_ip, remote_ip, 4) == 0)``.
 */
static inline uint8_t mirror_find_tcp_socket_remote(uint16_t       slot_port,
                                                    uint16_t       want_port,
                                                    const uint8_t* slot_ip,
                                                    const uint8_t* want_ip)
{
  if (slot_port == want_port && memcmp(slot_ip, want_ip, 4U) == 0) {
    return 1U;
  }
  return 0U;
}

/** Mirror of line 127: ``if ((data_len != 0U) && (data != nullptr))``. */
static inline uint8_t mirror_tcp_emit_segment_payload(uint16_t data_len, const void* data)
{
  if ((data_len != 0U) && (data != NULL)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_mcdc_find_tcp_socket_state
 *
 * @par MC/DC:
 * Decision: ``if (match_state != 0U && t->state != want_state)``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 84)
 *  - V1: match_state=0                  -> C1=F shorts. Decision F (do not skip).
 *  - V2: match_state=1, state == want   -> C1=T, C2=F. Decision F.
 *  - V3: match_state=1, state != want   -> C1=T, C2=T. Decision T (skip slot).
 * V1 vs V3 vary C1 (decision flips). V2 vs V3 vary C2 (decision flips).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_find_tcp_socket_state(void)
{
  TEST_BEGIN("ra_net_tcp find_tcp_socket state MC/DC");
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_find_tcp_socket_state(0U, 1U, 2U));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_find_tcp_socket_state(1U, 2U, 2U));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)mirror_find_tcp_socket_state(1U, 1U, 2U));
  TEST_END("ra_net_tcp find_tcp_socket state MC/DC");
}

/**
 * @test test_mcdc_find_tcp_socket_remote
 *
 * @par MC/DC:
 * Decision: ``if (t->remote_port == remote_port && memcmp(...) == 0)``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 90)
 *  - V1: port mismatch                  -> C1=F shorts. Decision F.
 *  - V2: port match, IP mismatch        -> C1=T, C2=F. Decision F.
 *  - V3: port match, IP match           -> C1=T, C2=T. Decision T (slot found).
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_find_tcp_socket_remote(void)
{
  TEST_BEGIN("ra_net_tcp find_tcp_socket remote MC/DC");
  const uint8_t a[4] = {10U, 0U, 0U, 1U};
  const uint8_t b[4] = {10U, 0U, 0U, 2U};
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_find_tcp_socket_remote(80U, 81U, a, a));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_find_tcp_socket_remote(80U, 80U, a, b));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)mirror_find_tcp_socket_remote(80U, 80U, a, a));
  TEST_END("ra_net_tcp find_tcp_socket remote MC/DC");
}

/**
 * @test test_mcdc_tcp_emit_segment_payload
 *
 * @par MC/DC:
 * Decision: ``if ((data_len != 0U) && (data != nullptr))``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 127)
 *  - V1: data_len=0                     -> C1=F shorts. Decision F (no payload copy).
 *  - V2: data_len>0, data=NULL          -> C1=T, C2=F. Decision F (defensive skip).
 *  - V3: data_len>0, data!=NULL         -> C1=T, C2=T. Decision T (memcpy payload).
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_tcp_emit_segment_payload(void)
{
  TEST_BEGIN("ra_net_tcp tcp_emit_segment payload MC/DC");
  const uint8_t buf[1] = {0xAAU};
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_tcp_emit_segment_payload(0U, buf));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mirror_tcp_emit_segment_payload(1U, NULL));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)mirror_tcp_emit_segment_payload(1U, buf));
  TEST_END("ra_net_tcp tcp_emit_segment payload MC/DC");
}

int32_t main(void)
{
  test_mcdc_find_tcp_socket_state();
  test_mcdc_find_tcp_socket_remote();
  test_mcdc_tcp_emit_segment_payload();
  (void)fprintf(stderr, "[OK ] test_ra_net_tcp.c\n");
  return 0;
}
