/**
 * @file test_ra8_tls.c
 * @brief Unit tests for the ``ra8_tls`` Mbed TLS facade.
 *
 * @details
 * Compiled with ``RA8_OFF_TARGET`` defined, so ``ra8_tls.c``
 * substitutes its loopback BIO drain for every ``mbedtls_ssl_*`` call.
 * That keeps the host test build free of any hard dependency on the
 * heavy Mbed TLS object library while still exercising:
 *
 *  - global init / deinit lifecycle (single-shot guard, deinit-without-init).
 *  - ``ra8_tls_session_open`` argument validation paths.
 *  - Session pool exhaustion (open more than ``k_ra8_tls_max_sessions``).
 *  - ``ra8_tls_session_close`` rejects NULL and non-pool pointers.
 *  - End-to-end loopback: handshake + send + recv with a tiny in-memory
 *    BIO pair so the function-pointer plumbing is fully covered.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_tls.h"
#include "unity_minimal.h"

/**
 * @enum t_tls_t
 * @brief Byte-count seed and the below-range session handle.
 */
typedef enum : uint32_t {
  k_t_count_unset  = 99U,    /**< Pre-set sent/received count; a transfer that
                                  fails must leave it rather than report zero.   */
  k_t_handle_below = 0x100U, /**< A pointer-shaped value below every real
                                  session, which the handle guard must reject.   */
} t_tls_t;

/* =============================================================================
 * Loopback BIO pair
 * =============================================================================
 */

/** @brief Capacity (bytes) of the loopback BIO ring buffer. */
typedef enum : uint16_t {
  k_loop_buf_capacity = 256U, /**< Loop buffer capacity. */
} loop_buf_capacity_t;

/**
 * @struct loop_bio_t
 * @brief Tiny single-producer/single-consumer byte ring used by tests.
 */
typedef struct {
  uint8_t  buf[k_loop_buf_capacity]; /**< Backing storage.          */
  uint16_t head;                     /**< Producer index.           */
  uint16_t tail;                     /**< Consumer index.           */
  uint16_t count;                    /**< Bytes currently buffered. */
} loop_bio_t;

/** @brief Per-test loopback context shared between send and recv callbacks. */
static loop_bio_t s_loop;

#include "support/ra8_tls_test_contracts.h"

/**
 * @brief Reset the loopback ring to an empty state.
 * @copydoc internal_loop_reset
 */
RA8_INTERNAL static void internal_loop_reset(void)
{
  (void)memset(&s_loop, 0, sizeof(s_loop));
}

/**
 * @brief BIO send callback that buffers ciphertext in ``s_loop``.
 * @copydoc internal_loop_bio_send
 */
RA8_INTERNAL static int internal_loop_bio_send(void* ctx, const uint8_t* buf, size_t len)
{
  (void)ctx;
  loop_bio_t* loop    = &s_loop;
  size_t      written = 0U;
  while ((written < len) && (loop->count < (uint16_t)k_loop_buf_capacity)) {
    loop->buf[loop->head] = buf[written];
    loop->head            = (uint16_t)((loop->head + 1U) % (uint16_t)k_loop_buf_capacity);
    loop->count++;
    written++;
  }
  return (int)written;
}

/**
 * @brief BIO recv callback that drains ciphertext from ``s_loop``.
 * @copydoc internal_loop_bio_recv
 */
RA8_INTERNAL static int internal_loop_bio_recv(void* ctx, uint8_t* buf, size_t len)
{
  (void)ctx;
  loop_bio_t* loop = &s_loop;
  size_t      read = 0U;
  while ((read < len) && (loop->count > 0U)) {
    buf[read]  = loop->buf[loop->tail];
    loop->tail = (uint16_t)((loop->tail + 1U) % (uint16_t)k_loop_buf_capacity);
    loop->count--;
    read++;
  }
  return (int)read;
}

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Build a valid session config wired to the loopback BIO pair.
 * @copydoc internal_make_loopback_cfg
 */
