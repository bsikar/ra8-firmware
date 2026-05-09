/**
 * @file test_ra_psa_crypto.c
 * @brief Unit tests for the ra_psa_crypto facade
 *
 * @details
 * Exercises every public entry point in ``ra_psa_crypto.h`` against
 * the simulator-mode stand-ins defined in ``ra_psa_crypto.c``:
 *
 * - init / deinit symmetry and double-init guard
 * - key import / destroy with the static pool capacity
 * - SHA-256 round-trip against a known FIPS 180-4 test vector
 * - sign + verify round-trip with a deterministic test key
 * - AES-GCM encrypt + decrypt round-trip with tag-tampering detection
 * - error paths: NULL pointers, uninitialised facade, oversized keys,
 *   wrong algorithm tags, undersized output buffers
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_psa_crypto.h"
#include "unity_minimal.h"

/**
 * @enum ra_psa_test_const_t
 * @brief Magic-number-free constants used by the test bodies.
 */
typedef enum : uint16_t {
  k_psa_test_aes_key_len   = 16U,  /**< 128-bit symmetric key.                 */
  k_psa_test_ecdsa_key_len = 32U,  /**< 256-bit "private" key (sim).           */
  k_psa_test_plain_len     = 8U,   /**< Sample plaintext length.               */
  k_psa_test_aad_len       = 4U,   /**< Sample AAD length.                     */
  k_psa_test_oversize_len  = 200U, /**< Larger than k_ra_psa_max_key_bytes.    */
} ra_psa_test_const_t;

/* SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
 * (FIPS 180-4 Appendix B Test 1.) */
static const uint8_t k_sha256_abc_digest[32] = {
  0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU, 0x41U, 0x41U, 0x40U,
  0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U, 0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U,
  0x7aU, 0x9cU, 0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
};

static const uint8_t k_test_aes_key[16] = {
  0x00U,
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
  0x77U,
  0x88U,
  0x99U,
  0xaaU,
  0xbbU,
  0xccU,
  0xddU,
  0xeeU,
  0xffU,
};

static const uint8_t k_test_ecdsa_key[32] = {
  0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1aU,
  0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU, 0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U,
  0x26U, 0x27U, 0x28U, 0x29U, 0x2aU, 0x2bU, 0x2cU, 0x2dU, 0x2eU, 0x2fU,
};

static const uint8_t k_test_nonce[12] = {
  0x01U,
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
  0x08U,
  0x09U,
  0x0aU,
  0x0bU,
  0x0cU,
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void prep_init(void)
{
  /* deinit is allowed to fail with not-initialised on the very first
   * call; subsequent calls observe a properly torn-down module. */
  (void)ra_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_crypto_init());
}

static void teardown(void)
{
  (void)ra_psa_crypto_deinit();
}

