/**
 * @file test_ra8_tls_net.c
 * @brief Unit tests for the ``ra8_tls`` TLS-over-network glue: cipher /
 *        verify introspection, the #21 MSS clamp, and an end-to-end
 *        handshake + record exchange driven over ``ra8_net_pal`` frames.
 *
 * @details
 * Compiled with ``RA8_SIMULATOR_MODE`` defined, so ``ra8_tls.c`` runs its
 * loopback BIO drain instead of a real Mbed TLS handshake. That lets the
 * host build exercise the full public contract of the new facade surface
 * added for the TLS-client example (issue #261):
 *
 *  - ``ra8_tls_mss_clamp`` -- the IPv4 + TCP overhead subtraction that keeps
 *    every segment inside the #21-pinned 128-byte MTU, including its
 *    two-condition rejection decision (MC/DC).
 *  - ``ra8_tls_get_cipher_suite`` -- the negotiated-cipher accessor, its
 *    three-condition argument guard (MC/DC), and the bounded name copy
 *    (truncating vs. naturally terminating -- the loop-guard MC/DC pair).
 *  - ``ra8_tls_get_verify_result`` -- the verification-flag accessor.
 *  - A genuine end-to-end path that binds the ``ra8_tls`` BIO callbacks to
 *    the ``ra8_net_pal`` frame ring (a loopback on the host build), so both
 *    ``ra8_tls.h`` and ``ra8_net_pal.h`` are exercised together exactly as
 *    the firmware example wires them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_net_pal.h"
#include "ra8_tls.h"
#include "unity_minimal.h"

/**
 * @enum t_tls_probe_t
 * @brief Connection ids and out-parameter seeds for the network shim.
 *
 * @details
 * The `k_t_*_unset` values are pre-set into out-parameters before each call: a
 * shim that returns without writing one leaves the seed behind, which the
 * assertion then catches instead of accepting a plausible zero.
 */
typedef enum : uint32_t {
  k_t_conn_id_a   = 7U,      /**< Connection id for the first arm.            */
  k_t_conn_id_b   = 9U,      /**< A different id for the second, also used as
                                  its flags word.                              */
  k_t_flags_unset = 0xDEADU, /**< Pre-set socket flags.                       */
  k_t_u16_unset   = 0xFFFFU, /**< Pre-set 16-bit out-parameter: the MSS, and
                                  the connection id where one is expected.      */
} t_tls_probe_t;

/* =============================================================================
 * BIO adapter: bind ra8_tls to the ra8_net_pal frame ring (host loopback)
 * =============================================================================
 */

/** @brief Sizing constants for the net_pal-backed BIO adapter. */
typedef enum : uint16_t {
  k_np_bio_reasm_cap = 512U, /**< Reassembly buffer for inbound frames. */
} np_bio_dim_t;

/**
 * @struct np_bio_t
 * @brief Per-session BIO context bridging ra8_tls to ra8_net_pal frames.
 *
 * @details
 * ``send`` fragments the ciphertext into MTU-sized frames pushed with
 * ``ra8_net_pal_send_frame``; ``recv`` pulls one frame at a time with
 * ``ra8_net_pal_recv_frame`` and hands the bytes back through a small
 * reassembly window so a partial read leaves the remainder buffered.
 */
typedef struct {
  uint8_t  scratch[k_ra8_net_pal_frame_max]; /**< One-frame receive scratch. */
  uint8_t  reasm[k_np_bio_reasm_cap];        /**< Reassembled inbound bytes. */
  uint16_t reasm_len;                        /**< Valid bytes in ``reasm``.  */
  uint16_t reasm_pos;                        /**< Next unread reasm offset.  */
  uint16_t mtu;                              /**< Per-frame clamp in bytes.  */
} np_bio_t;

/** @brief Shared adapter context for the loopback tests. */
static np_bio_t s_np_bio;

/**
 * @brief BIO send: fragment ciphertext into <= MTU ra8_net_pal frames.
 */
static int np_bio_send(void* ctx, const uint8_t* buf, size_t len)
{
  np_bio_t* b   = (np_bio_t*)ctx;
  size_t    off = 0U;
  /* Bounded by ``len``: each iteration consumes at least one byte. */
  while (off < len) {
    size_t   remain = len - off;
    uint16_t chunk  = (remain > (size_t)b->mtu) ? b->mtu : (uint16_t)remain;
    if (ra8_net_pal_send_frame(&buf[off], chunk) != k_ra8_ok) {
      break; /* Ring full: report the partial count already accepted. */
    }
    off += (size_t)chunk;
  }
  return (int)off;
}

