/**
 * @file test_ra_net_ipv4.c
 * @brief MC/DC vectors for libs/ra_net/src/ra_net_ipv4.c (public APIs)
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
}

/**
 * @test test_mcdc_ra_net_test_inject_frame_len_guard
 *
 * @par MC/DC:
 * Decision: ``if ((len < k_eth_hdr_len) || (len > k_ra_net_pal_frame_max))``
 * (2 conditions, libs/ra_net/src/ra_net_ipv4.c -- ra_net_test_inject_frame)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: len=64 (>=14 and <=1518)   -> C1=F, C2=F. Decision F (proceed).
 *  - V2: len=10 (<14)               -> C1=T shorts. Decision T -> invalid_arg.
 *  - V3: len=4096 (>1518)           -> C1=F, C2=T. Decision T -> invalid_arg.
 *
 * V1 vs V2 vary C1; V1 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_ra_net_test_inject_frame_len_guard(void)
{
  TEST_BEGIN("ra_net_test_inject_frame MC/DC: len < hdr || len > max");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));

  uint8_t frame[1600];
  (void)memset(frame, 0, sizeof frame);

  /* V1: len=64. Decision F. (Eth+ARP-shaped; payload bytes are ignored
   * by the inject queue.) */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_test_inject_frame(frame, 64U));

  /* V2: len=10 (< 14). Decision T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_test_inject_frame(frame, 10U));

  /* V3: len=1600 (> 1518). Decision T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_test_inject_frame(frame, 1600U));

  TEST_END("ra_net_test_inject_frame MC/DC: len < hdr || len > max");
}

/**
 * @test test_mcdc_ra_net_test_drain_tx_null_guard
 *
 * @par MC/DC:
 * Decision: ``if ((out_buf == nullptr) || (inout_len == nullptr))``
 * (2 conditions, libs/ra_net/src/ra_net_ipv4.c -- ra_net_test_drain_tx)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: both non-NULL              -> C1=F, C2=F. Decision F (proceed).
 *  - V2: out_buf=NULL               -> C1=T shorts. Decision T -> null_ptr.
 *  - V3: inout_len=NULL             -> C1=F, C2=T. Decision T -> null_ptr.
 *
 * V1 vs V2 vary C1; V1 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_ra_net_test_drain_tx_null_guard(void)
{
  TEST_BEGIN("ra_net_test_drain_tx MC/DC: out_buf NULL || inout_len NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));

  uint8_t  buf[64];
  uint16_t cap = (uint16_t)sizeof buf;

  /* V1: both non-NULL. Decision F. Returns no_data with empty TX ring. */
  ra_err_t e = ra_net_test_drain_tx(buf, &cap);
  TEST_ASSERT(e == k_ra_ok || e == k_ra_err_no_data);

  /* V2: out_buf NULL -> decision T -> null_ptr. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_net_test_drain_tx(NULL, &cap));

  /* V3: inout_len NULL -> decision T -> null_ptr. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_net_test_drain_tx(buf, NULL));

  TEST_END("ra_net_test_drain_tx MC/DC: out_buf NULL || inout_len NULL");
}

/**
 * @test test_mcdc_ra_net_send_arg_guard
 *
 * @par MC/DC:
 * Decision: ``if ((len == 0U) || (handle >= k_ra_net_max_sockets))``
 * (2 conditions, libs/ra_net/src/ra_net_ipv4.c -- ra_net_send)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: len=4, handle=0          -> C1=F, C2=F. Decision F (proceed).
 *  - V2: len=0, handle=0          -> C1=T shorts. Decision T -> invalid_arg.
 *  - V3: len=4, handle=99         -> C1=F, C2=T. Decision T -> invalid_arg.
 *
 * V1 vs V2 vary C1; V1 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_ra_net_send_arg_guard(void)
{
  TEST_BEGIN("ra_net_send MC/DC: len==0 || handle >= max");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));
  ra_net_handle_t h = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_socket_udp(5555U, &h));

  const uint8_t       payload[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  const ra_net_ipv4_t dst        = {.bytes = {192U, 168U, 1U, 99U}};

  /* V1: valid len + valid handle -> C1=F, C2=F. Decision F. */
  ra_err_t e = ra_net_send(h, payload, 4U, dst, 1234U);
  TEST_ASSERT(e == k_ra_ok || e != k_ra_err_invalid_arg);

  /* V2: len==0 -> decision T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_send(h, payload, 0U, dst, 1234U));

  /* V3: handle out of range -> C1=F, C2=T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_net_send((ra_net_handle_t)99U, payload, 4U, dst, 1234U));

  TEST_END("ra_net_send MC/DC: len==0 || handle >= max");
}

/**
 * @test test_mcdc_ra_net_recv_null_guard
 *
 * @par MC/DC:
 * Decision: ``if ((buf == nullptr) || (got_len == nullptr) ||
 *               (remote_ip == nullptr) || (remote_port == nullptr))``
 * (4 conditions, libs/ra_net/src/ra_net_ipv4.c -- ra_net_recv)
 *
 * Per DO-178C 6.4.4.3, N+1 = 5 vectors are sufficient for masking-MC/DC
 * on a 4-condition short-circuit OR. Representative subset chosen:
 *  - V1: all four non-NULL              -> C1=F,C2=F,C3=F,C4=F. Decision F.
 *  - V2: buf=NULL                       -> C1=T shorts. Decision T.
 *  - V3: got_len=NULL                   -> C1=F,C2=T shorts. Decision T.
 *  - V4: remote_ip=NULL                 -> C1=F,C2=F,C3=T shorts. Decision T.
 *  - V5: remote_port=NULL               -> C1=F,C2=F,C3=F,C4=T. Decision T.
 *
 * Independence (DO-178C 6.4.4.3 short-circuit-aware MC/DC):
 *  - V1 vs V2: C1 flips F->T, decision flips F->T. C2..C4 masked by V2.
 *  - V1 vs V3: C2 flips F->T (with C1 held F), decision flips F->T.
 *  - V1 vs V4: C3 flips F->T (with C1=C2 held F), decision flips F->T.
 *  - V1 vs V5: C4 flips F->T (with C1=C2=C3 held F), decision flips F->T.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 4-condition short-circuit OR. The 5-vector representative subset
 * above (Chilenski "masking MC/DC", DO-178C 6.4.4.3) discharges the
 * MC/DC obligation: every condition appears in a pair where it
 * uniquely determines the outcome. Full multi-condition coverage
 * would require 2^4 = 16 vectors, of which 11 are dominated by the
 * masking-MC/DC subset and therefore unnecessary per DO-178C
 * 6.4.4.3 guidance.
 */