/* ------------------------------------------------------------------ */
/* tests                                                              */
/* ------------------------------------------------------------------ */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_double(void)
{
  TEST_BEGIN("psa init double-init guard");
  prep_init();
  TEST_ASSERT_EQ(k_ra_err_exists, ra_psa_crypto_init());
  teardown();
  TEST_END("psa init double-init guard");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_without_init(void)
{
  TEST_BEGIN("psa deinit without init");
  (void)ra_psa_crypto_deinit(); /* drain any leftover state */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_psa_crypto_deinit());
  TEST_END("psa deinit without init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_import_uninitialised(void)
{
  TEST_BEGIN("psa import without init");
  (void)ra_psa_crypto_deinit();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t k = (ra_psa_key_t)0x1U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_NULL(k);
  TEST_END("psa import without init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_import_invalid_args(void)
{
  TEST_BEGIN("psa import invalid args");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = k_ra_psa_usage_encrypt,
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(nullptr, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(&k, nullptr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(&k, &attr, nullptr, (size_t)k_psa_test_aes_key_len));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_import(&k, &attr, k_test_aes_key, 0U));
  /* Oversized key. */
  uint8_t big[k_psa_test_oversize_len];
  (void)memset(big, 0x5aU, sizeof(big));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_psa_key_import(&k, &attr, big, sizeof(big)));
  /* Empty usage rejected. */
  const ra_psa_key_attr_t no_use = {.type  = k_ra_psa_key_type_aes,
                                    .alg   = k_ra_psa_alg_aes_gcm,
                                    .usage = k_ra_psa_usage_none};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(&k, &no_use, k_test_aes_key, sizeof(k_test_aes_key)));
  teardown();
  TEST_END("psa import invalid args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_import_destroy_round_trip(void)
{
  TEST_BEGIN("psa import + destroy round trip");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_NOT_NULL(k);
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_destroy(k));
  /* Destroying twice must fail (slot no longer in_use). */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_destroy(k));
  teardown();
  TEST_END("psa import + destroy round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pool_exhaustion(void)
{
  TEST_BEGIN("psa pool exhaustion");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = k_ra_psa_usage_encrypt,
  };
  ra_psa_key_t handles[k_ra_psa_max_keys];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_psa_max_keys; ++i) {
    handles[i] = nullptr;
    TEST_ASSERT_EQ(k_ra_ok,
                   ra_psa_key_import(&handles[i], &attr, k_test_aes_key, sizeof(k_test_aes_key)));
    TEST_ASSERT_NOT_NULL(handles[i]);
  }
  ra_psa_key_t over = nullptr;
  TEST_ASSERT_EQ(k_ra_err_no_mem,
                 ra_psa_key_import(&over, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_NULL(over);
  /* Free one and reissue. */
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_destroy(handles[0]));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_key_import(&handles[0], &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  for (uint8_t i = 0U; i < (uint8_t)k_ra_psa_max_keys; ++i) {
    (void)ra_psa_key_destroy(handles[i]);
  }
  teardown();
  TEST_END("psa pool exhaustion");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_destroy_invalid_handle(void)
{
  TEST_BEGIN("psa destroy invalid handle");
  prep_init();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_destroy(nullptr));
  /* Pointer that doesn't lie inside the pool. */
  uint8_t            fake  = 0U;
  const ra_psa_key_t bogus = (ra_psa_key_t)&fake;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_destroy(bogus));
  teardown();
  TEST_END("psa destroy invalid handle");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hash_known_vector(void)
{
  TEST_BEGIN("psa sha-256 known answer");
  prep_init();
  const uint8_t input[3] = {'a', 'b', 'c'};
  uint8_t       digest[k_ra_psa_sha256_len];
  size_t        out_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     input,
                                     sizeof(input),
                                     digest,
                                     sizeof(digest),
                                     &out_len));
  TEST_ASSERT_EQ(k_ra_psa_sha256_len, out_len);
  TEST_ASSERT(memcmp(digest, k_sha256_abc_digest, sizeof(digest)) == 0);
  teardown();
  TEST_END("psa sha-256 known answer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hash_invalid_args(void)
{
  TEST_BEGIN("psa sha-256 invalid args");
  prep_init();
  uint8_t out[k_ra_psa_sha256_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256, nullptr, 0U, nullptr, sizeof(out), &ol));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,

    ra_psa_hash_compute(k_ra_psa_alg_aes_gcm, (const uint8_t*)"x", 1U, out, sizeof(out), &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256, (const uint8_t*)"x", 1U, out, 4U, &ol));
  teardown();
  TEST_END("psa sha-256 invalid args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sign_verify_round_trip(void)
{
  TEST_BEGIN("psa sign + verify round trip");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_ecc_p256_priv,
    .alg   = k_ra_psa_alg_ecdsa_sha_256,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_sign | k_ra_psa_usage_verify),
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));

  /* Hash a fixed message and sign the digest. */
  const uint8_t msg[3] = {'a', 'b', 'c'};
  uint8_t       digest[k_ra_psa_sha256_len];
  size_t        digest_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     msg,
                                     sizeof(msg),
                                     digest,
                                     sizeof(digest),
                                     &digest_len));

  uint8_t sig[k_ra_psa_max_sig_bytes];
  size_t  sig_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_sign_hash(k,
                                  k_ra_psa_alg_ecdsa_sha_256,
                                  digest,
                                  digest_len,
                                  sig,
                                  sizeof(sig),
                                  &sig_len));
  TEST_ASSERT(sig_len > 0U);

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_psa_verify_hash(k, k_ra_psa_alg_ecdsa_sha_256, digest, digest_len, sig, sig_len));

  /* Tamper a byte and ensure verification fails. */
  sig[0] ^= 0xFFU;
  TEST_ASSERT_EQ(
    k_ra_err_crc_mismatch,
    ra_psa_verify_hash(k, k_ra_psa_alg_ecdsa_sha_256, digest, digest_len, sig, sig_len));

  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_destroy(k));
  teardown();
  TEST_END("psa sign + verify round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sign_wrong_alg(void)
{
  TEST_BEGIN("psa sign wrong alg");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_ecc_p256_priv,
    .alg   = k_ra_psa_alg_ecdsa_sha_256,
    .usage = k_ra_psa_usage_sign,
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra_psa_sha256_len] = {};
  uint8_t sig[k_ra_psa_max_sig_bytes];
  size_t  sl = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_not_supported,
    ra_psa_sign_hash(k, k_ra_psa_alg_aes_gcm, hash, sizeof(hash), sig, sizeof(sig), &sl));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa sign wrong alg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_aead_round_trip(void)
{
  TEST_BEGIN("psa aead encrypt + decrypt round trip");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));

  const uint8_t plain[k_psa_test_plain_len] = {'h', 'e', 'l', 'l', 'o', '!', '!', '\0'};
  const uint8_t aad[k_psa_test_aad_len]     = {'A', 'A', 'D', '0'};
  uint8_t       ct[k_psa_test_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ct_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ct_len));
  TEST_ASSERT_EQ((sizeof(plain) + (size_t)k_ra_psa_gcm_tag_len), ct_len);

  uint8_t recovered[k_psa_test_plain_len];
  size_t  rec_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     ct,
                                     ct_len,
                                     recovered,
                                     sizeof(recovered),
                                     &rec_len));
  TEST_ASSERT_EQ(sizeof(plain), rec_len);
  TEST_ASSERT(memcmp(recovered, plain, sizeof(plain)) == 0);

  /* Tamper the tag and ensure decrypt detects it. */
  ct[ct_len - 1U] ^= 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_crc_mismatch,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     ct,
                                     ct_len,
                                     recovered,
                                     sizeof(recovered),
                                     &rec_len));

  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead encrypt + decrypt round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_aead_invalid_args(void)
{
  TEST_BEGIN("psa aead invalid args");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));

  uint8_t out[64];
  size_t  ol = 0U;
  /* Wrong nonce length. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     8U,
                                     nullptr,
                                     0U,
                                     (const uint8_t*)"hi",
                                     2U,
                                     out,
                                     sizeof(out),
                                     &ol));
  /* Output too small. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     (const uint8_t*)"hi",
                                     2U,
                                     out,
                                     2U,
                                     &ol));
  /* Decrypt with cipher_len < tag length. */
  uint8_t tiny[8] = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     tiny,
                                     sizeof(tiny),
                                     out,
                                     sizeof(out),
                                     &ol));

  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead invalid args");
}

