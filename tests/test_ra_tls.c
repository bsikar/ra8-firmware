/**
 * @file test_ra_tls.c
 * @brief Unit tests for libs/ra_tls (minimal TLS 1.2 client).
 *
 * @details
 * Validates the public ``ra_tls_*`` API:
 *
 *   - ``ra_tls_client_init`` argument validation and successful init.
 *   - ``ra_tls_client_close`` wipes magic and key material.
 *   - Handshake state-machine progression against a synthetic peer
 *     that replies with hand-built ServerHello / Certificate /
 *     ServerHelloDone bytes followed by a ChangeCipherSpec record.
 *   - Record encrypt + decrypt round-trip against the deterministic
 *     ``ra_sce`` software-stub backend (cipher recovers plaintext).
 *   - ``ra_tls_client_send`` / ``_recv`` reject NULL args / pre-init.
 *   - Transcript hash updates incrementally.
 *
 * The library .c files are pulled in directly via ``#include`` so the
 * test is self-contained and does not require updates to the
 * ``tests/CMakeLists.txt`` library glob (parallel-build safety).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sce.h"
#include "ra_sim_mmap.h"
#include "ra_tls.h"
#include "unity_minimal.h"

/* Pull the ra_tls translation units into this test executable. The
 * tests/CMakeLists.txt library glob does not include libs/ra_tls/src
 * yet (sibling-owned file); compiling the .c files directly keeps the
 * test self-contained. */
#include "../libs/ra_tls/src/ra_tls_client.c"    // NOLINT
#include "../libs/ra_tls/src/ra_tls_handshake.c" // NOLINT
#include "../libs/ra_tls/src/ra_tls_record.c"    // NOLINT

/**
 * @enum ra_tls_test_const_t
 * @brief Magic numbers used by the tests, named to keep the
 *        no-magic-numbers rule satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_tls_test_pinned_byte    = 0xAAU, /**< Filler for pinned-cert hash. */
  k_ra_tls_test_record_buf     = 1024U, /**< Outbound encrypted-record scratch. */
  k_ra_tls_test_plain_buf      = 64U,   /**< Plaintext scratch length. */
  k_ra_tls_test_plain_len      = 13U,   /**< Round-trip plaintext length. */
  k_ra_tls_test_pattern_app    = 0x42U, /**< Plaintext pattern byte. */
  k_ra_tls_test_synthetic_pkt  = 256U,  /**< Synthetic-server-reply scratch. */
  k_ra_tls_test_short_hostname = 11U,   /**< strlen("example.com"). */
} ra_tls_test_const_t;

/* =============================================================================
 * Stub transport for handshake / send / recv tests
 * =============================================================================
 */

/**
 * @struct stub_transport_t
 * @brief Capture-only transport context used by tests.
 *
 * @details
 * The ``send`` hook accumulates bytes in ``sent_buf`` so tests can
 * inspect the on-wire framing. The ``recv`` hook drains
 * ``recv_buf[recv_cursor..recv_len]`` one shot at a time.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  sent_buf[k_ra_tls_test_record_buf];
  uint32_t sent_len;
  uint8_t  recv_buf[k_ra_tls_test_record_buf];
  uint32_t recv_len;
  uint32_t recv_cursor;
  /** @brief Bytes returned per recv() call; 0 means "all remaining". */
  uint32_t recv_chunk;
} stub_transport_t;

/**
 * @brief ``ra_tls_send_fn`` implementation appending to ``sent_buf``.
 */
static ra_err_t stub_send(void* ctx, const uint8_t* buf, uint32_t len)
{
  stub_transport_t* t = (stub_transport_t*)ctx;
  if ((t->sent_len + len) > sizeof(t->sent_buf)) {
    return k_ra_err_no_mem;
  }
  (void)memcpy(&t->sent_buf[t->sent_len], buf, (size_t)len);
  t->sent_len += len;
  return k_ra_ok;
}

