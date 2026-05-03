/**
 * @file test_ra_net_udp.c
 * @brief MC/DC vectors for libs/ra_net/src/ra_net_udp.c
 *
 * @details
 * Drives compound boolean decisions in ra_net_udp directly on production
 * source. Internal helpers are reached through ra_net_internal.h per the
 * project's "Test access to internal symbols (MC/DC scope)" policy
 * (CLAUDE.md, SOLID / Dependency Inversion section).
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

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

/** Synthesise a minimal Ethernet+IPv4+UDP frame around a UDP payload. */
static uint16_t build_udp_frame(uint8_t*       frame,
                                uint16_t       sport,
                                uint16_t       dport,
                                const uint8_t* payload,
                                uint16_t       payload_len)
{
  /* Ethernet header (DST/SRC MAC + EtherType=IPv4). */
  for (uint16_t i = 0U; i < 6U; i++) {
    frame[i]      = 0x02U; /* DST = our local mac */
    frame[6U + i] = 0x10U; /* SRC */
  }
  frame[12U] = 0x08U;
  frame[13U] = 0x00U;

  /* IPv4 header (no options, IHL=5). */
  uint8_t* ip      = &frame[14U];
  uint16_t udp_len = (uint16_t)(8U + payload_len);
  uint16_t ip_len  = (uint16_t)(20U + udp_len);
  ip[0]            = 0x45U;
  ip[1]            = 0U;
  ip[2]            = (uint8_t)(ip_len >> 8U);
  ip[3]            = (uint8_t)(ip_len & 0xFFU);
  ip[4]            = 0U;
  ip[5]            = 0U;
  ip[6]            = 0U;
  ip[7]            = 0U;
  ip[8]            = 64U;
  ip[9]            = 17U; /* UDP */
  ip[10]           = 0U;
  ip[11]           = 0U;
  /* Source IP. */
  ip[12] = 192U;
  ip[13] = 168U;
  ip[14] = 1U;
  ip[15] = 50U;
  /* Dest IP (us). */
  ip[16] = 192U;
  ip[17] = 168U;
  ip[18] = 1U;
  ip[19] = 10U;

  uint8_t* udp = &frame[14U + 20U];
  udp[0]       = (uint8_t)(sport >> 8U);
  udp[1]       = (uint8_t)(sport & 0xFFU);
  udp[2]       = (uint8_t)(dport >> 8U);
  udp[3]       = (uint8_t)(dport & 0xFFU);
  udp[4]       = (uint8_t)(udp_len >> 8U);
  udp[5]       = (uint8_t)(udp_len & 0xFFU);
  udp[6]       = 0U;
  udp[7]       = 0U;
  if (payload != NULL && payload_len > 0U) {
    (void)memcpy(&udp[8], payload, payload_len);
  }
  return (uint16_t)(14U + ip_len);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_mcdc_find_udp_socket_match
 *
 * @par MC/DC:
 * Decision: ``if (s->socks[i].kind == k_ra_net_sock_udp && udp.local_port == port)``
 * (2 conditions, libs/ra_net/src/ra_net_udp.c line 69)
 *  - V1: no socket bound (kind=unused)              -> C1=F shorts. -> -1.
 *  - V2: socket bound on port 80, query port 81     -> C1=T, C2=F. -> -1.
 *  - V3: socket bound on port 80, query port 80     -> C1=T, C2=T. -> >=0.
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC. Driven directly
 * against production ra_net_udp_internal_find_socket_by_port via
 * ra_net_internal.h (test-access policy, see CLAUDE.md).
 */
static void test_mcdc_find_udp_socket_match(void)
{
  TEST_BEGIN("ra_net_udp find_socket_by_port MC/DC");
  prep();
  /* V1: no UDP socket at all. */
  TEST_ASSERT_EQ((int32_t)-1, (int32_t)ra_net_udp_internal_find_socket_by_port(80U));
  /* Bind a UDP socket on port 80. */
  ra_net_handle_t h = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_socket(80U, &h));
  /* V2: kind matches, port mismatch. */
  TEST_ASSERT_EQ((int32_t)-1, (int32_t)ra_net_udp_internal_find_socket_by_port(81U));
  /* V3: kind matches, port matches. */
  TEST_ASSERT(ra_net_udp_internal_find_socket_by_port(80U) >= 0);
  TEST_END("ra_net_udp find_socket_by_port MC/DC");
}