/* ================================================================== */
/* MC/DC vectors                                                      */
/* ================================================================== */
/*
 * The vectors below close DO-178C Level B / IEC 61508 SIL 3 MC/DC
 * obligations for compound boolean decisions in
 * ``libs/ra_psa_crypto/src/ra_psa_crypto.c``. Each test follows the
 * canonical N+1 pattern (see tests/test_ra_xspi.c::
 * test_set_xip_mode_mcdc_addr_bytes for the reference template).
 */

/**
 * @enum ra_psa_mcdc_const_t
 * @brief Magic-number-free vector inputs for the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_psa_mcdc_short_input  = 1U,
  k_psa_mcdc_short_aad    = 4U,
  k_psa_mcdc_plain_len    = 8U,
  k_psa_mcdc_tiny_len     = 8U,
  k_psa_mcdc_bad_nonce    = 8U,
  k_psa_mcdc_small_outcap = 2U,
} ra_psa_mcdc_const_t;

/** @brief Helper: import a valid AES key with the requested usage mask. */
static ra_psa_key_t mcdc_import_aes_key(ra_psa_key_usage_t usage)
{
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = usage,
  };
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  return k;
}

/**
 * @test test_mcdc_handle_valid_range_check
 * @par MC/DC:
 * Decision: `if ((handle < base) || (handle >= end))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 180,
 * `internal_handle_valid`). Reached via `ra_psa_key_destroy`.
 * V1 real slot: C1=F,C2=F -> F. V2 stack ptr below pool: C1=T -> T.
 * V3 high address: C1=F,C2=T -> T. N+1 = 3 vectors for N=2.
 */