RA8_INTERNAL static ra8_tls_session_cfg_t internal_make_loopback_cfg(void)
{
  ra8_tls_session_cfg_t cfg = {};
  cfg.bio_send              = internal_loop_bio_send;
  cfg.bio_recv              = internal_loop_bio_recv;
  cfg.bio_ctx               = &s_loop;
  cfg.server_name           = "test.local";
  return cfg;
}

/* =============================================================================
 * Tests
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

/** @copydoc internal_test_global_init_idempotent_failure */
RA8_INTERNAL static void internal_test_global_init_idempotent_failure(void)
{
  TEST_BEGIN("global_init refuses double-init");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_tls_global_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("global_init refuses double-init");
}

/**
 * @copydoc internal_test_global_deinit_without_init
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_global_deinit_without_init(void)
{
  TEST_BEGIN("global_deinit without init returns not_initialized");
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_tls_global_deinit());
  TEST_END("global_deinit without init returns not_initialized");
}

/**
 * @copydoc internal_test_session_open_invalid_args
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_session_open_invalid_args(void)
{
  TEST_BEGIN("session_open argument validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  ra8_tls_session_t     s   = (ra8_tls_session_t)0x1U;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();

  /* NULL out_session pointer rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(nullptr, &cfg));

  /* NULL cfg rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(&s, nullptr));
  TEST_ASSERT_NULL(s);

  /* NULL bio_send rejected. */
  ra8_tls_session_cfg_t bad = internal_make_loopback_cfg();
  bad.bio_send              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(&s, &bad));
  TEST_ASSERT_NULL(s);

  /* NULL bio_recv rejected. */
  bad          = internal_make_loopback_cfg();
  bad.bio_recv = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(&s, &bad));
  TEST_ASSERT_NULL(s);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("session_open argument validation");
}

/**
 * @copydoc internal_test_session_open_before_init
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_session_open_before_init(void)
{
  TEST_BEGIN("session_open before global_init returns not_initialized");
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_tls_session_open(&s, &cfg));
  TEST_ASSERT_NULL(s);
  TEST_END("session_open before global_init returns not_initialized");
}

/**
 * @copydoc internal_test_session_pool_exhaustion
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_session_pool_exhaustion(void)
{
  TEST_BEGIN("session pool exhaustion returns no_mem");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  ra8_tls_session_cfg_t cfg                              = internal_make_loopback_cfg();
  ra8_tls_session_t     sessions[k_ra8_tls_max_sessions] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_tls_max_sessions; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&sessions[i], &cfg));
    TEST_ASSERT_NOT_NULL(sessions[i]);
  }

  /* One past the cap must fail. */
  ra8_tls_session_t overflow = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_tls_session_open(&overflow, &cfg));
  TEST_ASSERT_NULL(overflow);

  /* Closing one slot frees it for the next caller. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(sessions[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&overflow, &cfg));
  TEST_ASSERT_NOT_NULL(overflow);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(overflow));
  for (uint8_t i = 1U; i < (uint8_t)k_ra8_tls_max_sessions; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(sessions[i]));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("session pool exhaustion returns no_mem");
}

/**
 * @copydoc internal_test_session_close_invalid_handle
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_session_close_invalid_handle(void)
{
  TEST_BEGIN("session_close rejects bogus handles");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  /* NULL handle. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_close(nullptr));

  /* Pointer that is not inside the pool. */
  uint8_t           bogus_storage[16] = {};
  ra8_tls_session_t bogus             = (ra8_tls_session_t)bogus_storage;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_close(bogus));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("session_close rejects bogus handles");
}

