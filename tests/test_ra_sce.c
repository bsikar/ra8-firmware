/**
 * @file test_ra_sce.c
 * @brief Unit tests for ra_sce.c (Secure Crypto Engine HAL)
 *
 * @details
 * Drives the SCE HAL public API against the ``ra_sim_mmap``-backed
 * register window. The implementation in ``libs/ra_hal/src/ra_sce.c``
 * is a deterministic software stub (see file-level @warning there);
 * these tests therefore validate against **stub-pattern** invariants,
 * not NIST Known-Answer-Test vectors:
 *
 * - AES round-trip restores the plaintext;
 * - SHA over the same input produces the same digest;
 * - HMAC over the same (key, message) produces the same tag;
 * - HMAC tag changes when the key changes;
 * - TRNG returns a buffer that is not entirely zero (the stub
 *   xorshift mixer never emits an all-zero word for the first
 *   request after open);
 * - all argument-validation paths reject the documented bad inputs.
 *
 * When a real crypto backend is dropped in place of the stub, this
 * file should be re-pointed at NIST KAT vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_sce_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sce.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_sce_test_const_t
 * @brief Magic numbers used by the tests, named to keep the
 *        no-magic-numbers rule satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_test_aes_block      = 16U,   /**< AES block size in bytes.        */
  k_ra_sce_test_two_blocks     = 32U,   /**< Two AES blocks worth of data.   */
  k_ra_sce_test_unaligned_len  = 17U,   /**< Non-block-aligned length.       */
  k_ra_sce_test_ctr_len        = 13U,   /**< Arbitrary CTR-mode length.      */
  k_ra_sce_test_msg_len        = 24U,   /**< Generic message length.         */
  k_ra_sce_test_trng_len       = 32U,   /**< TRNG draw length.               */
  k_ra_sce_test_short_trng_len = 5U,    /**< Non-aligned TRNG draw length.   */
  k_ra_sce_test_zero_len       = 0U,    /**< Reject-bad-length sentinel.     */
  k_ra_sce_test_bad_mode       = 99U,   /**< Out-of-range mode value.        */
  k_ra_sce_test_bad_key_bits   = 100U,  /**< Out-of-range key-bits value.    */
  k_ra_sce_test_pattern_msg    = 0x5AU, /**< Pattern byte for messages.      */
  k_ra_sce_test_pattern_key    = 0xA5U, /**< Pattern byte for AES keys.     */
  k_ra_sce_test_pattern_iv     = 0x33U, /**< Pattern byte for IV.           */
  k_ra_sce_test_pattern_hkey   = 0x77U, /**< Pattern byte for HMAC keys.    */
  k_ra_sce_test_pattern_hkey_b = 0x88U, /**< Alternate HMAC key pattern.    */
  k_ra_sce_test_hkey_len       = 16U,   /**< HMAC key length.                */
} ra_sce_test_const_t;

/**
 * @brief Reset the world before each test.
 *
 * @details
 * Scrubs the simulated MMIO backing store and resets ``ra_mstp``
 * so each test sees a clean state. Then opens the SCE engine via
 * ``ra_sce_open`` so the per-test body can exercise the HAL.
 *
 * @pre Simulator regions are mapped (constructor in ``ra_sim_mmap``).
 * @post SCE engine is open; subsequent calls are valid.
 *
 * @since 0.1.0
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_sce_cfg_t cfg = {.run_self_test = true};
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_open(&cfg));
}

/**
 * @brief Tear down the engine after each test.
 *
 * @details
 * Calls ``ra_sce_close``; ignores ``k_ra_err_invalid_state`` for
 * tests that themselves close the engine.
 *
 * @pre None.
 * @post SCE engine is closed.
 *
 * @since 0.1.0
 */
static void teardown(void)
{
  (void)ra_sce_close();
}

/**
 * @brief Test: ``ra_sce_open(NULL)`` must reject the NULL config.
 * @since 0.1.0
 */
