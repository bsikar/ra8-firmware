/**
 * @file test_ra8_psa_crypto_api.c
 * @brief Unit tests for the ra8_psa_crypto facade public-API contract
 *
 * @details
 * Split sibling of the original test_ra8_psa_crypto.c suite covering
 * the happy paths and first-line argument rejection of every public
 * entry point in ``ra8_psa_crypto.h`` against the simulator-mode
 * stand-ins defined in ``ra8_psa_crypto.c``:
 *
 * - init / deinit symmetry and double-init guard
 * - key import / destroy with the static pool capacity
 * - SHA-256 round-trip against a known FIPS 180-4 test vector
 * - sign + verify round-trip with a deterministic test key
 * - AES-GCM encrypt + decrypt round-trip with tag-tampering detection
 * - error paths: NULL pointers, uninitialized facade, oversized keys,
 *   wrong algorithm tags, undersized output buffers
 *
 * Sibling suites: test_ra8_psa_crypto_guards.c (non-AEAD MC/DC
 * vectors + single-condition error legs) and
 * test_ra8_psa_crypto_aead_mcdc.c (AEAD MC/DC vectors).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"
#include "unity_minimal.h"

/**
 * @enum psa_crypto_api_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_psa_crypto_api_sig_ff = 0xFFU,
  k_psa_crypto_api_val_64 = 64,
} psa_crypto_api_uint8_const_t;

/**
 * @enum ra8_psa_test_const_t
 * @brief Magic-number-free constants used by the test bodies.
 */
typedef enum : uint16_t {
  k_psa_test_aes_key_len  = 16U,  /**< 128-bit symmetric key.               */
  k_psa_test_plain_len    = 8U,   /**< Sample plaintext length.             */
  k_psa_test_aad_len      = 4U,   /**< Sample AAD length.                   */
  k_psa_test_oversize_len = 200U, /**< Larger than k_ra8_psa_max_key_bytes. */
} ra8_psa_test_const_t;

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
/* helpers */
/* ------------------------------------------------------------------ */
static void prep_init(void)
{
  /* deinit is allowed to fail with not-initialized on the very first
   * call; subsequent calls observe a properly torn-down module. */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
}

static void teardown(void)
{
  (void)ra8_psa_crypto_deinit();
}