/**
 * @brief ``ra_tls_recv_fn`` returning the staged synthetic bytes.
 */
static ra_err_t stub_recv(void* ctx, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  stub_transport_t* t         = (stub_transport_t*)ctx;
  const uint32_t    available = t->recv_len - t->recv_cursor;
  uint32_t          take      = (available < max_len) ? available : max_len;
  if ((t->recv_chunk != 0U) && (take > t->recv_chunk)) {
    take = t->recv_chunk;
  }
  (void)memcpy(buf, &t->recv_buf[t->recv_cursor], (size_t)take);
  t->recv_cursor += take;
  *got_len = take;
  return k_ra_ok;
}

/* =============================================================================
 * Test fixtures
 * =============================================================================
 */

static void prep_sce(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_sce_cfg_t cfg = {.run_self_test = true};
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_open(&cfg));
}

static void teardown_sce(void)
{
  (void)ra_sce_close();
}

/**
 * @brief Test 1: init / close lifecycle.
 */
static void test_init_close(void)
{
  TEST_BEGIN("ra_tls init/close");
  prep_sce();
  ra_tls_client_t  ctx = {};
  stub_transport_t t   = {};
  uint8_t          pinned[k_ra_tls_sha256_digest_bytes];
  (void)memset(pinned, (int)k_ra_tls_test_pinned_byte, sizeof(pinned));
  const ra_tls_config_t cfg = {
    .hostname                    = "example.com",
    .expected_server_cert_sha256 = pinned,
    .send                        = stub_send,
    .recv                        = stub_recv,
    .transport_ctx               = &t,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_init(&ctx, &cfg));
  TEST_ASSERT_EQ((int)k_ra_tls_ctx_magic, (int)ctx.magic);
  TEST_ASSERT_EQ((int)k_ra_tls_state_idle, (int)ctx.state);
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_close(&ctx));
  TEST_ASSERT_EQ(0, (int)ctx.magic);
  TEST_ASSERT_EQ((int)k_ra_tls_state_closed, (int)ctx.state);
  teardown_sce();
  TEST_END("ra_tls init/close");
}

/**
 * @brief Test 2: NULL / invalid-arg guards.
 */