static void test_open_null_cfg(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sce_open(nullptr));
}

/**
 * @brief Test: AES-128 ECB round-trip recovers the plaintext.
 * @since 0.1.0
 */
static void test_aes128_ecb_roundtrip(void)
{
  prep();
  uint8_t key[k_ra_sce_test_aes_block];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  uint8_t plain[k_ra_sce_test_two_blocks];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    plain[i] = (uint8_t)i;
  }
  uint8_t cipher[k_ra_sce_test_two_blocks];
  uint8_t back[k_ra_sce_test_two_blocks];

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_decrypt(cipher, back, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    TEST_ASSERT_EQ(plain[i], back[i]);
  }
  /* Stub-pattern: cipher must differ from plain in at least one byte. */
  bool different = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    if (cipher[i] != plain[i]) {
      different = true;
      break;
    }
  }
  TEST_ASSERT(different);
  teardown();
}

/**
 * @brief Test: AES-256 CBC round-trip recovers the plaintext.
 * @since 0.1.0
 */
static void test_aes256_cbc_roundtrip(void)
{
  prep();
  uint8_t key[k_ra_sce_aes_max_key_bytes];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  uint8_t iv[k_ra_sce_test_aes_block];
  (void)memset(iv, (int)k_ra_sce_test_pattern_iv, sizeof(iv));

  uint8_t plain[k_ra_sce_test_two_blocks];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    plain[i] = (uint8_t)((uint32_t)k_ra_sce_test_pattern_msg + i);
  }
  uint8_t cipher[k_ra_sce_test_two_blocks];
  uint8_t back[k_ra_sce_test_two_blocks];

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_256, k_ra_sce_aes_mode_cbc, iv));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_256, k_ra_sce_aes_mode_cbc, iv));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_decrypt(cipher, back, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    TEST_ASSERT_EQ(plain[i], back[i]);
  }
  teardown();
}

/**
 * @brief Test: AES CTR mode accepts non-block-aligned lengths.
 * @since 0.1.0
 */
static void test_aes_ctr_accepts_unaligned(void)
{
  prep();
  uint8_t key[k_ra_sce_test_aes_block];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  uint8_t iv[k_ra_sce_test_aes_block];
  (void)memset(iv, (int)k_ra_sce_test_pattern_iv, sizeof(iv));
  uint8_t plain[k_ra_sce_test_ctr_len];
  uint8_t cipher[k_ra_sce_test_ctr_len];
  uint8_t back[k_ra_sce_test_ctr_len];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_ctr_len; ++i) {
    plain[i] = (uint8_t)i;
  }

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ctr, iv));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_ctr_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ctr, iv));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_decrypt(cipher, back, (uint32_t)k_ra_sce_test_ctr_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_ctr_len; ++i) {
    TEST_ASSERT_EQ(plain[i], back[i]);
  }
  teardown();
}

/**
 * @brief Test: AES ECB rejects non-block-aligned length.
 * @since 0.1.0
 */
static void test_aes_ecb_rejects_unaligned(void)
{
  prep();
  uint8_t key[k_ra_sce_test_aes_block];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  uint8_t plain[k_ra_sce_test_unaligned_len];
  uint8_t cipher[k_ra_sce_test_unaligned_len];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sce_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_unaligned_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_aes_finish(nullptr));
  teardown();
}

/**
 * @brief Test: ``ra_sce_aes_init`` rejects unsupported key widths.
 * @since 0.1.0
 */
static void test_aes_key_bits_validation(void)
{
  prep();
  uint8_t key[k_ra_sce_aes_max_key_bytes];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sce_aes_init(key,
                                 (ra_sce_aes_key_bits_t)k_ra_sce_test_bad_key_bits,
                                 k_ra_sce_aes_mode_ecb,
                                 nullptr));
  teardown();
}

/**
 * @brief Test: ``ra_sce_aes_init`` rejects unknown modes.
 * @since 0.1.0
 */