static void test_mcdc_ra_net_recv_null_guard(void)
{
  TEST_BEGIN("ra_net_recv MC/DC: buf||got_len||remote_ip||remote_port NULL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));
  ra_net_handle_t h = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_socket_udp(5556U, &h));

  uint8_t       buf[16];
  uint16_t      got = 0U;
  ra_net_ipv4_t rip = {};
  uint16_t      rp  = 0U;

  /* V1: all non-NULL. Decision F. (no_data when no traffic queued.) */
  ra_err_t e = ra_net_recv(h, buf, (uint16_t)sizeof buf, &got, &rip, &rp);
  TEST_ASSERT(e == k_ra_err_no_data || e == k_ra_ok);

  /* V2: buf NULL. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_net_recv(h, NULL, (uint16_t)sizeof buf, &got, &rip, &rp));
  /* V3: got_len NULL. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_net_recv(h, buf, (uint16_t)sizeof buf, NULL, &rip, &rp));
  /* V4: remote_ip NULL. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_net_recv(h, buf, (uint16_t)sizeof buf, &got, NULL, &rp));
  /* V5: remote_port NULL. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_net_recv(h, buf, (uint16_t)sizeof buf, &got, &rip, NULL));

  TEST_END("ra_net_recv MC/DC: buf||got_len||remote_ip||remote_port NULL");
}

/**
 * @test test_mcdc_ra_net_recv_arg_guard
 *
 * @par MC/DC:
 * Decision: ``if ((max_len == 0U) || (handle >= k_ra_net_max_sockets))``
 * (2 conditions, libs/ra_net/src/ra_net_ipv4.c -- ra_net_recv)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: max_len=16, handle=0     -> C1=F, C2=F. Decision F.
 *  - V2: max_len=0,  handle=0     -> C1=T shorts. Decision T.
 *  - V3: max_len=16, handle=99    -> C1=F, C2=T. Decision T.
 *
 * V1 vs V2 vary C1; V1 vs V3 vary C2.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_ra_net_recv_arg_guard(void)
{
  TEST_BEGIN("ra_net_recv MC/DC: max_len==0 || handle >= max");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_open(&k_test_cfg));
  ra_net_handle_t h = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_socket_udp(5557U, &h));

  uint8_t       buf[16];
  uint16_t      got = 0U;
  ra_net_ipv4_t rip = {};
  uint16_t      rp  = 0U;

  /* V1: max_len>0, handle valid. Decision F (returns no_data). */
  ra_err_t e = ra_net_recv(h, buf, 16U, &got, &rip, &rp);
  TEST_ASSERT(e == k_ra_err_no_data || e == k_ra_ok);

  /* V2: max_len==0 -> decision T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_net_recv(h, buf, 0U, &got, &rip, &rp));

  /* V3: handle out-of-range -> C1=F, C2=T -> invalid_arg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_net_recv((ra_net_handle_t)99U, buf, 16U, &got, &rip, &rp));

  TEST_END("ra_net_recv MC/DC: max_len==0 || handle >= max");
}

int32_t main(void)
{
  test_mcdc_ra_net_test_inject_frame_len_guard();
  test_mcdc_ra_net_test_drain_tx_null_guard();
  test_mcdc_ra_net_send_arg_guard();
  test_mcdc_ra_net_recv_null_guard();
  test_mcdc_ra_net_recv_arg_guard();
  (void)fprintf(stderr, "[OK ] test_ra_net_ipv4.c\n");
  return 0;
}