static void test_mcdc_handle_valid_range_check(void)
{
  TEST_BEGIN("psa MC/DC: handle range (handle<base || handle>=end)");
  prep_init();
  ra_psa_key_t real = mcdc_import_aes_key(k_ra_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(real);
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_destroy(real));
  uint8_t            on_stack = 0U;
  const ra_psa_key_t below    = (ra_psa_key_t)&on_stack;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_destroy(below));
  const ra_psa_key_t high = (ra_psa_key_t)(uintptr_t)0x7FFFFFFFFFFFUL;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_destroy(high));
  teardown();
  TEST_END("psa MC/DC: handle range (handle<base || handle>=end)");
}

/**
 * @test test_mcdc_key_import_arg_quad
 * @par MC/DC:
 * Decision: `if ((out_handle == NULL) || (attr == NULL) ||
 *               (data == NULL) || (data_len == 0U))` (4 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 673, `ra_psa_key_import`).
 * V1 all valid -> F. V2 out=NULL -> C1=T. V3 attr=NULL -> C2=T.
 * V4 data=NULL -> C3=T. V5 len=0 -> C4=T. N+1 = 5 vectors for N=4.
 */
static void test_mcdc_key_import_arg_quad(void)
{
  TEST_BEGIN("psa MC/DC: import arg quad (out||attr||data||len)");
  prep_init();
  const ra_psa_key_attr_t attr = {.type  = k_ra_psa_key_type_aes,
                                  .alg   = k_ra_psa_alg_aes_gcm,
                                  .usage = k_ra_psa_usage_encrypt};
  ra_psa_key_t            k    = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_destroy(k));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(nullptr, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(&k, nullptr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_key_import(&k, &attr, nullptr, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_psa_key_import(&k, &attr, k_test_aes_key, 0U));
  teardown();
  TEST_END("psa MC/DC: import arg quad (out||attr||data||len)");
}

/**
 * @test test_mcdc_hash_compute_out_pair
 * @par MC/DC:
 * Decision: `if ((out == NULL) || (out_len == NULL))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 735). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_hash_compute_out_pair(void)
{
  TEST_BEGIN("psa MC/DC: hash_compute (out||out_len) NULL pair");
  prep_init();
  uint8_t       out[k_ra_psa_sha256_len];
  size_t        ol  = 0U;
  const uint8_t inp = (uint8_t)'a';
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     &inp,
                                     (size_t)k_psa_mcdc_short_input,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     &inp,
                                     (size_t)k_psa_mcdc_short_input,
                                     nullptr,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     &inp,
                                     (size_t)k_psa_mcdc_short_input,
                                     out,
                                     sizeof(out),
                                     nullptr));
  teardown();
  TEST_END("psa MC/DC: hash_compute (out||out_len) NULL pair");
}

/**
 * @test test_mcdc_hash_compute_input_and_len
 * @par MC/DC:
 * Decision: `if ((input == NULL) && (input_len != 0U))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 738).
 * V1 NULL,len=0 -> C1=T,C2=F -> F. V2 !=NULL,len=1 -> C1=F -> F.
 * V3 NULL,len=1 -> C1=T,C2=T -> T. N+1 = 3 vectors for N=2.
 */
static void test_mcdc_hash_compute_input_and_len(void)
{
  TEST_BEGIN("psa MC/DC: hash_compute (input==NULL && len!=0)");
  prep_init();
  uint8_t       out[k_ra_psa_sha256_len];
  size_t        ol  = 0U;
  const uint8_t inp = (uint8_t)'a';
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256, nullptr, 0U, out, sizeof(out), &ol));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     &inp,
                                     (size_t)k_psa_mcdc_short_input,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     nullptr,
                                     (size_t)k_psa_mcdc_short_input,
                                     out,
                                     sizeof(out),
                                     &ol));
  teardown();
  TEST_END("psa MC/DC: hash_compute (input==NULL && len!=0)");
}