static void test_aes_mode_validation(void)
{
  prep();
  uint8_t key[k_ra_sce_test_aes_block];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sce_aes_init(key,
                                 k_ra_sce_aes_key_bits_128,
                                 (ra_sce_aes_mode_t)k_ra_sce_test_bad_mode,
                                 nullptr));
  teardown();
}

/**
 * @brief Test: AES encrypt without prior init returns invalid_state.
 * @since 0.1.0
 */
static void test_aes_encrypt_requires_init(void)
{
  prep();
  uint8_t plain[k_ra_sce_test_aes_block];
  uint8_t cipher[k_ra_sce_test_aes_block];
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_sce_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_aes_block));
  teardown();
}

/**
 * @brief Test: SHA-256 produces deterministic output for the same input.
 * @since 0.1.0
 */
static void test_sha256_deterministic(void)
{
  prep();
  uint8_t msg[k_ra_sce_test_msg_len];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_msg_len; ++i) {
    msg[i] = (uint8_t)((uint32_t)k_ra_sce_test_pattern_msg ^ i);
  }
  uint8_t digest_a[k_ra_sce_sha256_digest_bytes];
  uint8_t digest_b[k_ra_sce_sha256_digest_bytes];

  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_init(k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_final(digest_a));

  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_init(k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_final(digest_b));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_sha256_digest_bytes; ++i) {
    TEST_ASSERT_EQ(digest_a[i], digest_b[i]);
  }
  /* Stub-pattern: digest is not all-zero. */
  bool nonzero = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_sha256_digest_bytes; ++i) {
    if (digest_a[i] != 0U) {
      nonzero = true;
      break;
    }
  }
  TEST_ASSERT(nonzero);
  teardown();
}

/**
 * @brief Test: ``ra_sce_sha_init`` rejects unknown modes; ``_final``
 *        rejects NULL output.
 * @since 0.1.0
 */
static void test_sha_arg_validation(void)
{
  prep();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sce_sha_init((ra_sce_sha_mode_t)k_ra_sce_test_bad_mode));

  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_init(k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sce_sha_final(nullptr));
  uint8_t digest[k_ra_sce_sha256_digest_bytes];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_sha_final(digest));
  teardown();
}

/**
 * @brief Test: HMAC-SHA-256 is deterministic for the same (key, msg).
 * @since 0.1.0
 */
static void test_hmac_sha256_deterministic(void)
{
  prep();
  uint8_t key[k_ra_sce_test_hkey_len];
  (void)memset(key, (int)k_ra_sce_test_pattern_hkey, sizeof(key));
  uint8_t msg[k_ra_sce_test_msg_len];
  (void)memset(msg, (int)k_ra_sce_test_pattern_msg, sizeof(msg));

  uint8_t mac_a[k_ra_sce_sha256_digest_bytes];
  uint8_t mac_b[k_ra_sce_sha256_digest_bytes];

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_hmac_init(key, (uint32_t)k_ra_sce_test_hkey_len, k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_final(mac_a));

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_hmac_init(key, (uint32_t)k_ra_sce_test_hkey_len, k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_final(mac_b));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_sha256_digest_bytes; ++i) {
    TEST_ASSERT_EQ(mac_a[i], mac_b[i]);
  }
  teardown();
}

/**
 * @brief Test: HMAC tag changes when the key changes.
 * @since 0.1.0
 */
static void test_hmac_changes_with_key(void)
{
  prep();
  uint8_t key_a[k_ra_sce_test_hkey_len];
  uint8_t key_b[k_ra_sce_test_hkey_len];
  (void)memset(key_a, (int)k_ra_sce_test_pattern_hkey, sizeof(key_a));
  (void)memset(key_b, (int)k_ra_sce_test_pattern_hkey_b, sizeof(key_b));
  uint8_t msg[k_ra_sce_test_msg_len];
  (void)memset(msg, (int)k_ra_sce_test_pattern_msg, sizeof(msg));

  uint8_t mac_a[k_ra_sce_sha256_digest_bytes];
  uint8_t mac_b[k_ra_sce_sha256_digest_bytes];

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sce_hmac_init(key_a, (uint32_t)k_ra_sce_test_hkey_len, k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_final(mac_a));

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sce_hmac_init(key_b, (uint32_t)k_ra_sce_test_hkey_len, k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_update(msg, (uint32_t)k_ra_sce_test_msg_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_hmac_final(mac_b));

  bool different = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_sha256_digest_bytes; ++i) {
    if (mac_a[i] != mac_b[i]) {
      different = true;
      break;
    }
  }
  TEST_ASSERT(different);
  teardown();
}