/**
 * @test test_mcdc_udp_send_arg_guard
 *
 * @par MC/DC:
 * Decision: ``if ((dst_port == 0U) || (len == 0U))``
 * (2 conditions, libs/ra_net/src/ra_net_udp.c line 258 -- ra_net_udp_send)
 *  - V1: dst_port=53, len=4   -> C1=F, C2=F. Decision F (proceed past guard).
 *  - V2: dst_port=0,  len=4   -> C1=T shorts. -> invalid_arg.
 *  - V3: dst_port=53, len=0   -> C1=F, C2=T. -> invalid_arg.
 * V1 vs V2 vary C1; V1 vs V3 vary C2.
 *
 * Driven directly against production ra_net_udp_send via
 * ra_net_internal.h (test-access policy).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully. V1 returns
 * a non-invalid_arg error code (the call proceeds past this guard);
 * V2/V3 return k_ra_err_invalid_arg.
 */
static void test_mcdc_udp_send_arg_guard(void)
{
  TEST_BEGIN("ra_net_udp_send MC/DC: dst_port==0 || len==0");
  prep();
  ra_net_handle_t h = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_socket(1234U, &h));
  ra_net_ipv4_t dst    = {.bytes = {192U, 168U, 1U, 50U}};
  uint8_t       buf[4] = {1U, 2U, 3U, 4U};

  /* V1: both valid -> guard does NOT trip. May return a non-invalid_arg
   * error from downstream PAL but must not be invalid_arg. */
  ra_err_t e1 = ra_net_udp_send(h, buf, 4U, dst, 53U);
  TEST_ASSERT(e1 != k_ra_err_invalid_arg);

  /* V2: dst_port=0 -> C1 shorts -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_udp_send(h, buf, 4U, dst, 0U));
  /* V3: len=0 -> C2 -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_udp_send(h, buf, 0U, dst, 53U));
  TEST_END("ra_net_udp_send MC/DC: dst_port==0 || len==0");
}

/**
 * @test test_mcdc_udp_handle_dns_route
 *
 * @par MC/DC:
 * Decision: ``if (s->dns_pending != 0U && sport == k_dns_port)``
 * (2 conditions, libs/ra_net/src/ra_net_udp.c line 399)
 *  - V1: pending=0           -> C1=F shorts. Decision F.
 *  - V2: pending=1, sport=80 -> C1=T, C2=F. Decision F.
 *  - V3: pending=1, sport=53 -> C1=T, C2=T. Decision T (route to DNS).
 * V1 vs V3 vary C1; V2 vs V3 vary C2.
 *
 * Driven directly against production ra_net_udp_handle by injecting
 * frames and toggling s->dns_pending via ra_net_internal_state().
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_udp_handle_dns_route(void)
{
  TEST_BEGIN("ra_net_udp_handle DNS route MC/DC");
  prep();
  ra_net_handle_t h = 0;
  /* Bind socket on the dest port we'll target so the frame isn't
   * dropped after the DNS branch. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_socket(53U, &h));

  ra_net_state_t* s         = ra_net_internal_state();
  uint8_t         frame[80] = {};
  uint16_t        flen      = 0U;

  /* V1: pending=0, sport=53. C1 shorts; no DNS routing. */
  s->dns_pending = 0U;
  flen           = build_udp_frame(frame, 53U, 53U, NULL, 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_handle(frame, flen));

  /* V2: pending=1, sport=80. C2 false; no DNS routing. */
  s->dns_pending = 1U;
  s->dns_txid    = 0U;
  flen           = build_udp_frame(frame, 80U, 53U, NULL, 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_handle(frame, flen));

  /* V3: pending=1, sport=53. Both true; DNS branch executed. */
  s->dns_pending = 1U;
  s->dns_txid    = 0U;
  flen           = build_udp_frame(frame, 53U, 53U, NULL, 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_udp_handle(frame, flen));

  TEST_END("ra_net_udp_handle DNS route MC/DC");
}