/**
 * @test test_mcdc_sign_hash_arg_triple
 * @par MC/DC:
 * Decision: `if ((hash == NULL) || (sig == NULL) || (sig_len == NULL))`
 * (3 conditions, libs/ra_psa_crypto/src/ra_psa_crypto.c line 778).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_sign_hash_arg_triple(void)
{
  TEST_BEGIN("psa MC/DC: sign_hash (hash||sig||sig_len) NULL triple");
  prep_init();
  const ra_psa_key_attr_t attr = {.type  = k_ra_psa_key_type_ecc_p256_priv,
                                  .alg   = k_ra_psa_alg_ecdsa_sha_256,
                                  .usage = k_ra_psa_usage_sign};
  ra_psa_key_t            k    = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra_psa_sha256_len] = {};
  uint8_t sig[k_ra_psa_max_sig_bytes];
  size_t  sl = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,

    ra_psa_sign_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), &sl));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,

    ra_psa_sign_hash(k, k_ra_psa_alg_ecdsa_sha_256, nullptr, sizeof(hash), sig, sizeof(sig), &sl));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,

    ra_psa_sign_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), nullptr, sizeof(sig), &sl));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,

    ra_psa_sign_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), nullptr));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: sign_hash (hash||sig||sig_len) NULL triple");
}

/**
 * @test test_mcdc_verify_hash_arg_pair
 * @par MC/DC:
 * Decision: `if ((hash == NULL) || (sig == NULL))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 834). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_verify_hash_arg_pair(void)
{
  TEST_BEGIN("psa MC/DC: verify_hash (hash||sig) NULL pair");
  prep_init();
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_ecc_p256_priv,
    .alg   = k_ra_psa_alg_ecdsa_sha_256,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_sign | k_ra_psa_usage_verify)};
  ra_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra_psa_sha256_len] = {};
  uint8_t sig[k_ra_psa_sha256_len]  = {};
  size_t  sl                        = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,

    ra_psa_sign_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), &sl));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_verify_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sl));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_verify_hash(k, k_ra_psa_alg_ecdsa_sha_256, nullptr, sizeof(hash), sig, sl));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_psa_verify_hash(k, k_ra_psa_alg_ecdsa_sha_256, hash, sizeof(hash), nullptr, sl));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: verify_hash (hash||sig) NULL pair");
}

/**
 * @test test_mcdc_aead_encrypt_check_triple
 * @par MC/DC:
 * Decision: `if ((nonce == NULL) || (out == NULL) || (out_len == NULL))`
 * (3 conditions, libs/ra_psa_crypto/src/ra_psa_crypto.c line 894,
 * `internal_aead_encrypt_check`). N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_encrypt_check_triple(void)
{
  TEST_BEGIN("psa MC/DC: aead_encrypt_check (nonce||out||out_len) NULL triple");
  prep_init();
  ra_psa_key_t k = mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt));
  TEST_ASSERT_NOT_NULL(k);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     nullptr,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     nullptr,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     nullptr));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: aead_encrypt_check (nonce||out||out_len) NULL triple");
}

/**
 * @test test_mcdc_aead_encrypt_check_plain_aad_quad
 * @par MC/DC:
 * Decision: `if (((plain == NULL) && (plain_len != 0U)) ||
 *               ((aad == NULL) && (aad_len != 0U)))` (4 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 897). Conditions
 * A=(plain==NULL), B=(plain_len!=0), C=(aad==NULL), D=(aad_len!=0).
 * V1 plain!=NULL,plen=8,aad=NULL,alen=0 -> A=F, sub2=F -> F.
 * V2 plain=NULL,plen=8 -> A=T,B=T -> T. V3 plain=NULL,plen=0 -> A=T,B=F -> F.
 * V4 aad=NULL,alen=4 -> sub1=F, C=T,D=T -> T. V5 aad!=NULL,alen=0 -> sub2=F -> F.
 * N+1 = 5 vectors for N=4.
 */