/**
 * @brief BIO recv: drain one ra8_net_pal frame, buffering the remainder.
 */
static int np_bio_recv(void* ctx, uint8_t* buf, size_t len)
{
  np_bio_t* b = (np_bio_t*)ctx;
  if (b->reasm_pos >= b->reasm_len) {
    uint16_t cap  = (uint16_t)k_ra8_net_pal_frame_max;
    b->reasm_len  = 0U;
    b->reasm_pos  = 0U;
    ra8_err_t err = ra8_net_pal_recv_frame(b->scratch, &cap);
    if (err != k_ra8_ok) {
      return 0; /* No frame ready -- loopback drained. */
    }
    (void)memcpy(b->reasm, b->scratch, (size_t)cap);
    b->reasm_len = cap;
  }
  size_t avail = (size_t)(b->reasm_len - b->reasm_pos);
  size_t n     = (len < avail) ? len : avail;
  (void)memcpy(buf, &b->reasm[b->reasm_pos], n);
  b->reasm_pos = (uint16_t)(b->reasm_pos + (uint16_t)n);
  return (int)n;
}

/** @brief MAC used by the loopback bring-up. */
static const ra8_net_pal_mac_t k_np_bio_mac = {.bytes = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U}};

/**
 * @brief Reset the adapter and build a session config wired to it.
 */
static ra8_tls_session_cfg_t make_np_cfg(void)
{
  (void)memset(&s_np_bio, 0, sizeof(s_np_bio));
  s_np_bio.mtu = (uint16_t)k_ra8_tls_mtu_min;

  ra8_tls_session_cfg_t cfg = {};
  cfg.bio_send              = np_bio_send;
  cfg.bio_recv              = np_bio_recv;
  cfg.bio_ctx               = &s_np_bio;
  cfg.server_name           = "endpoint.test";
  cfg.verify_mode           = k_ra8_tls_verify_optional;
  return cfg;
}

/* =============================================================================
 * ra8_tls_mss_clamp
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- asserts the arithmetic contract
 * of ra8_tls_mss_clamp at documented points; the decision itself is
 * covered by test_mcdc_mss_clamp)
 */
static void test_mss_clamp_values(void)
{
  TEST_BEGIN("mss_clamp: 128-byte MTU yields MSS 88");
  uint16_t mss = k_t_u16_unset;
  /* 128 - 20 (IPv4) - 20 (TCP) == 88. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_mss_clamp((uint16_t)k_ra8_tls_mtu_min, &mss));
  TEST_ASSERT_EQ(88, mss);

  /* NULL out pointer rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_mss_clamp((uint16_t)k_ra8_tls_mtu_min, nullptr));

  /* A larger MTU widens the MSS by the same overhead. */
  mss = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_mss_clamp(1500U, &mss));
  TEST_ASSERT_EQ(1460, mss);
  TEST_END("mss_clamp: 128-byte MTU yields MSS 88");
}

/**
 * @test test_mcdc_mss_clamp
 *
 * @par MC/DC:
 * Decision: ``if ((mtu <= overhead) || ((mtu - overhead) < mss_min))`` in
 * ``ra8_tls_mss_clamp`` (libs/ra8_tls/src/ra8_tls.c), overhead == 40,
 * mss_min == 64. 2 conditions, N+1 = 3 vectors.
 * - V1: mtu=128 -> C1=F (128>40), C2=F (88>=64) -> dec F -> k_ra8_ok, mss=88
 * - V2: mtu=40  -> C1=T (40<=40) short-circuits -> dec T -> invalid_arg
 * - V3: mtu=100 -> C1=F (100>40), C2=T (60<64)  -> dec T -> invalid_arg
 * (V1,V2) flips C1 with C2 masked; (V1,V3) flips C2 with C1=F fixed.
 */