/**
 * @test test_mcdc_dns_qname_loop
 *
 * @par MC/DC:
 * Decision: ``while (pos < dlen && dns[pos] != 0U)``
 * (2 conditions, libs/ra_net/src/ra_net_udp.c lines 532 and 554 -- two
 * textually identical guards inside dns_consume_response)
 *  - V1: pos<dlen, byte!=0  -> Continue.
 *  - V2: pos>=dlen          -> C1=F shorts. Exit.
 *  - V3: pos<dlen, byte==0  -> C1=T, C2=F. Exit.
 *
 * Driven directly via ra_net_udp_internal_dns_consume_response with
 * crafted DNS payloads (test-access policy).
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully. Two
 * production line numbers share this textually-identical guard, so one
 * vector set discharges both per "structurally equivalent decisions".
 */
static void test_mcdc_dns_qname_loop(void)
{
  TEST_BEGIN("ra_net_udp dns_consume_response qname-loop MC/DC");
  prep();
  ra_net_state_t* s = ra_net_internal_state();
  s->dns_pending    = 1U;
  s->dns_txid       = 0x1234U;

  /* Build DNS response: header + 1 question (qname=AAA terminator) +
   * 1 answer with A record. */
  /* Header: txid=0x1234, flags=0x8000 (response), qd=1, an=1, ns=0, ar=0. */
  uint8_t  msg[64] = {};
  uint16_t pos     = 0U;
  msg[pos++]       = 0x12U;
  msg[pos++]       = 0x34U;
  msg[pos++]       = 0x80U;
  msg[pos++]       = 0x00U;
  msg[pos++]       = 0U;
  msg[pos++]       = 1U; /* qd */
  msg[pos++]       = 0U;
  msg[pos++]       = 1U; /* an */
  msg[pos++]       = 0U;
  msg[pos++]       = 0U;
  msg[pos++]       = 0U;
  msg[pos++]       = 0U;
  /* Question: qname = "a" (1-byte label), null, qtype=1, qclass=1. */
  msg[pos++] = 1U;  /* label len */
  msg[pos++] = 'a'; /* label byte (V1: byte != 0, in-bounds -> continue) */
  msg[pos++] = 0U;  /* root null (V3: pos<dlen, byte==0 -> exit) */
  msg[pos++] = 0U;
  msg[pos++] = 1U; /* qtype=A */
  msg[pos++] = 0U;
  msg[pos++] = 1U; /* qclass=IN */
  /* Answer: name=00 (root), type=A, class=IN, ttl=0, rdlen=4, rdata=1.2.3.4 */
  msg[pos++] = 0U;
  msg[pos++] = 0U;
  msg[pos++] = 1U;
  msg[pos++] = 0U;
  msg[pos++] = 1U;
  msg[pos++] = 0U;
  msg[pos++] = 0U;
  msg[pos++] = 0U;
  msg[pos++] = 0U;
  msg[pos++] = 0U;
  msg[pos++] = 4U;
  msg[pos++] = 1U;
  msg[pos++] = 2U;
  msg[pos++] = 3U;
  msg[pos++] = 4U;

  /* V1+V3 covered by full parse: enters loop (V1), then byte==0 (V3). */
  ra_net_udp_internal_dns_consume_response(msg, pos);

  /* V2: pass dlen so small that the loop-guard pos<dlen short-circuits.
   * Use only the 12-byte header: qd=1 forces entering the question loop
   * and pos==12==dlen so C1=F shorts. */
  s->dns_pending = 1U;
  s->dns_txid    = 0x1234U;
  ra_net_udp_internal_dns_consume_response(msg, 12U);

  TEST_END("ra_net_udp dns_consume_response qname-loop MC/DC");
}