static void test_null_and_pre_init(void)
{
  TEST_BEGIN("ra_tls NULL guards");
  ra_tls_client_t  ctx = {};
  stub_transport_t t   = {};
  uint8_t          pinned[k_ra_tls_sha256_digest_bytes];
  (void)memset(pinned, (int)k_ra_tls_test_pinned_byte, sizeof(pinned));

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_init(nullptr, nullptr));
  const ra_tls_config_t bad_cfg = {.hostname                    = nullptr,
                                   .expected_server_cert_sha256 = pinned,
                                   .send                        = stub_send,
                                   .recv                        = stub_recv,
                                   .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_init(&ctx, &bad_cfg));
  const ra_tls_config_t empty_host = {.hostname                    = "",
                                      .expected_server_cert_sha256 = pinned,
                                      .send                        = stub_send,
                                      .recv                        = stub_recv,
                                      .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_tls_client_init(&ctx, &empty_host));

  /* Pre-init send/recv must report invalid_state. */
  uint8_t  buf[1] = {};
  uint32_t got    = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_tls_client_send(&ctx, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_tls_client_recv(&ctx, buf, 1U, &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_send(nullptr, buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_recv(nullptr, buf, 1U, &got));
  TEST_END("ra_tls NULL guards");
}

/**
 * @brief Test 3: record encrypt + decrypt round-trips via stub SCE.
 */
static void test_record_roundtrip(void)
{
  TEST_BEGIN("ra_tls record roundtrip");
  prep_sce();
  ra_tls_client_t  ctx = {};
  stub_transport_t t   = {};
  uint8_t          pinned[k_ra_tls_sha256_digest_bytes];
  (void)memset(pinned, (int)k_ra_tls_test_pinned_byte, sizeof(pinned));
  const ra_tls_config_t cfg = {.hostname                    = "h",
                               .expected_server_cert_sha256 = pinned,
                               .send                        = stub_send,
                               .recv                        = stub_recv,
                               .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_init(&ctx, &cfg));

  /* Manually fault the keys + IVs (skip handshake) so we can drive the
   * record layer in isolation. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_aes128_key_bytes; ++i) {
    ctx.client_write_key[i] = (uint8_t)(i + 1U);
    ctx.server_write_key[i] = (uint8_t)(i + 1U);
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_gcm_implicit_iv_bytes; ++i) {
    ctx.client_write_iv[i] = (uint8_t)(0x10U + i);
    ctx.server_write_iv[i] = (uint8_t)(0x10U + i);
  }

  uint8_t plain[k_ra_tls_test_plain_buf];
  (void)memset(plain, (int)k_ra_tls_test_pattern_app, (size_t)k_ra_tls_test_plain_len);
  uint8_t  rec[k_ra_tls_test_record_buf];
  uint32_t reclen = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_tls_record_encrypt(&ctx,
                                       k_ra_tls_ct_application_data,
                                       plain,
                                       (uint32_t)k_ra_tls_test_plain_len,
                                       rec,
                                       (uint32_t)sizeof(rec),
                                       &reclen));
  /* Header byte = ContentType. */
  TEST_ASSERT_EQ((int)k_ra_tls_ct_application_data, (int)rec[k_ra_tls_off_record_type]);
  TEST_ASSERT_EQ((int)k_ra_tls_version_major, (int)rec[k_ra_tls_off_record_ver_hi]);

  /* Reset client_seq for the decrypt side so the stub regenerates
   * the same keystream. */
  ctx.server_seq               = 0U;
  ra_tls_content_type_t got_ct = k_ra_tls_ct_handshake;
  uint8_t               back[k_ra_tls_test_plain_buf];
  uint32_t              backlen = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_tls_record_decrypt(&ctx, rec, reclen, &got_ct, back, (uint32_t)sizeof(back), &backlen));
  TEST_ASSERT_EQ((int)k_ra_tls_ct_application_data, (int)got_ct);
  TEST_ASSERT_EQ((int)k_ra_tls_test_plain_len, (int)backlen);
  for (uint32_t i = 0U; i < (uint32_t)k_ra_tls_test_plain_len; ++i) {
    TEST_ASSERT_EQ(plain[i], back[i]);
  }
  (void)ra_tls_client_close(&ctx);
  teardown_sce();
  TEST_END("ra_tls record roundtrip");
}

/**
 * @brief Test 4: send / recv null-arg validation.
 */