static void test_mcdc_aead_encrypt_check_plain_aad_quad(void)
{
  TEST_BEGIN("psa MC/DC: encrypt_check (plain==NULL&&len) || (aad==NULL&&len)");
  prep_init();
  ra_psa_key_t k = mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt));
  TEST_ASSERT_NOT_NULL(k);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  const uint8_t aad[k_psa_mcdc_short_aad]   = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     nullptr,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     nullptr,
                                     0U,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     sizeof(aad),
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: encrypt_check (plain==NULL&&len) || (aad==NULL&&len)");
}

/**
 * @test test_mcdc_aead_encrypt_handle_alg_usage
 * @par MC/DC:
 * Decision: `if (!internal_handle_valid(handle) ||
 *               (alg != k_ra_psa_alg_aes_gcm) ||
 *               ((handle->attr.usage & k_ra_psa_usage_encrypt) == 0U))`
 * (3 conditions, libs/ra_psa_crypto/src/ra_psa_crypto.c line 903).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_encrypt_handle_alg_usage(void)
{
  TEST_BEGIN("psa MC/DC: encrypt (!valid || alg!=gcm || !usage_encrypt)");
  prep_init();
  ra_psa_key_t  k                           = mcdc_import_aes_key(k_ra_psa_usage_encrypt);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(nullptr,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_sha_256,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  ra_psa_key_t k2 = mcdc_import_aes_key(k_ra_psa_usage_decrypt);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_encrypt(k2,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k2);
  teardown();
  TEST_END("psa MC/DC: encrypt (!valid || alg!=gcm || !usage_encrypt)");
}

/**
 * @test test_mcdc_aead_encrypt_size_pair
 * @par MC/DC:
 * Decision: `if ((nonce_len != gcm_nonce_len) ||
 *               (out_cap < (plain_len + gcm_tag_len)))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 907). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_encrypt_size_pair(void)
{
  TEST_BEGIN("psa MC/DC: encrypt sizes (nonce_len||out_cap)");
  prep_init();
  ra_psa_key_t  k                           = mcdc_import_aes_key(k_ra_psa_usage_encrypt);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     (size_t)k_psa_mcdc_bad_nonce,
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     out,
                                     (size_t)k_psa_mcdc_small_outcap,
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: encrypt sizes (nonce_len||out_cap)");
}

/**
 * @test test_mcdc_aead_decrypt_check_triple
 * @par MC/DC:
 * Decision: `if ((nonce == NULL) || (cipher == NULL) || (out_len == NULL))`
 * (3 conditions, libs/ra_psa_crypto/src/ra_psa_crypto.c line 994,
 * `internal_aead_decrypt_check`). N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_decrypt_check_triple(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (nonce||cipher||out_len) NULL triple");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  TEST_ASSERT_NOT_NULL(k);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     nullptr,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     nullptr,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     nullptr));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (nonce||cipher||out_len) NULL triple");
}

/**
 * @test test_mcdc_aead_decrypt_aad_pair
 * @par MC/DC:
 * Decision: `if ((aad == NULL) && (aad_len != 0U))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 997). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_aad_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (aad==NULL && aad_len!=0)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  const uint8_t aad[k_psa_mcdc_short_aad]   = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  uint8_t        out[k_psa_mcdc_plain_len];
  size_t         ol = 0U;
  const ra_err_t v1 = ra_psa_aead_decrypt(k,
                                          k_ra_psa_alg_aes_gcm,
                                          k_test_nonce,
                                          sizeof(k_test_nonce),
                                          nullptr,
                                          0U,
                                          ct,
                                          ctl,
                                          out,
                                          sizeof(out),
                                          &ol);
  TEST_ASSERT(v1 == k_ra_ok || v1 == k_ra_err_crc_mismatch);
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     sizeof(aad),
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (aad==NULL && aad_len!=0)");
}

/**
 * @test test_mcdc_aead_decrypt_handle_alg_usage
 * @par MC/DC:
 * Decision: `if (!internal_handle_valid(handle) ||
 *               (alg != k_ra_psa_alg_aes_gcm) ||
 *               ((handle->attr.usage & k_ra_psa_usage_decrypt) == 0U))`
 * (3 conditions, libs/ra_psa_crypto/src/ra_psa_crypto.c line 1003).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_decrypt_handle_alg_usage(void)
{
  TEST_BEGIN("psa MC/DC: decrypt (!valid || alg!=gcm || !usage_decrypt)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(nullptr,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_sha_256,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  ra_psa_key_t k2 = mcdc_import_aes_key(k_ra_psa_usage_encrypt);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k2,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k2);
  teardown();
  TEST_END("psa MC/DC: decrypt (!valid || alg!=gcm || !usage_decrypt)");
}

/**
 * @test test_mcdc_aead_decrypt_size_pair
 * @par MC/DC:
 * Decision: `if ((nonce_len != gcm_nonce_len) ||
 *               (cipher_len < gcm_tag_len))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 1007). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_size_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt sizes (nonce_len||cipher_len)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     (size_t)k_psa_mcdc_bad_nonce,
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  uint8_t tiny[k_psa_mcdc_tiny_len] = {0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     tiny,
                                     sizeof(tiny),
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt sizes (nonce_len||cipher_len)");
}

/**
 * @test test_mcdc_aead_decrypt_out_pair
 * @par MC/DC:
 * Decision: `if ((out == NULL) && (plain_len != 0U))` (2 conditions,
 * libs/ra_psa_crypto/src/ra_psa_crypto.c line 1012).
 * V1 out=NULL,plain_len=0 (cipher = exactly tag) -> C1=T,C2=F -> F.
 * V2 out!=NULL,plain_len=8 -> C1=F -> F. V3 out=NULL,plain_len=8 -> T.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_out_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (out==NULL && plain_len!=0)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       ct8[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl8 = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     ct8,
                                     sizeof(ct8),
                                     &ctl8));
  uint8_t ct0[k_ra_psa_gcm_tag_len];
  size_t  ctl0 = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     nullptr,
                                     0U,
                                     ct0,
                                     sizeof(ct0),
                                     &ctl0));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct0,
                                     ctl0,
                                     nullptr,
                                     0U,
                                     &ol));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct8,
                                     ctl8,
                                     out,
                                     sizeof(out),
                                     &ol));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct8,
                                     ctl8,
                                     nullptr,
                                     0U,
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (out==NULL && plain_len!=0)");
}

/**
 * @test test_mcdc_sim_aead_buf_loops
 * @par MC/DC:
 * Loop guards in `internal_sim_aead_tag` (lines 381, 384, 387, 390) and
 * `internal_sim_keystream` (lines 416, 419) -- each is a 2-condition AND
 * `(i < N) && (off < sizeof(buf))`. The C2 flip (off >= 256) is structurally
 * unreachable through the public API: max(key + nonce + aad + cipher) is
 * bounded by the input-validation checks at lines 894-908 well below 256.
 * Documented as a deactivated condition under DO-178C 6.4.4.3. We exercise
 * C1's entered-body and exit transitions for all six loops via an encrypt +
 * decrypt round-trip with non-trivial inputs.
 */