/**
 * @test test_mcdc_dns_qname_root_null
 *
 * @par MC/DC:
 * Decision: ``if (pos < dlen && dns[pos] == 0U)`` (line 539)
 *  - V1: pos<dlen, byte==0 -> T (skip root null).
 *  - V2: pos>=dlen         -> C1=F shorts.
 *  - V3: pos<dlen, byte!=0 -> C1=T, C2=F.
 *
 * Driven directly against ra_net_udp_internal_dns_consume_response.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_dns_qname_root_null(void)
{
  TEST_BEGIN("ra_net_udp dns_consume_response root-null MC/DC");
  prep();
  ra_net_state_t* s = ra_net_internal_state();

  /* V1: full well-formed message with root null after qname. */
  s->dns_pending = 1U;
  s->dns_txid    = 0xAAAAU;
  uint8_t v1[24] =
    {0xAAU, 0xAAU, 0x80U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 'b', 0U, 0U, 1U, 0U, 1U};
  ra_net_udp_internal_dns_consume_response(v1, sizeof(v1));

  /* V2: dlen too small -- loop sees pos>=dlen at root-null check. */
  s->dns_pending = 1U;
  s->dns_txid    = 0xBBBBU;
  uint8_t v2[14] = {0xBBU, 0xBBU, 0x80U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 'c'};
  ra_net_udp_internal_dns_consume_response(v2, sizeof(v2));

  /* V3: byte!=0 at the qname-end position (compressed pointer pattern
   * 0xC0 then advances past, leaving non-zero at pos). */
  s->dns_pending = 1U;
  s->dns_txid    = 0xCCCCU;
  uint8_t v3[18] =
    {0xCCU, 0xCCU, 0x80U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0xC0U, 0x0CU, 0U, 1U, 0U, 1U};
  ra_net_udp_internal_dns_consume_response(v3, sizeof(v3));

  TEST_END("ra_net_udp dns_consume_response root-null MC/DC");
}

static void build_dns_answer(uint8_t*  msg,
                             uint16_t* len,
                             uint16_t  txid,
                             uint16_t  rrtype,
                             uint16_t  rrclass,
                             uint16_t  rdlen)
{
  uint16_t p = 0U;
  msg[p++]   = (uint8_t)(txid >> 8U);
  msg[p++]   = (uint8_t)(txid & 0xFFU);
  msg[p++]   = 0x80U;
  msg[p++]   = 0U;
  msg[p++]   = 0U;
  msg[p++]   = 0U; /* qd=0 */
  msg[p++]   = 0U;
  msg[p++]   = 1U; /* an=1 */
  msg[p++]   = 0U;
  msg[p++]   = 0U;
  msg[p++]   = 0U;
  msg[p++]   = 0U;
  /* Answer: name=root null. */
  msg[p++] = 0U;
  msg[p++] = (uint8_t)(rrtype >> 8U);
  msg[p++] = (uint8_t)(rrtype & 0xFFU);
  msg[p++] = (uint8_t)(rrclass >> 8U);
  msg[p++] = (uint8_t)(rrclass & 0xFFU);
  msg[p++] = 0U;
  msg[p++] = 0U;
  msg[p++] = 0U;
  msg[p++] = 0U;
  msg[p++] = (uint8_t)(rdlen >> 8U);
  msg[p++] = (uint8_t)(rdlen & 0xFFU);
  for (uint16_t i = 0U; i < rdlen; i++) {
    msg[p++] = (uint8_t)(i + 1U);
  }
  *len = p;
}