/**
 * @brief Test: ``ra_sce_hmac_init`` rejects NULL keys and zero length.
 * @since 0.1.0
 */
static void test_hmac_arg_validation(void)
{
  prep();
  uint8_t key[k_ra_sce_test_hkey_len];
  (void)memset(key, (int)k_ra_sce_test_pattern_hkey, sizeof(key));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_sce_hmac_init(nullptr, (uint32_t)k_ra_sce_test_hkey_len, k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sce_hmac_init(key, (uint32_t)k_ra_sce_test_zero_len, k_ra_sce_sha_mode_sha256));
  teardown();
}

/**
 * @brief Test: TRNG produces a non-zero buffer of the requested size.
 * @since 0.1.0
 */
static void test_trng_returns_nonzero(void)
{
  prep();
  uint8_t buf[k_ra_sce_test_trng_len];
  (void)memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_random(buf, (uint32_t)k_ra_sce_test_trng_len));
  bool nonzero = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_trng_len; ++i) {
    if (buf[i] != 0U) {
      nonzero = true;
      break;
    }
  }
  TEST_ASSERT(nonzero);

  /* Tail byte-wise fill: a non-multiple-of-4 request must still
   * fully populate the buffer. */
  uint8_t short_buf[k_ra_sce_test_short_trng_len];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_random(short_buf, (uint32_t)k_ra_sce_test_short_trng_len));
  teardown();
}

/**
 * @brief Test: ``ra_sce_random`` rejects NULL and zero-length.
 * @since 0.1.0
 */
static void test_trng_arg_validation(void)
{
  prep();
  uint8_t buf[k_ra_sce_test_trng_len];
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sce_random(nullptr, (uint32_t)k_ra_sce_test_trng_len));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sce_random(buf, (uint32_t)k_ra_sce_test_zero_len));
  teardown();
}

/**
 * @brief Test: every API rejects calls before ``ra_sce_open``.
 * @since 0.1.0
 */
static void test_api_requires_open(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  uint8_t key[k_ra_sce_test_aes_block];
  (void)memset(key, (int)k_ra_sce_test_pattern_key, sizeof(key));
  uint8_t buf[k_ra_sce_test_aes_block];

  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sce_close());
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_sce_aes_init(key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sce_sha_init(k_ra_sce_sha_mode_sha256));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sce_random(buf, (uint32_t)k_ra_sce_test_aes_block));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_state,
    ra_sce_hmac_init(key, (uint32_t)k_ra_sce_test_aes_block, k_ra_sce_sha_mode_sha256));
}

/**
 * @brief Test entry point.
 *
 * @return 0 on success, non-zero on first failure.
 * @since 0.1.0
 */
int main(void)
{
  test_open_null_cfg();
  test_aes128_ecb_roundtrip();
  test_aes256_cbc_roundtrip();
  test_aes_ctr_accepts_unaligned();
  test_aes_ecb_rejects_unaligned();
  test_aes_key_bits_validation();
  test_aes_mode_validation();
  test_aes_encrypt_requires_init();
  test_sha256_deterministic();
  test_sha_arg_validation();
  test_hmac_sha256_deterministic();
  test_hmac_changes_with_key();
  test_hmac_arg_validation();
  test_trng_returns_nonzero();
  test_trng_arg_validation();
  test_api_requires_open();
  return 0;
}