static void test_mcdc_mss_clamp(void)
{
  TEST_BEGIN("mss_clamp MC/DC: (mtu<=overhead) || ((mtu-overhead)<mss_min)");
  uint16_t mss = k_t_u16_unset;
  /* V1: both conditions false -> success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_mss_clamp(128U, &mss));
  TEST_ASSERT_EQ(88, mss);
  /* V2: C1 true (mtu == overhead) -> invalid, out zeroed. */
  mss = k_t_u16_unset;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_mss_clamp(40U, &mss));
  TEST_ASSERT_EQ(0, mss);
  /* V3: C1 false, C2 true (segment below mss_min) -> invalid, out zeroed. */
  mss = k_t_u16_unset;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_mss_clamp(100U, &mss));
  TEST_ASSERT_EQ(0, mss);
  TEST_END("mss_clamp MC/DC: (mtu<=overhead) || ((mtu-overhead)<mss_min)");
}

/* =============================================================================
 * ra8_tls_get_cipher_suite / ra8_tls_get_verify_result
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- happy-path introspection contract;
 * the argument guard's decision is covered by test_mcdc_cipher_arg_guard)
 */
static void test_cipher_and_verify_sim(void)
{
  TEST_BEGIN("get_cipher_suite/get_verify_result report the sim sentinels");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = make_np_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_np_bio_mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_handshake(s));

  uint16_t id                              = k_t_u16_unset;
  char     name[k_ra8_tls_cipher_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_cipher_suite(s, &id, name, sizeof(name)));
  TEST_ASSERT_EQ(0, id);
  TEST_ASSERT_EQ(0, strcmp(name, "sim-tls-loopback"));

  uint32_t flags = k_t_flags_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_verify_result(s, &flags));
  TEST_ASSERT_EQ(0, flags);

  /* get_verify_result rejects a NULL out pointer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_verify_result(s, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("get_cipher_suite/get_verify_result report the sim sentinels");
}

/**
 * @test test_mcdc_cipher_arg_guard
 *
 * @par MC/DC:
 * Decision: ``if ((out_id == NULL) || (out_name == NULL) || (name_cap == 0))``
 * in ``ra8_tls_get_cipher_suite`` (libs/ra8_tls/src/ra8_tls.c). 3 conditions,
 * N+1 = 4 vectors.
 * - V1: out_id=ok, out_name=ok, cap=nz -> C1=F,C2=F,C3=F -> dec F -> k_ra8_ok
 * - V2: out_id=NULL                    -> C1=T short-circuits -> invalid_arg
 * - V3: out_id=ok, out_name=NULL       -> C1=F,C2=T short-circuits -> invalid_arg
 * - V4: out_id=ok, out_name=ok, cap=0  -> C1=F,C2=F,C3=T -> invalid_arg
 * (V1,V2) isolates C1; (V1,V3) isolates C2; (V1,V4) isolates C3.
 */
static void test_mcdc_cipher_arg_guard(void)
{
  TEST_BEGIN("get_cipher_suite MC/DC: (out_id||out_name||cap) NULL/zero guard");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = make_np_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_np_bio_mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));

  uint16_t id                              = k_t_conn_id_a;
  char     name[k_ra8_tls_cipher_name_cap] = {};
  /* V1: all valid -> success (session is open, no handshake needed for id 0). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_cipher_suite(s, &id, name, sizeof(name)));
  /* V2: out_id NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_cipher_suite(s, nullptr, name, sizeof(name)));
  /* V3: out_name NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_cipher_suite(s, &id, nullptr, sizeof(name)));
  /* V4: name_cap zero. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_cipher_suite(s, &id, name, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("get_cipher_suite MC/DC: (out_id||out_name||cap) NULL/zero guard");
}

/**
 * @test test_mcdc_cipher_name_copy
 *
 * @par MC/DC:
 * Decision: the ``while ((i < last) && (src[i] != '\0'))`` copy loop in
 * ``internal_copy_cstr`` (libs/ra8_tls/src/ra8_tls.c), exercised through
 * ``ra8_tls_get_cipher_suite`` with the 16-char sim name "sim-tls-loopback".
 * 2 conditions, N+1 = 3 vectors.
 * - V1: mid-copy, i<last AND src[i]!=0 -> C1=T,C2=T -> body runs (both cases).
 * - V2: cap=4 -> i reaches last(=3) before NUL -> C1=F -> truncated to "sim".
 * - V3: cap=48 -> src[i]==NUL before last -> C2=F -> full "sim-tls-loopback".
 * (V1,V2) isolates C1 (loop-bound stop); (V1,V3) isolates C2 (NUL stop).
 */