/**
 * @test test_mcdc_dns_a_record_selector
 *
 * @par MC/DC:
 * Decision: ``if (rrtype == k_dns_rr_a && rrclass == k_dns_rr_class_in && rdlen == 4U)``
 * (3 conditions, libs/ra_net/src/ra_net_udp.c line 570)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 4 for N=3):
 *  - V1: A, IN, rdlen=4   -> Decision T (capture).
 *  - V2: AAAA             -> C1 shorts.
 *  - V3: A, CH            -> C2 shorts.
 *  - V4: A, IN, rdlen=16  -> C3=F.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 3-condition short-circuit AND. N+1 = 4 vectors.
 */
static void test_mcdc_dns_a_record_selector(void)
{
  TEST_BEGIN("ra_net_udp dns_consume_response A-record selector MC/DC");
  prep();
  ra_net_state_t* s = ra_net_internal_state();
  uint8_t         msg[80];
  uint16_t        mlen = 0U;

  /* V1: A, IN, rdlen=4 -> T (captures). */
  s->dns_pending = 1U;
  s->dns_txid    = 0x1111U;
  build_dns_answer(msg, &mlen, 0x1111U, 1U, 1U, 4U);
  ra_net_udp_internal_dns_consume_response(msg, mlen);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s->dns_pending);

  /* V2: AAAA (28), IN, rdlen=4 -> C1 shorts. */
  s->dns_pending = 1U;
  s->dns_txid    = 0x2222U;
  build_dns_answer(msg, &mlen, 0x2222U, 28U, 1U, 4U);
  ra_net_udp_internal_dns_consume_response(msg, mlen);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s->dns_pending); /* not captured */

  /* V3: A, CH (3), rdlen=4 -> C2 shorts. */
  s->dns_pending = 1U;
  s->dns_txid    = 0x3333U;
  build_dns_answer(msg, &mlen, 0x3333U, 1U, 3U, 4U);
  ra_net_udp_internal_dns_consume_response(msg, mlen);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s->dns_pending);

  /* V4: A, IN, rdlen=16 -> C3=F. */
  s->dns_pending = 1U;
  s->dns_txid    = 0x4444U;
  build_dns_answer(msg, &mlen, 0x4444U, 1U, 1U, 16U);
  ra_net_udp_internal_dns_consume_response(msg, mlen);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s->dns_pending);

  TEST_END("ra_net_udp dns_consume_response A-record selector MC/DC");
}

/**
 * @test test_mcdc_dns_query_null_guard
 *
 * @par MC/DC:
 * Decision: ``if ((hostname == nullptr) || (out_ip == nullptr))``
 * (2 conditions, ra_net_dns_query)
 *  - V1: both non-NULL.
 *  - V2: hostname=NULL.
 *  - V3: out_ip=NULL.
 */
static void test_mcdc_dns_query_null_guard(void)
{
  TEST_BEGIN("ra_net_dns_query MC/DC: hostname NULL || out_ip NULL");
  prep();
  ra_net_ipv4_t ip = {};
  ra_err_t      e  = ra_net_dns_query("example.com", &ip);
  TEST_ASSERT(e != k_ra_err_null_ptr);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_net_dns_query(NULL, &ip));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_net_dns_query("example.com", NULL));
  TEST_END("ra_net_dns_query MC/DC: hostname NULL || out_ip NULL");
}

int32_t main(void)
{
  test_mcdc_find_udp_socket_match();
  test_mcdc_udp_send_arg_guard();
  test_mcdc_udp_handle_dns_route();
  test_mcdc_dns_qname_loop();
  test_mcdc_dns_qname_root_null();
  test_mcdc_dns_a_record_selector();
  test_mcdc_dns_query_null_guard();
  (void)fprintf(stderr, "[OK ] test_ra_net_udp.c\n");
  return 0;
}
