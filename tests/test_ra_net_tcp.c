/**
 * @file test_ra_net_tcp.c
 * @brief MC/DC vectors for libs/ra_net/src/ra_net_tcp.c
 *
 * @details
 * Drives compound boolean decisions in ra_net_tcp directly on production
 * source via ra_net_internal.h per the project's "Test access to
 * internal symbols (MC/DC scope)" policy (CLAUDE.md).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_net.h"
#include "ra_net_internal.h"
#include "ra_net_pal.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static const ra_net_config_t k_test_cfg = {
  .mac        = {.bytes = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U}},
  .ip         = {.bytes = {192U, 168U, 1U, 10U}},
  .netmask    = {.bytes = {255U, 255U, 255U, 0U}},
  .gateway    = {.bytes = {192U, 168U, 1U, 1U}},
  .dns_server = {.bytes = {8U, 8U, 8U, 8U}},
};

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_net_pal_deinit();
  (void)ra_net_close();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));
}

/**
 * @test test_mcdc_find_tcp_socket_state
 *
 * @par MC/DC:
 * Decision: ``if (match_state != 0U && t->state != want_state)``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 111)
 *  - V1: match_state=0                  -> C1=F shorts.
 *  - V2: match_state=1, state == want   -> C1=T, C2=F.
 *  - V3: match_state=1, state != want   -> C1=T, C2=T (skip slot).
 * Driven directly via ra_net_tcp_internal_find_socket by setting up a
 * TCP socket in a known state and varying the (match_state, want_state)
 * inputs.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_find_tcp_socket_state(void)
{
  TEST_BEGIN("ra_net_tcp find_socket state MC/DC");
  prep();
  ra_net_handle_t h = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_tcp_listen(80U, &h));
  ra_net_ipv4_t any = {.bytes = {0U, 0U, 0U, 0U}};

  /* V1: match_state=0 -> short circuit; LISTEN socket found regardless. */
  TEST_ASSERT(ra_net_tcp_internal_find_socket(80U, any, 0U, k_tcp_state_closed, 0U) >= 0);
  /* V2: match_state=1, want=LISTEN -> matches. */
  TEST_ASSERT(ra_net_tcp_internal_find_socket(80U, any, 0U, k_tcp_state_listen, 1U) >= 0);
  /* V3: match_state=1, want=ESTABLISHED -> mismatch -> skip. */
  TEST_ASSERT_EQ(
    (int32_t)-1,
    (int32_t)ra_net_tcp_internal_find_socket(80U, any, 0U, k_tcp_state_established, 1U));
  TEST_END("ra_net_tcp find_socket state MC/DC");
}

/**
 * @test test_mcdc_find_tcp_socket_remote
 *
 * @par MC/DC:
 * Decision: ``if (t->remote_port == remote_port && memcmp(ip, ip, 4) == 0)``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 117)
 *  - V1: port mismatch                  -> C1=F shorts.
 *  - V2: port match, IP mismatch        -> C1=T, C2=F.
 *  - V3: port match, IP match           -> C1=T, C2=T (slot found).
 *
 * Driven directly via ra_net_tcp_internal_find_socket by binding a
 * connected TCP socket to a known peer and varying the query 4-tuple.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_find_tcp_socket_remote(void)
{
  TEST_BEGIN("ra_net_tcp find_socket remote MC/DC");
  prep();
  ra_net_state_t* s = ra_net_internal_state();
  /* Hand-construct an ESTABLISHED TCP slot. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_unused) {
      s->socks[i].kind                   = k_ra_net_sock_tcp;
      s->socks[i].tcp.state              = k_tcp_state_established;
      s->socks[i].tcp.local_port         = 80U;
      s->socks[i].tcp.remote_port        = 5000U;
      s->socks[i].tcp.remote_ip.bytes[0] = 10U;
      s->socks[i].tcp.remote_ip.bytes[1] = 0U;
      s->socks[i].tcp.remote_ip.bytes[2] = 0U;
      s->socks[i].tcp.remote_ip.bytes[3] = 1U;
      break;
    }
  }
  ra_net_ipv4_t want_ok = {.bytes = {10U, 0U, 0U, 1U}};
  ra_net_ipv4_t want_no = {.bytes = {10U, 0U, 0U, 2U}};

  /* V1: port mismatch -> C1=F shorts. */
  TEST_ASSERT_EQ(
    (int32_t)-1,
    (int32_t)ra_net_tcp_internal_find_socket(80U, want_ok, 5001U, k_tcp_state_established, 1U));
  /* V2: port match, IP mismatch -> C1=T, C2=F. */
  TEST_ASSERT_EQ(
    (int32_t)-1,
    (int32_t)ra_net_tcp_internal_find_socket(80U, want_no, 5000U, k_tcp_state_established, 1U));
  /* V3: port match, IP match -> C1=T, C2=T. */
  TEST_ASSERT(ra_net_tcp_internal_find_socket(80U, want_ok, 5000U, k_tcp_state_established, 1U) >=
              0);
  TEST_END("ra_net_tcp find_socket remote MC/DC");
}

/**
 * @test test_mcdc_tcp_emit_segment_payload
 *
 * @par MC/DC:
 * Decision: ``if ((data_len != 0U) && (data != nullptr))``
 * (2 conditions, libs/ra_net/src/ra_net_tcp.c line 204)
 *  - V1: data_len=0                     -> C1=F shorts.
 *  - V2: data_len>0, data=NULL          -> C1=T, C2=F.
 *  - V3: data_len>0, data!=NULL         -> C1=T, C2=T (memcpy payload).
 *
 * Driven directly via ra_net_tcp_internal_emit_segment.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_tcp_emit_segment_payload(void)
{
  TEST_BEGIN("ra_net_tcp tcp_emit_segment payload MC/DC");
  prep();
  ra_net_state_t* s = ra_net_internal_state();
  /* Set up a TCP socket so emit_segment has valid context. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_net_max_sockets; i++) {
    if (s->socks[i].kind == k_ra_net_sock_unused) {
      s->socks[i].kind                   = k_ra_net_sock_tcp;
      s->socks[i].tcp.state              = k_tcp_state_established;
      s->socks[i].tcp.local_port         = 80U;
      s->socks[i].tcp.remote_port        = 5000U;
      s->socks[i].tcp.remote_ip.bytes[0] = 10U;
      s->socks[i].tcp.remote_ip.bytes[3] = 1U;
      ra_net_tcp_sock_t* t               = &s->socks[i].tcp;
      const uint8_t      buf[1]          = {0xAAU};

      /* V1: data_len=0, data=NULL -> short circuit, no copy. */
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, NULL, 0U);
      /* V2: data_len>0, data=NULL -> defensive skip (no copy). */
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, NULL, 1U);
      /* V3: data_len>0, data!=NULL -> memcpy payload. */
      (void)ra_net_tcp_internal_emit_segment(t, (uint8_t)k_tcp_flag_ack, buf, 1U);
      break;
    }
  }
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
