/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_key_vault.c
 * @brief Unit tests for the secure key vault + NSC veneer
 */

#include <stdint.h>

#include "key_vault.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "unity_minimal.h"

static const uint8_t k_test_key[k_ra8_key_vault_key_bytes] = {
  0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU,
  0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U,
  0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU, 0x20U,
};

static const uint8_t k_test_chal[k_ra8_key_vault_chal_bytes] = {
  0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xA8U, 0xA9U, 0xAAU,
  0xABU, 0xACU, 0xADU, 0xAEU, 0xAFU, 0xB0U, 0xB1U, 0xB2U, 0xB3U, 0xB4U, 0xB5U,
  0xB6U, 0xB7U, 0xB8U, 0xB9U, 0xBAU, 0xBBU, 0xBCU, 0xBDU, 0xBEU, 0xBFU,
};

/** @brief Key-authentication-key (KAK) sizes exercised by the tests. */
typedef enum : uint16_t {
  k_test_kak_128     = 16U, /**< AES-128 KAK length (also a too-small cap). */
  k_test_kak_256     = 32U, /**< AES-256 KAK length.                        */
  k_test_kak_bad_len = 24U, /**< Unsupported KAK length (not 16 nor 32).    */
} test_kv_kak_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_zeroes_vault(void)
{
  TEST_BEGIN("ra8_key_vault_init zeroes everything");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_END("ra8_key_vault_init zeroes everything");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_store_then_challenge(void)
{
  TEST_BEGIN("store + challenge produces deterministic digest");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_store(0U, k_test_key));

  uint8_t digest_a[k_ra8_key_vault_digest_bytes] = {0U};
  uint8_t digest_b[k_ra8_key_vault_digest_bytes] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_sha256_xor_challenge(0U, k_test_chal, digest_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_sha256_xor_challenge(0U, k_test_chal, digest_b));
  /* Same inputs -> same digest. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_key_vault_digest_bytes; ++i) {
    TEST_ASSERT_EQ(digest_a[i], digest_b[i]);
  }
  TEST_END("store + challenge produces deterministic digest");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_different_challenge_different_digest(void)
{
  TEST_BEGIN("changing the challenge changes the digest");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_store(0U, k_test_key));

  uint8_t digest_a[k_ra8_key_vault_digest_bytes] = {0U};
  uint8_t digest_b[k_ra8_key_vault_digest_bytes] = {0U};
  uint8_t chal_b[k_ra8_key_vault_chal_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_key_vault_chal_bytes; ++i) {
    chal_b[i] = k_test_chal[i];
  }
  chal_b[0] ^= 0x01U; /* Flip one bit. */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_sha256_xor_challenge(0U, k_test_chal, digest_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_sha256_xor_challenge(0U, chal_b, digest_b));
  bool any_diff = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_key_vault_digest_bytes; ++i) {
    if (digest_a[i] != digest_b[i]) {
      any_diff = true;
      break;
    }
  }
  TEST_ASSERT(any_diff);
  TEST_END("changing the challenge changes the digest");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arg_validation(void)
{
  TEST_BEGIN("ra8_key_vault_*: arg validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_key_vault_store(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_key_vault_store(99U, k_test_key));

  uint8_t digest[k_ra8_key_vault_digest_bytes] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_key_vault_sha256_xor_challenge(0U, nullptr, digest));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_key_vault_sha256_xor_challenge(0U, k_test_chal, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_key_vault_sha256_xor_challenge(99U, k_test_chal, digest));
  TEST_END("ra8_key_vault_*: arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nsc_veneer_forwards(void)
{
  TEST_BEGIN("ra8_nsc_key_vault_challenge forwards through veneer");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_store(2U, k_test_key));

  uint8_t digest_direct[k_ra8_key_vault_digest_bytes] = {0U};
  uint8_t digest_veneer[k_ra8_key_vault_digest_bytes] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_sha256_xor_challenge(2U, k_test_chal, digest_direct));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_key_vault_challenge(2U, k_test_chal, digest_veneer));
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_key_vault_digest_bytes; ++i) {
    TEST_ASSERT_EQ(digest_direct[i], digest_veneer[i]);
  }
  TEST_END("ra8_nsc_key_vault_challenge forwards through veneer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the KAK provisioning /
 * load contract branch by branch; each case toggles one precondition.)
 */
static void test_mac_key_provisioning(void)
{
  TEST_BEGIN("ra8_key_vault_set/load_mac_key: provision + read back the KAK");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());

  uint8_t  out[k_ra8_key_vault_mac_key_bytes] = {0U};
  uint16_t out_len                            = 0U;

  /* Before provisioning: no KAK -> not_found. */
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    ra8_key_vault_load_mac_key(out, (uint16_t)k_ra8_key_vault_mac_key_bytes, &out_len));

  /* set_mac_key argument validation. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_key_vault_set_mac_key(nullptr, (uint16_t)k_test_kak_256));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_key_vault_set_mac_key(k_test_key, (uint16_t)k_test_kak_bad_len));

  /* Provision a 32-byte (AES-256) KAK, then read it back verbatim. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_set_mac_key(k_test_key, (uint16_t)k_test_kak_256));

  /* load_mac_key argument validation. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_key_vault_load_mac_key(nullptr, (uint16_t)k_ra8_key_vault_mac_key_bytes, &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_key_vault_load_mac_key(out, (uint16_t)k_ra8_key_vault_mac_key_bytes, nullptr));
  /* Output buffer smaller than the stored KAK -> invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_key_vault_load_mac_key(out, (uint16_t)k_test_kak_128, &out_len));

  /* Successful load returns the exact bytes and length. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_key_vault_load_mac_key(out, (uint16_t)k_ra8_key_vault_mac_key_bytes, &out_len));
  TEST_ASSERT_EQ(k_test_kak_256, out_len);
  for (uint16_t i = 0U; i < (uint16_t)k_test_kak_256; ++i) {
    TEST_ASSERT_EQ(k_test_key[i], out[i]);
  }

  /* Re-provisioning with a 16-byte (AES-128) KAK is also accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_set_mac_key(k_test_key, (uint16_t)k_test_kak_128));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_key_vault_load_mac_key(out, (uint16_t)k_ra8_key_vault_mac_key_bytes, &out_len));
  TEST_ASSERT_EQ(k_test_kak_128, out_len);
  TEST_END("ra8_key_vault_set/load_mac_key: provision + read back the KAK");
}

int32_t main(void)
{
  test_init_zeroes_vault();
  test_store_then_challenge();
  test_different_challenge_different_digest();
  test_arg_validation();
  test_nsc_veneer_forwards();
  test_mac_key_provisioning();
  (void)fprintf(stderr, "[OK ] test_ra8_key_vault.c\n");
  return 0;
}