/* ------------------------------------------------------------------ */
/* tests */
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
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_psa_crypto_init());
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
  (void)ra8_psa_crypto_deinit(); /* drain any leftover state */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_psa_crypto_deinit());
  TEST_END("psa deinit without init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_import_uninitialized(void)
{
  TEST_BEGIN("psa import without init");
  (void)ra8_psa_crypto_deinit();
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt),
  };
  ra8_psa_key_t k = (ra8_psa_key_t)0x1U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = k_ra8_psa_usage_encrypt,
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(nullptr, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(&k, nullptr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(&k, &attr, nullptr, (size_t)k_psa_test_aes_key_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_import(&k, &attr, k_test_aes_key, 0U));
  /* Oversized key. */
  uint8_t big[k_psa_test_oversize_len];
  (void)memset(big, 0x5aU, sizeof(big));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_psa_key_import(&k, &attr, big, sizeof(big)));
  /* Empty usage rejected. */
  const ra8_psa_key_attr_t no_use = {.type  = k_ra8_psa_key_type_aes,
                                     .alg   = k_ra8_psa_alg_aes_gcm,
                                     .usage = k_ra8_psa_usage_none};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(&k, &no_use, k_test_aes_key, sizeof(k_test_aes_key)));
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt),
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_NOT_NULL(k);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_destroy(k));
  /* Destroying twice must fail (slot no longer in_use). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(k));
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = k_ra8_psa_usage_encrypt,
  };
  ra8_psa_key_t handles[k_ra8_psa_max_keys];
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_psa_max_keys; ++i) {
    handles[i] = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_psa_key_import(&handles[i], &attr, k_test_aes_key, sizeof(k_test_aes_key)));
    TEST_ASSERT_NOT_NULL(handles[i]);
  }
  ra8_psa_key_t over = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_psa_key_import(&over, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_NULL(over);
  /* Free one and reissue. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_destroy(handles[0]));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&handles[0], &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_psa_max_keys; ++i) {
    (void)ra8_psa_key_destroy(handles[i]);
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(nullptr));
  /* Pointer that doesn't lie inside the pool. */
  uint8_t fake = 0U;
  /* ra8_psa_key_t is itself a pointer typedef, so `const` here qualifies the
   * local handle -- which is exactly the intent: the handle never changes.
   * There is no pointee to qualify; the key material is owned by the pool. */
  // NOLINTNEXTLINE(misc-misplaced-const)
  const ra8_psa_key_t bogus = (ra8_psa_key_t)&fake;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(bogus));
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
  uint8_t       digest[k_ra8_psa_sha256_len];
  size_t        out_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      input,
                                      sizeof(input),
                                      digest,
                                      sizeof(digest),
                                      &out_len));
  TEST_ASSERT_EQ(k_ra8_psa_sha256_len, out_len);
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
  uint8_t out[k_ra8_psa_sha256_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_psa_hash_compute(k_ra8_psa_alg_sha_256, nullptr, 0U, nullptr, sizeof(out), &ol));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,

    ra8_psa_hash_compute(k_ra8_psa_alg_aes_gcm, (const uint8_t*)"x", 1U, out, sizeof(out), &ol));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_psa_hash_compute(k_ra8_psa_alg_sha_256, (const uint8_t*)"x", 1U, out, 4U, &ol));
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_ecc_p256_priv,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_sign | k_ra8_psa_usage_verify),
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));

  /* Hash a fixed message and sign the digest. */
  const uint8_t msg[3] = {'a', 'b', 'c'};
  uint8_t       digest[k_ra8_psa_sha256_len];
  size_t        digest_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      msg,
                                      sizeof(msg),
                                      digest,
                                      sizeof(digest),
                                      &digest_len));

  uint8_t sig[k_ra8_psa_max_sig_bytes];
  size_t  sig_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_sign_hash(k,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   digest,
                                   digest_len,
                                   sig,
                                   sizeof(sig),
                                   &sig_len));
  TEST_ASSERT(sig_len > 0U);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_psa_verify_hash(k, k_ra8_psa_alg_ecdsa_sha_256, digest, digest_len, sig, sig_len));

  /* Tamper a byte and ensure verification fails. */
  sig[0] ^= k_psa_crypto_api_sig_ff;
  TEST_ASSERT_EQ(
    k_ra8_err_crc_mismatch,
    ra8_psa_verify_hash(k, k_ra8_psa_alg_ecdsa_sha_256, digest, digest_len, sig, sig_len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_destroy(k));
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_ecc_p256_priv,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = k_ra8_psa_usage_sign,
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_max_sig_bytes];
  size_t  sl = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_psa_sign_hash(k, k_ra8_psa_alg_aes_gcm, hash, sizeof(hash), sig, sizeof(sig), &sl));
  (void)ra8_psa_key_destroy(k);
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt),
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));

  const uint8_t plain[k_psa_test_plain_len] = {'h', 'e', 'l', 'l', 'o', '!', '!', '\0'};
  const uint8_t aad[k_psa_test_aad_len]     = {'A', 'A', 'D', '0'};
  uint8_t       ct[k_psa_test_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ct_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      aad,
                                      sizeof(aad),
                                      plain,
                                      sizeof(plain),
                                      ct,
                                      sizeof(ct),
                                      &ct_len));
  TEST_ASSERT_EQ((sizeof(plain) + (size_t)k_ra8_psa_gcm_tag_len), ct_len);

  uint8_t recovered[k_psa_test_plain_len];
  size_t  rec_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
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

  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead encrypt + decrypt round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_aead_tamper_detected(void)
{
  TEST_BEGIN("psa aead decrypt detects a tampered tag");
  prep_init();
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt),
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));

  const uint8_t plain[k_psa_test_plain_len] = {'h', 'e', 'l', 'l', 'o', '!', '!', '\0'};
  const uint8_t aad[k_psa_test_aad_len]     = {'A', 'A', 'D', '0'};
  uint8_t       ct[k_psa_test_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ct_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      aad,
                                      sizeof(aad),
                                      plain,
                                      sizeof(plain),
                                      ct,
                                      sizeof(ct),
                                      &ct_len));

  /* Tamper the tag and ensure decrypt detects it. */
  ct[ct_len - 1U] ^= k_psa_crypto_api_sig_ff;
  uint8_t recovered[k_psa_test_plain_len];
  size_t  rec_len = 0U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      aad,
                                      sizeof(aad),
                                      ct,
                                      ct_len,
                                      recovered,
                                      sizeof(recovered),
                                      &rec_len));

  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead decrypt detects a tampered tag");
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
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt),
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));

  uint8_t out[k_psa_crypto_api_val_64];
  size_t  ol = 0U;
  /* Wrong nonce length. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      tiny,
                                      sizeof(tiny),
                                      out,
                                      sizeof(out),
                                      &ol));

  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead invalid args");
}

int32_t main(void)
{
  test_init_double();
  test_deinit_without_init();
  test_import_uninitialized();
  test_import_invalid_args();
  test_import_destroy_round_trip();
  test_pool_exhaustion();
  test_destroy_invalid_handle();
  test_hash_known_vector();
  test_hash_invalid_args();
  test_sign_verify_round_trip();
  test_sign_wrong_alg();
  test_aead_round_trip();
  test_aead_tamper_detected();
  test_aead_invalid_args();
  (void)fprintf(stderr, "[OK  ] test_ra8_psa_crypto_api.c\n");
  return 0;
}