/**
 * @copydoc internal_test_loopback_handshake_and_io
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_loopback_handshake_and_io(void)
{
  TEST_BEGIN("loopback handshake + send + recv round-trip");
  internal_loop_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  TEST_ASSERT_NOT_NULL(s);

  /* Fake handshake drives one byte through the BIO pair. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_handshake(s));

  /* Send returns the count buffered by internal_loop_bio_send. */
  const uint8_t payload[] = {'h', 'i'};
  size_t        sent      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_send(s, payload, sizeof(payload), &sent));
  TEST_ASSERT_EQ(sizeof(payload), sent);

  /* Recv reads the bytes back out of the loopback ring. */
  uint8_t recv_buf[8] = {};
  size_t  received    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_recv(s, recv_buf, sizeof(recv_buf), &received));
  TEST_ASSERT_EQ(sizeof(payload), received);
  TEST_ASSERT(recv_buf[0] == 'h');
  TEST_ASSERT(recv_buf[1] == 'i');

  /* Zero-length transfers are no-ops with success. */
  sent = k_t_count_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_send(s, payload, 0U, &sent));
  TEST_ASSERT_EQ(0, sent);

  received = k_t_count_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_recv(s, recv_buf, 0U, &received));
  TEST_ASSERT_EQ(0, received);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("loopback handshake + send + recv round-trip");
}

/**
 * @copydoc internal_test_io_arg_validation
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_io_arg_validation(void)
{
  TEST_BEGIN("send/recv argument validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));

  uint8_t buf[4] = {};
  size_t  n      = 0U;

  /* NULL out_sent / out_received rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_send(s, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_recv(s, buf, sizeof(buf), nullptr));

  /* NULL handle rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_send(nullptr, buf, sizeof(buf), &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_recv(nullptr, buf, sizeof(buf), &n));

  /* NULL buf with non-zero len rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_send(s, nullptr, 4U, &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_recv(s, nullptr, 4U, &n));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("send/recv argument validation");
}

/**
 * @copydoc internal_test_mcdc_tls
 * @test internal_test_mcdc_tls
 *
 * @par MC/DC:
 * Two 2-condition decisions in libs/ra8_tls/src/ra8_tls.c:
 *
 * Decision A (line 217, ``ra8_tls_session_open``):
 * ``if ((cfg->bio_send == NULL) || (cfg->bio_recv == NULL))``
 * - V1: send=valid, recv=valid -> C1=F,C2=F -> dec F (open ok)
 * - V2: send=NULL, recv=valid  -> C1=T (short-circuits) -> dec T (invalid_arg)
 * - V3: send=valid, recv=NULL  -> C1=F,C2=T -> dec T (invalid_arg)
 * Pairs (V1,V2) flip C1 with C2 fixed; (V1,V3) flip C2 with C1 fixed.
 *
 * Decision B (line 341, ``ra8_tls_send``):
 * ``if ((buf == NULL) && (len > 0U))``
 * - V1: buf=NULL, len=0       -> C1=T,C2=F -> dec F (returns ok 0-byte)
 * - V2: buf=valid, len=0      -> C1=F (short-circuits) -> dec F
 * - V3: buf=NULL, len=4       -> C1=T,C2=T -> dec T -> invalid_arg
 * Pairs (V2,V3) flip C1 with C2=T fixed; (V1,V3) flip C2 with C1=T fixed.
 */
RA8_INTERNAL static void internal_test_mcdc_tls(void)
{
  TEST_BEGIN("tls MC/DC: session_open BIO + send NULL/len decisions");
  internal_loop_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());

  /* Decision A vectors. */
  ra8_tls_session_t     s  = nullptr;
  ra8_tls_session_cfg_t v1 = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &v1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));

  s                        = nullptr;
  ra8_tls_session_cfg_t v2 = internal_make_loopback_cfg();
  v2.bio_send              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(&s, &v2));

  s                        = nullptr;
  ra8_tls_session_cfg_t v3 = internal_make_loopback_cfg();
  v3.bio_recv              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_open(&s, &v3));

  /* Decision B vectors. Need a live session. */
  ra8_tls_session_cfg_t cfg_io = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg_io));
  uint8_t buf[4] = {0U};
  size_t  sent   = 0U;
  /* V1: buf=NULL, len=0 -> C1=T,C2=F -> dec F. Implementation accepts. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_send(s, nullptr, 0U, &sent));
  /* V2: buf=valid, len=0 -> C1=F short-circuits -> dec F. */
  sent = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_send(s, buf, 0U, &sent));
  /* V3: buf=NULL, len>0 -> C1=T,C2=T -> dec T -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_send(s, nullptr, 4U, &sent));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("tls MC/DC: session_open BIO + send NULL/len decisions");
}

/**
 * @copydoc internal_test_mcdc_tls_recv_buf_len
 * @test internal_test_mcdc_tls_recv_buf_len
 *
 * @par MC/DC:
 * Decision: ``if ((buf == NULL) && (len > 0U))`` in ``ra8_tls_recv``
 * (libs/ra8_tls/src/ra8_tls.c). 2 conditions, N+1 = 3 vectors.
 * - V1: buf=NULL,  len=0 -> C1=T, C2=F -> dec F (early ok return on len==0)
 * - V2: buf=valid, len=0 -> C1=F short-circuits -> dec F (early ok)
 * - V3: buf=NULL,  len>0 -> C1=T, C2=T -> dec T -> invalid_arg
 * (V2,V3) flips C1 with C2=T fixed; (V1,V3) flips C2 with C1=T fixed.
 */
