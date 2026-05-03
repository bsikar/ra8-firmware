/**
 * @file test_app_crypto_aes_demo.c
 * @brief Integration test: AES-128-GCM encrypt + decrypt round-trip
 *
 * @details
 * Mirrors examples/ek_ra8d2/crypto_aes_demo/main.c bring-up flow and
 * exercises the soft-fallback ra_psa_crypto AEAD path in
 * RA_SIMULATOR_MODE (always defined for the host test build).
 * Confirms key import, encrypt, decrypt, byte compare, and
 * destroy succeed end to end, plus the negative paths (NULL handle,
 * tampered ciphertext).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_psa_crypto.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_aes_app_key_bytes   = 16U,
  k_test_aes_app_plain_bytes = 8U,
  k_test_aes_app_aad_bytes   = 4U,
} test_aes_app_layout_t;

static const uint8_t k_t_aes_key[k_test_aes_app_key_bytes] = {
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
  0xAAU,
  0xBBU,
  0xCCU,
  0xDDU,
  0xEEU,
  0xFFU,
};
static const uint8_t k_t_aes_nonce[k_ra_psa_gcm_nonce_len] = {
  0xA0U,
  0xA1U,
  0xA2U,
  0xA3U,
  0xA4U,
  0xA5U,
  0xA6U,
  0xA7U,
  0xA8U,
  0xA9U,
  0xAAU,
  0xABU,
};
static const uint8_t k_t_aes_plain[k_test_aes_app_plain_bytes] = {
  'R',
  'A',
  '8',
  'D',
  '2',
  '_',
  'O',
  'K',
};
static const uint8_t k_t_aes_aad[k_test_aes_app_aad_bytes] = {'A', 'E', 'A', 'D'};

static void prep(void)
{
  /* Tear down + re-init so each test gets a clean facade state. */
  (void)ra_psa_crypto_deinit();
  (void)ra_psa_crypto_init();
}

static void teardown(void)
{
  (void)ra_psa_crypto_deinit();
}

[[nodiscard]] static ra_err_t
do_round_trip(uint8_t* recovered, size_t recovered_cap, size_t* rec_len)
{
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t key = nullptr;
  ra_err_t     err = ra_psa_key_import(&key, &attr, k_t_aes_key, (size_t)k_test_aes_app_key_bytes);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t ct[k_test_aes_app_plain_bytes + k_ra_psa_gcm_tag_len] = {};
  size_t  ct_len                                                = 0U;
  err = ra_psa_aead_encrypt(key,
                            k_ra_psa_alg_aes_gcm,
                            k_t_aes_nonce,
                            (size_t)k_ra_psa_gcm_nonce_len,
                            k_t_aes_aad,
                            (size_t)k_test_aes_app_aad_bytes,
                            k_t_aes_plain,
                            (size_t)k_test_aes_app_plain_bytes,
                            ct,
                            sizeof(ct),
                            &ct_len);
  if (err != k_ra_ok) {
    (void)ra_psa_key_destroy(key);
    return err;
  }
  err = ra_psa_aead_decrypt(key,
                            k_ra_psa_alg_aes_gcm,
                            k_t_aes_nonce,
                            (size_t)k_ra_psa_gcm_nonce_len,
                            k_t_aes_aad,
                            (size_t)k_test_aes_app_aad_bytes,
                            ct,
                            ct_len,
                            recovered,
                            recovered_cap,
                            rec_len);
  (void)ra_psa_key_destroy(key);
  return err;
}

/**
 * @brief Golden round-trip: import, encrypt, decrypt, compare.
 *
 * @par MC/DC:
 * Compound decision in app: ``import != ok || encrypt != ok ||
 * decrypt != ok || memcmp != 0``. Four atomic conditions x N+1 = 5
 * vectors. Vector A (this test): all ok. Vectors B-E covered by the
 * negative tests below + the existing test_ra_psa_crypto.c suite.
 */
static void test_aes_app_round_trip_ok(void)
{
  prep();
  TEST_BEGIN("crypto_aes_demo: encrypt + decrypt round-trip");
  uint8_t recovered[k_test_aes_app_plain_bytes] = {};
  size_t  rec_len                               = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)do_round_trip(recovered, sizeof(recovered), &rec_len));
  TEST_ASSERT_EQ((int64_t)sizeof(recovered), (int64_t)rec_len);
  TEST_ASSERT_EQ(0, memcmp(recovered, k_t_aes_plain, sizeof(recovered)));
  teardown();
  TEST_END("crypto_aes_demo: encrypt + decrypt round-trip");
}

/**
 * @brief Tampered ciphertext is rejected by AEAD decrypt (tag check).
 *
 * @par MC/DC:
 * Decision in driver: ``aead tag check pass``. Two vectors -- tag
 * intact (golden test above) + tag flipped (this test).
 */
static void test_aes_app_tag_tamper_rejected(void)
{
  prep();
  TEST_BEGIN("crypto_aes_demo: tampered tag rejected");
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  ra_psa_key_t key = nullptr;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_psa_key_import(&key, &attr, k_t_aes_key, (size_t)k_test_aes_app_key_bytes));
  uint8_t ct[k_test_aes_app_plain_bytes + k_ra_psa_gcm_tag_len] = {};
  size_t  ct_len                                                = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_psa_aead_encrypt(key,
                                          k_ra_psa_alg_aes_gcm,
                                          k_t_aes_nonce,
                                          (size_t)k_ra_psa_gcm_nonce_len,
                                          k_t_aes_aad,
                                          (size_t)k_test_aes_app_aad_bytes,
                                          k_t_aes_plain,
                                          (size_t)k_test_aes_app_plain_bytes,
                                          ct,
                                          sizeof(ct),
                                          &ct_len));
  ct[ct_len - 1U] ^= 0xFFU;
  uint8_t        recovered[k_test_aes_app_plain_bytes] = {};
  size_t         rec_len                               = 0U;
  const ra_err_t err = ra_psa_aead_decrypt(key,
                                           k_ra_psa_alg_aes_gcm,
                                           k_t_aes_nonce,
                                           (size_t)k_ra_psa_gcm_nonce_len,
                                           k_t_aes_aad,
                                           (size_t)k_test_aes_app_aad_bytes,
                                           ct,
                                           ct_len,
                                           recovered,
                                           sizeof(recovered),
                                           &rec_len);
  TEST_ASSERT_EQ((int)k_ra_err_crc_mismatch, (int)err);
  (void)ra_psa_key_destroy(key);
  teardown();
  TEST_END("crypto_aes_demo: tampered tag rejected");
}

/**
 * @brief NULL key handle is rejected by ra_psa_key_import.
 *
 * @par MC/DC:
 * Decision: ``out_handle == nullptr``. Two vectors -- NULL (this
 * test) + non-NULL (golden test).
 */
static void test_aes_app_import_null_rejected(void)
{
  prep();
  TEST_BEGIN("crypto_aes_demo: NULL handle import rejected");
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)(k_ra_psa_usage_encrypt | k_ra_psa_usage_decrypt),
  };
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_psa_key_import(nullptr, &attr, k_t_aes_key, (size_t)k_test_aes_app_key_bytes));
  teardown();
  TEST_END("crypto_aes_demo: NULL handle import rejected");
}

int main(void)
{
  test_aes_app_round_trip_ok();
  test_aes_app_tag_tamper_rejected();
  test_aes_app_import_null_rejected();
  return 0;
}