static void test_mcdc_sim_aead_buf_loops(void)
{
  TEST_BEGIN("psa MC/DC: sim AEAD/keystream buffer loop guards (C1)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  const uint8_t aad[k_psa_mcdc_short_aad]   = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     aad,
                                     sizeof(aad),
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  uint8_t ct0[k_ra_psa_gcm_tag_len];
  size_t  ctl0 = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     nullptr,
                                     0U,
                                     ct0,
                                     sizeof(ct0),
                                     &ctl0));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: sim AEAD/keystream buffer loop guards (C1)");
}

/**
 * @test test_mcdc_sim_aead_buf_loops_overflow
 *
 * @par MC/DC:
 * Decisions in libs/ra_psa_crypto/src/ra_psa_crypto.c (sim AEAD path):
 * lines 476/479/482/485 (encrypt scratch fill loops) and 524/527 (HKDF
 * keystream seed fill loops). Each is a 2-cond AND short-circuit:
 *   ``(i < <len>) && (off < sizeof(buf))``
 * with sizeof(buf) == k_ra_psa_sim_scratch_bytes (256). The pre-existing
 * ``test_mcdc_sim_aead_buf_loops`` covers C1 by varying lengths but never
 * exhausts ``off`` because total input stays well under 256 bytes.
 *
 * This test forces ``off >= 256`` by feeding a single AEAD encrypt with
 * combined key(16) + nonce(12) + aad(0) + plaintext(>= 228) >= 256
 * bytes. The cipher-fill loop (line 485) then trips C2=F. By symmetry,
 * the same encrypt also flips C2 in 476/479/482 once off saturates --
 * which it does once cipher_len exhausts the scratch.
 *
 * - V1 (existing): small input keeps off well below 256 -> C2=T always.
 * - V2 (new): plaintext = 240 bytes -> 16+12+0+240 = 268 > 256 -> at
 *   some i the cipher loop sees off==256 -> C1=T, C2=F -> dec F.
 *   Pair (V1,V2) isolates C2 for line 485. Same applies to 524/527 if
 *   exercised through ra_psa_key_derive (HKDF) with similarly large
 *   key+nonce -- here we focus on the encrypt path because it's the
 *   only one with a public API that lets us drive the lengths.
 */