RA8_INTERNAL static void internal_test_mcdc_tls_recv_buf_len(void)
{
  TEST_BEGIN("tls MC/DC: recv buf == NULL && len > 0");
  internal_loop_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  uint8_t buf[4] = {0U};
  size_t  got    = 0U;
  /* V1: buf=NULL, len=0 -> dec F, len-zero short-circuits to ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_recv(s, nullptr, 0U, &got));
  /* V2: buf=valid, len=0 -> C1=F short-circuits, dec F, len-zero short-circuits to ok. */
  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_recv(s, buf, 0U, &got));
  /* V3: buf=NULL, len=4 -> dec T -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_recv(s, nullptr, 4U, &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("tls MC/DC: recv buf == NULL && len > 0");
}

/**
 * @copydoc internal_test_mcdc_tls_handle_valid_bounds
 * @test internal_test_mcdc_tls_handle_valid_bounds
 *
 * @par MC/DC:
 * Decision: ``if ((session < base) || (session >= end))`` in
 * ``internal_handle_valid`` (libs/ra8_tls/src/ra8_tls.c). 2 conditions, N+1=3.
 * Exercised through any session-using public API (we use ra8_tls_session_close
 * here -- it returns invalid_arg when the handle fails internal_handle_valid).
 * - V1: session inside pool (live)            -> C1=F, C2=F -> dec F (-> ok)
 * - V2: session below base (e.g. small ptr)    -> C1=T short-circuits -> invalid_arg
 * - V3: session above end (large ptr)          -> C1=F, C2=T -> invalid_arg
 * (V1,V2) flips C1 with C2 masked; (V1,V3) flips C2 with C1 fixed F.
 */
RA8_INTERNAL static void internal_test_mcdc_tls_handle_valid_bounds(void)
{
  TEST_BEGIN("tls MC/DC: handle_valid (session<base || session>=end)");
  internal_loop_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_init());
  ra8_tls_session_t     s   = nullptr;
  ra8_tls_session_cfg_t cfg = internal_make_loopback_cfg();
  /* V1: open a real session (lives inside the pool). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_open(&s, &cfg));
  /* V2: cast a deliberately-low pointer -- below the pool base. */
  ra8_tls_session_t below = (ra8_tls_session_t)(uintptr_t)k_t_handle_below;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_close(below));
  /* V3: cast a deliberately-high pointer -- above the pool end. */
  ra8_tls_session_t above = (ra8_tls_session_t)(uintptr_t)UINTPTR_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tls_session_close(above));
  /* V1: the live session closes successfully. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_session_close(s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tls_global_deinit());
  TEST_END("tls MC/DC: handle_valid (session<base || session>=end)");
}

int main(void)
{
  internal_test_global_deinit_without_init();
  internal_test_global_init_idempotent_failure();
  internal_test_session_open_before_init();
  internal_test_session_open_invalid_args();
  internal_test_session_pool_exhaustion();
  internal_test_session_close_invalid_handle();
  internal_test_loopback_handshake_and_io();
  internal_test_io_arg_validation();
  internal_test_mcdc_tls();
  internal_test_mcdc_tls_recv_buf_len();
  internal_test_mcdc_tls_handle_valid_bounds();
  return 0;
}