static void test_mcdc_cipher_name_copy(void)
{
  TEST_BEGIN("get_cipher_suite MC/DC: bounded name copy truncate vs terminate");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = make_np_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_np_bio_mac));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_handshake(s));

  uint16_t id = 0U;
  /* V2 (C1 false: loop-bound truncation): cap 4 keeps 3 chars + NUL. */
  char small[4] = {'x', 'x', 'x', 'x'};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_cipher_suite(s, &id, small, sizeof(small)));
  TEST_ASSERT_EQ(0, strcmp(small, "sim"));
  /* V3 (C2 false: NUL termination): full name fits. */
  char full[k_ra8_tls_cipher_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_cipher_suite(s, &id, full, sizeof(full)));
  TEST_ASSERT_EQ(0, strcmp(full, "sim-tls-loopback"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("get_cipher_suite MC/DC: bounded name copy truncate vs terminate");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the module-state and
 * bogus-handle guards of the two accessors, which are single-condition
 * checks; the compound argument guard is covered by test_mcdc_cipher_arg_guard)
 */
static void test_accessor_state_guards(void)
{
  TEST_BEGIN("cipher/verify accessors reject uninit + bogus handle");
  uint16_t id                              = k_t_conn_id_b;
  char     name[k_ra8_tls_cipher_name_cap] = {};
  uint32_t flags                           = k_t_conn_id_b;

  /* Not initialized: valid out args, session ignored -> not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_tls_get_cipher_suite(nullptr, &id, name, sizeof(name)));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_tls_get_verify_result(nullptr, &flags));

  /* Initialized + a pointer that is not inside the pool -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  uint8_t           bogus_storage[16] = {};
  ra8_tls_session_t bogus             = (ra8_tls_session_t)bogus_storage;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_cipher_suite(bogus, &id, name, sizeof(name)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_get_verify_result(bogus, &flags));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("cipher/verify accessors reject uninit + bogus handle");
}

/* =============================================================================
 * End-to-end: TLS session driven over the ra8_net_pal frame ring
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the public ra8_tls +
 * ra8_net_pal contract end-to-end over the loopback frame ring; the
 * decisions inside the code under test are covered by the dedicated
 * MC/DC cases above)
 */
static void test_end_to_end_over_net_pal(void)
{
  TEST_BEGIN("tls handshake + one record over the ra8_net_pal frame ring");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_np_bio_mac));

  /* The PAL echoes back what it was handed (host loopback ring). */
  ra8_net_pal_link_state_t link = k_ra8_net_pal_link_up;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_link_status(&link));
  TEST_ASSERT_EQ(k_ra8_net_pal_link_down, link);

  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = make_np_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  TEST_ASSERT_NOT_NULL(s);

  /* Handshake drives one byte out through np_bio_send (a net_pal frame) and
   * reads it back through np_bio_recv (the same ring). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_handshake(s));

  /* Exchange one application record: the payload is fragmented into <= MTU
   * net_pal frames on send and reassembled on recv. */
  const uint8_t record[] = {'G', 'E', 'T', ' ', '/'};
  size_t        sent     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_send(s, record, sizeof(record), &sent));
  TEST_ASSERT_EQ(sizeof(record), sent);

  uint8_t got[16]  = {};
  size_t  received = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_recv(s, got, sizeof(got), &received));
  TEST_ASSERT_EQ(sizeof(record), received);
  TEST_ASSERT_EQ(0, memcmp(got, record, sizeof(record)));

  /* Report cipher + verify exactly as the firmware example prints them. */
  uint16_t id                              = k_t_u16_unset;
  char     name[k_ra8_tls_cipher_name_cap] = {};
  uint32_t flags                           = k_t_u16_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_cipher_suite(s, &id, name, sizeof(name)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_get_verify_result(s, &flags));
  TEST_ASSERT_EQ(0, strcmp(name, "sim-tls-loopback"));
  TEST_ASSERT_EQ(0, flags);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("tls handshake + one record over the ra8_net_pal frame ring");
}

int main(void)
{
  test_mss_clamp_values();
  test_mcdc_mss_clamp();
  test_cipher_and_verify_sim();
  test_mcdc_cipher_arg_guard();
  test_mcdc_cipher_name_copy();
  test_accessor_state_guards();
  test_end_to_end_over_net_pal();
  return 0;
}