static void test_mcdc_sim_aead_buf_loops_overflow(void)
{
  TEST_BEGIN("psa MC/DC: sim AEAD scratch overflow (C2 for line 485)");
  prep_init();
  ra_psa_key_t k =
    mcdc_import_aes_key((ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt));
  /* 240-byte plaintext: 16(key) + 12(nonce) + 0(aad) + 240(cipher) = 268 > 256. */
  uint8_t plain[240] = {};
  uint8_t ct[240 + k_ra_psa_gcm_tag_len];
  size_t  ctl = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_encrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     plain,
                                     sizeof(plain),
                                     ct,
                                     sizeof(ct),
                                     &ctl));
  /* Round-trip decrypt to keep the test meaningful. */
  uint8_t out[240];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_aead_decrypt(k,
                                     k_ra_psa_alg_aes_gcm,
                                     k_test_nonce,
                                     sizeof(k_test_nonce),
                                     nullptr,
                                     0U,
                                     ct,
                                     ctl,
                                     out,
                                     sizeof(out),
                                     &ol));
  (void)ra_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: sim AEAD scratch overflow (C2 for line 485)");
}

int32_t main(void)
{
  test_init_double();
  test_deinit_without_init();
  test_import_uninitialised();
  test_import_invalid_args();
  test_import_destroy_round_trip();
  test_pool_exhaustion();
  test_destroy_invalid_handle();
  test_hash_known_vector();
  test_hash_invalid_args();
  test_sign_verify_round_trip();
  test_sign_wrong_alg();
  test_aead_round_trip();
  test_aead_invalid_args();
  test_mcdc_handle_valid_range_check();
  test_mcdc_key_import_arg_quad();
  test_mcdc_hash_compute_out_pair();
  test_mcdc_hash_compute_input_and_len();
  test_mcdc_sign_hash_arg_triple();
  test_mcdc_verify_hash_arg_pair();
  test_mcdc_aead_encrypt_check_triple();
  test_mcdc_aead_encrypt_check_plain_aad_quad();
  test_mcdc_aead_encrypt_handle_alg_usage();
  test_mcdc_aead_encrypt_size_pair();
  test_mcdc_aead_decrypt_check_triple();
  test_mcdc_aead_decrypt_aad_pair();
  test_mcdc_aead_decrypt_handle_alg_usage();
  test_mcdc_aead_decrypt_size_pair();
  test_mcdc_aead_decrypt_out_pair();
  test_mcdc_sim_aead_buf_loops();
  test_mcdc_sim_aead_buf_loops_overflow();
  (void)fprintf(stderr, "[OK  ] test_ra_psa_crypto.c\n");
  return 0;
}