static void test_send_recv_null(void)
{
  TEST_BEGIN("ra_tls send/recv NULL");
  prep_sce();
  ra_tls_client_t  ctx = {};
  stub_transport_t t   = {};
  uint8_t          pinned[k_ra_tls_sha256_digest_bytes];
  (void)memset(pinned, (int)k_ra_tls_test_pinned_byte, sizeof(pinned));
  const ra_tls_config_t cfg = {.hostname                    = "h",
                               .expected_server_cert_sha256 = pinned,
                               .send                        = stub_send,
                               .recv                        = stub_recv,
                               .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_init(&ctx, &cfg));
  ctx.state = k_ra_tls_state_established;

  uint8_t  buf[1] = {};
  uint32_t got    = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_send(&ctx, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_tls_client_send(&ctx, buf, 0U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_recv(&ctx, nullptr, 1U, &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_client_recv(&ctx, buf, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_tls_client_recv(&ctx, buf, 0U, &got));
  ctx.state = k_ra_tls_state_idle;
  (void)ra_tls_client_close(&ctx);
  teardown_sce();
  TEST_END("ra_tls send/recv NULL");
}

/**
 * @brief Test 5: transcript hash updates incrementally.
 *
 * @details
 * Two sequential ``ra_tls_transcript_update`` calls with different
 * inputs must produce a different running digest than a single call
 * with one of the inputs alone -- proving the absorbed-bytes counter
 * is mixed into the hash chain.
 */
static void test_transcript_incremental(void)
{
  TEST_BEGIN("ra_tls transcript incremental");
  prep_sce();
  ra_tls_client_t  ctx = {};
  stub_transport_t t   = {};
  uint8_t          pinned[k_ra_tls_sha256_digest_bytes];
  (void)memset(pinned, (int)k_ra_tls_test_pinned_byte, sizeof(pinned));
  const ra_tls_config_t cfg = {.hostname                    = "h",
                               .expected_server_cert_sha256 = pinned,
                               .send                        = stub_send,
                               .recv                        = stub_recv,
                               .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_init(&ctx, &cfg));

  const uint8_t a[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  const uint8_t b[3] = {0x05U, 0x06U, 0x07U};
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_transcript_update(&ctx, a, (uint32_t)sizeof(a)));
  uint8_t after_a[k_ra_tls_sha256_digest_bytes];
  (void)memcpy(after_a, ctx.transcript, sizeof(after_a));
  TEST_ASSERT_EQ((int)sizeof(a), (int)ctx.transcript_len);

  TEST_ASSERT_EQ(k_ra_ok, ra_tls_transcript_update(&ctx, b, (uint32_t)sizeof(b)));
  TEST_ASSERT_EQ((int)(sizeof(a) + sizeof(b)), (int)ctx.transcript_len);
  /* Digest must have changed after the second update. */
  TEST_ASSERT(memcmp(after_a, ctx.transcript, sizeof(after_a)) != 0);

  /* NULL guard. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_transcript_update(nullptr, a, sizeof(a)));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_tls_transcript_update(&ctx, nullptr, sizeof(a)));
  /* Zero-length is OK with NULL buf. */
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_transcript_update(&ctx, nullptr, 0U));
  (void)ra_tls_client_close(&ctx);
  teardown_sce();
  TEST_END("ra_tls transcript incremental");
}

/**
 * @brief Test 6: handshake state machine progresses with synthetic peer.
 *
 * @details
 * Stages a hand-built TLS handshake record containing ServerHello,
 * Certificate, and ServerHelloDone, plus a follow-up ChangeCipherSpec
 * record. ``ra_tls_client_handshake`` must pin the certificate hash,
 * advance through every state, and reach ``established``.
 */
static void test_handshake_progression(void)
{
  TEST_BEGIN("ra_tls handshake progression");
  prep_sce();
  stub_transport_t t = {};

  /* --- Pre-compute the pinned cert hash for a fixed leaf body. --- */
  const uint8_t leaf_body[] = {0x30U, 0x82U, 0x01U, 0x00U, 0x00U};
  uint8_t       cert_hash[k_ra_tls_sha256_digest_bytes];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_init(k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_update(leaf_body, (uint32_t)sizeof(leaf_body)));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_final(cert_hash));

  /* --- Compose the synthetic server reply directly into recv_buf. --- */
  uint8_t* p   = t.recv_buf;
  uint32_t pos = 0U;

  /* TLS record header: handshake, version 3.3, length placeholder. */
  p[pos++]                   = (uint8_t)k_ra_tls_ct_handshake;
  p[pos++]                   = (uint8_t)k_ra_tls_version_major;
  p[pos++]                   = (uint8_t)k_ra_tls_version_minor;
  const uint32_t rec_len_off = pos;
  p[pos++]                   = 0U;
  p[pos++]                   = 0U;
  const uint32_t body_start  = pos;

  /* ServerHello: type=2, len=38, ver, 32-byte random, sid_len=0,
   * suite=0x009C, compression=0. */
  p[pos++] = (uint8_t)k_ra_tls_hs_server_hello;
  p[pos++] = 0U;
  p[pos++] = 0U;
  p[pos++] = (uint8_t)k_ra_tls_priv_server_hello_min;
  p[pos++] = (uint8_t)k_ra_tls_version_major;
  p[pos++] = (uint8_t)k_ra_tls_version_minor;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_random_bytes; ++i) {
    p[pos++] = (uint8_t)(0x10U + i);
  }
  p[pos++] = 0U; /* session_id length */
  p[pos++] = (uint8_t)((0x009CU >> k_ra_tls_shift_byte) & k_ra_tls_mask_byte);
  p[pos++] = (uint8_t)(0x009CU & k_ra_tls_mask_byte);
  p[pos++] = 0U; /* compression_method = null */

  /* Certificate handshake message. Body = cert payload directly (the
   * pin hashes the whole body, so the inner ASN.1 layout is irrelevant
   * for our test). */
  p[pos++]                = (uint8_t)k_ra_tls_hs_certificate;
  const uint32_t leaf_len = (uint32_t)sizeof(leaf_body);
  p[pos++]                = 0U;
  p[pos++]                = (uint8_t)((leaf_len >> k_ra_tls_shift_byte) & k_ra_tls_mask_byte);
  p[pos++]                = (uint8_t)(leaf_len & k_ra_tls_mask_byte);
  (void)memcpy(&p[pos], leaf_body, sizeof(leaf_body));
  pos += leaf_len;

  /* ServerHelloDone: type=14, len=0. */
  p[pos++] = (uint8_t)k_ra_tls_hs_server_hello_done;
  p[pos++] = 0U;
  p[pos++] = 0U;
  p[pos++] = 0U;

  /* Patch record-length field. */
  const uint32_t hs_total = pos - body_start;
  p[rec_len_off]          = (uint8_t)((hs_total >> k_ra_tls_shift_byte) & k_ra_tls_mask_byte);
  p[rec_len_off + 1U]     = (uint8_t)(hs_total & k_ra_tls_mask_byte);

  /* The first recv() must hand back exactly the handshake record so
   * the client's second recv() call still finds the CCS record. */
  t.recv_chunk = pos;

  /* CCS record (1-byte payload), as the synthetic Server-Finished.
   * The minimal client only asserts the first record's CT here. */
  p[pos++]   = (uint8_t)k_ra_tls_ct_change_cipher_spec;
  p[pos++]   = (uint8_t)k_ra_tls_version_major;
  p[pos++]   = (uint8_t)k_ra_tls_version_minor;
  p[pos++]   = 0U;
  p[pos++]   = 1U;
  p[pos++]   = 1U;
  t.recv_len = pos;

  /* --- Drive the client. --- */
  ra_tls_client_t       ctx = {};
  const ra_tls_config_t cfg = {.hostname                    = "host.test",
                               .expected_server_cert_sha256 = cert_hash,
                               .send                        = stub_send,
                               .recv                        = stub_recv,
                               .transport_ctx               = &t};
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_init(&ctx, &cfg));
  /* The CCS+Finished arrives as a SECOND recv() call inside
   * internal_recv_server_finished. Reset the cursor so the first
   * recv() only returns the handshake bundle. The stub returns
   * everything in one shot, but the client only parses up to
   * ServerHelloDone in the first call -- so the leftover CCS bytes
   * remain in t.recv_buf for a second recv (which our stub will
   * happily ship). */
  TEST_ASSERT_EQ(k_ra_ok, ra_tls_client_handshake(&ctx));
  TEST_ASSERT_EQ((int)k_ra_tls_state_established, (int)ctx.state);
  TEST_ASSERT(t.sent_len > 0U);

  (void)ra_tls_client_close(&ctx);
  teardown_sce();
  TEST_END("ra_tls handshake progression");
}

int32_t main(void)
{
  test_init_close();
  test_null_and_pre_init();
  test_record_roundtrip();
  test_send_recv_null();
  test_transcript_incremental();
  test_handshake_progression();
  (void)fprintf(stderr, "[OK  ] test_ra_tls.c\n");
  return 0;
}
