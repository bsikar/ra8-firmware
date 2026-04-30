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

/* ---------------------------------------------------------------------------
 * RSA + ECC test fixtures.
 * --------------------------------------------------------------------------- */

/**
 * @enum ra_sce_test_pk_const_t
 * @brief Sizing constants for the RSA / ECC tests.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_test_rsa2048_bytes = 256U,  /**< RSA-2048 modulus.            */
  k_ra_sce_test_rsa_e_bytes   = 4U,    /**< RSA public-exponent bytes.   */
  k_ra_sce_test_rsa_msg_len   = 64U,   /**< RSA test message length.     */
  k_ra_sce_test_ecc_p256_priv = 32U,   /**< P-256 private bytes.         */
  k_ra_sce_test_ecc_p256_pub  = 64U,   /**< P-256 public bytes.          */
  k_ra_sce_test_ecc_p256_sig  = 64U,   /**< P-256 signature bytes.       */
  k_ra_sce_test_pattern_n     = 0xC3U, /**< Modulus pattern.             */
  k_ra_sce_test_pattern_d     = 0x4DU, /**< Private-exp pattern.         */
  k_ra_sce_test_pattern_h     = 0x99U, /**< Hash pattern.                */
  k_ra_sce_test_e_byte_lo     = 0x01U, /**< Public exp low byte.         */
  k_ra_sce_test_e_byte_hi     = 0x00U, /**< Public exp upper bytes.      */
} ra_sce_test_pk_const_t;

/**
 * @brief Test: RSA encrypt + decrypt round-trip recovers the message.
 * @since 0.1.0
 */
static void test_rsa_encrypt_decrypt_roundtrip(void)
{
  prep();
  uint8_t modulus[k_ra_sce_test_rsa2048_bytes];
  uint8_t priv_d[k_ra_sce_test_rsa2048_bytes];
  uint8_t pub_e[k_ra_sce_test_rsa_e_bytes] = {k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo,
                                              k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo};
  (void)memset(modulus, (int)k_ra_sce_test_pattern_n, sizeof(modulus));
  (void)memset(priv_d, (int)k_ra_sce_test_pattern_d, sizeof(priv_d));

  uint8_t plain[k_ra_sce_test_rsa_msg_len];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_rsa_msg_len; ++i) {
    plain[i] = (uint8_t)i;
  }

  const ra_sce_rsa_pub_t  pub  = {.key_bits = k_ra_sce_rsa_key_bits_2048,
                                  .modulus  = modulus,
                                  .exponent = pub_e};
  const ra_sce_rsa_priv_t priv = {.key_bits    = k_ra_sce_rsa_key_bits_2048,
                                  .modulus     = modulus,
                                  .private_exp = priv_d};

  uint8_t  ciphertext[k_ra_sce_test_rsa2048_bytes];
  uint8_t  recovered[k_ra_sce_test_rsa2048_bytes];
  uint32_t out_len = (uint32_t)k_ra_sce_test_rsa2048_bytes;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_rsa_public_encrypt(&pub,
                                           plain,
                                           (uint32_t)k_ra_sce_test_rsa_msg_len,
                                           ciphertext,
                                           (uint32_t)k_ra_sce_test_rsa2048_bytes));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_rsa_private_decrypt(&priv,
                                            ciphertext,
                                            (uint32_t)k_ra_sce_test_rsa2048_bytes,
                                            recovered,
                                            &out_len));

  /* Plaintext is right-aligned in a modulus-sized buffer. */
  const uint32_t offset =
    (uint32_t)k_ra_sce_test_rsa2048_bytes - (uint32_t)k_ra_sce_test_rsa_msg_len;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_rsa_msg_len; ++i) {
    TEST_ASSERT_EQ(plain[i], recovered[offset + i]);
  }
  teardown();
}

/**
 * @brief Test: RSA sign + verify round-trip succeeds.
 * @since 0.1.0
 */
static void test_rsa_sign_verify_roundtrip(void)
{
  prep();
  uint8_t modulus[k_ra_sce_test_rsa2048_bytes];
  uint8_t priv_d[k_ra_sce_test_rsa2048_bytes];
  uint8_t pub_e[k_ra_sce_test_rsa_e_bytes] = {k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo,
                                              k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo};
  (void)memset(modulus, (int)k_ra_sce_test_pattern_n, sizeof(modulus));
  (void)memset(priv_d, (int)k_ra_sce_test_pattern_d, sizeof(priv_d));

  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  (void)memset(hash, (int)k_ra_sce_test_pattern_h, sizeof(hash));

  const ra_sce_rsa_pub_t  pub  = {.key_bits = k_ra_sce_rsa_key_bits_2048,
                                  .modulus  = modulus,
                                  .exponent = pub_e};
  const ra_sce_rsa_priv_t priv = {.key_bits    = k_ra_sce_rsa_key_bits_2048,
                                  .modulus     = modulus,
                                  .private_exp = priv_d};
  uint8_t                 signature[k_ra_sce_test_rsa2048_bytes];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_rsa_private_sign(&priv,
                                         hash,
                                         (uint32_t)k_ra_sce_sha256_digest_bytes,
                                         signature,
                                         (uint32_t)k_ra_sce_test_rsa2048_bytes));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_rsa_public_verify(&pub,
                                          hash,
                                          (uint32_t)k_ra_sce_sha256_digest_bytes,
                                          signature,
                                          (uint32_t)k_ra_sce_test_rsa2048_bytes));
  teardown();
}

/**
 * @brief Test: RSA verify rejects a tampered signature.
 * @since 0.1.0
 */
static void test_rsa_verify_rejects_tamper(void)
{
  prep();
  uint8_t modulus[k_ra_sce_test_rsa2048_bytes];
  uint8_t priv_d[k_ra_sce_test_rsa2048_bytes];
  uint8_t pub_e[k_ra_sce_test_rsa_e_bytes] = {k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo,
                                              k_ra_sce_test_e_byte_hi,
                                              k_ra_sce_test_e_byte_lo};
  (void)memset(modulus, (int)k_ra_sce_test_pattern_n, sizeof(modulus));
  (void)memset(priv_d, (int)k_ra_sce_test_pattern_d, sizeof(priv_d));

  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  (void)memset(hash, (int)k_ra_sce_test_pattern_h, sizeof(hash));

  const ra_sce_rsa_pub_t  pub  = {.key_bits = k_ra_sce_rsa_key_bits_2048,
                                  .modulus  = modulus,
                                  .exponent = pub_e};
  const ra_sce_rsa_priv_t priv = {.key_bits    = k_ra_sce_rsa_key_bits_2048,
                                  .modulus     = modulus,
                                  .private_exp = priv_d};
  uint8_t                 signature[k_ra_sce_test_rsa2048_bytes];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_rsa_private_sign(&priv,
                                         hash,
                                         (uint32_t)k_ra_sce_sha256_digest_bytes,
                                         signature,
                                         (uint32_t)k_ra_sce_test_rsa2048_bytes));
  signature[0] ^= 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_sce_rsa_public_verify(&pub,
                                          hash,
                                          (uint32_t)k_ra_sce_sha256_digest_bytes,
                                          signature,
                                          (uint32_t)k_ra_sce_test_rsa2048_bytes));
  teardown();
}

/**
 * @brief Test: RSA encrypt rejects NULL inputs.
 * @since 0.1.0
 */
static void test_rsa_encrypt_arg_validation(void)
{
  prep();
  uint8_t buf[k_ra_sce_test_rsa2048_bytes];
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sce_rsa_public_encrypt(nullptr,
                                           buf,
                                           (uint32_t)k_ra_sce_test_rsa_msg_len,
                                           buf,
                                           (uint32_t)k_ra_sce_test_rsa2048_bytes));
  teardown();
}

/**
 * @brief Test: ECC keygen produces non-zero priv + pub.
 * @since 0.1.0
 */
static void test_ecc_keygen_p256(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  (void)memset(priv, 0, sizeof(priv));
  (void)memset(pub, 0, sizeof(pub));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pub));
  bool priv_nonzero = false;
  bool pub_nonzero  = false;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_ecc_p256_priv; ++i) {
    if (priv[i] != 0U) {
      priv_nonzero = true;
      break;
    }
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_ecc_p256_pub; ++i) {
    if (pub[i] != 0U) {
      pub_nonzero = true;
      break;
    }
  }
  TEST_ASSERT(priv_nonzero);
  TEST_ASSERT(pub_nonzero);
  teardown();
}

/**
 * @brief Test: ECDSA sign + verify round-trip on P-256.
 * @since 0.1.0
 */
static void test_ecc_ecdsa_sign_verify_p256(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pub));

  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  (void)memset(hash, (int)k_ra_sce_test_pattern_h, sizeof(hash));

  uint8_t sig[k_ra_sce_test_ecc_p256_sig];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_ecc_ecdsa_sign(k_ra_sce_ecc_curve_p256,
                                       priv,
                                       hash,
                                       (uint32_t)k_ra_sce_sha256_digest_bytes,
                                       sig));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_ecc_ecdsa_verify(k_ra_sce_ecc_curve_p256,
                                         pub,
                                         hash,
                                         (uint32_t)k_ra_sce_sha256_digest_bytes,
                                         sig));
  teardown();
}

/**
 * @brief Test: ECDSA verify rejects a tampered signature.
 * @since 0.1.0
 */
static void test_ecc_ecdsa_rejects_tamper(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pub));
  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  (void)memset(hash, (int)k_ra_sce_test_pattern_h, sizeof(hash));
  uint8_t sig[k_ra_sce_test_ecc_p256_sig];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_ecc_ecdsa_sign(k_ra_sce_ecc_curve_p256,
                                       priv,
                                       hash,
                                       (uint32_t)k_ra_sce_sha256_digest_bytes,
                                       sig));
  /* Tamper with the s component (second half). */
  sig[(uint32_t)k_ra_sce_test_ecc_p256_priv] ^= 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 ra_sce_ecc_ecdsa_verify(k_ra_sce_ecc_curve_p256,
                                         pub,
                                         hash,
                                         (uint32_t)k_ra_sce_sha256_digest_bytes,
                                         sig));
  teardown();
}

/**
 * @brief Test: ECDH derives the same shared secret for fixed inputs.
 * @since 0.1.0
 */
static void test_ecc_ecdh_deterministic(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pub));

  uint8_t shared_a[k_ra_sce_test_ecc_p256_priv];
  uint8_t shared_b[k_ra_sce_test_ecc_p256_priv];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_ecdh(k_ra_sce_ecc_curve_p256, priv, pub, shared_a));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_ecdh(k_ra_sce_ecc_curve_p256, priv, pub, shared_b));
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_ecc_p256_priv; ++i) {
    TEST_ASSERT_EQ(shared_a[i], shared_b[i]);
  }
  teardown();
}

/**
 * @brief Test: ECC keygen rejects unknown curve.
 * @since 0.1.0
 */
static void test_ecc_keygen_rejects_bad_curve(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sce_ecc_keygen((ra_sce_ecc_curve_t)k_ra_sce_test_bad_mode, priv, pub));
  teardown();
}

#include "ra_sce_key_injection.h"
#include "ra_sce_protected.h"

/**
 * @brief Test: AES key injection produces a valid blob.
 * @since 0.1.0
 */
static void test_key_inject_aes(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_aes(blob, raw_key, k_ra_sce_aes_key_bits_128));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_validate(blob, k_ra_sce_wrapped_type_aes));
  teardown();
}

/**
 * @brief Test: AES key injection rejects unsupported key width.
 * @since 0.1.0
 */
static void test_key_inject_aes_bad_width(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_sce_key_inject_aes(blob, raw_key, (ra_sce_aes_key_bits_t)k_ra_sce_test_bad_key_bits));
  teardown();
}

/**
 * @brief Test: tampering with a wrapped blob trips ``ra_sce_key_validate``.
 * @since 0.1.0
 */
static void test_key_validate_detects_tamper(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_aes(blob, raw_key, k_ra_sce_aes_key_bits_128));
  blob[(uint32_t)k_ra_sce_wrapped_max_total - 1U] ^= 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_sce_key_validate(blob, k_ra_sce_wrapped_type_aes));
  teardown();
}

/**
 * @brief Test: validating a blob with the wrong type tag fails.
 * @since 0.1.0
 */
static void test_key_validate_wrong_type(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_aes(blob, raw_key, k_ra_sce_aes_key_bits_128));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sce_key_validate(blob, k_ra_sce_wrapped_type_ecc_priv));
  teardown();
}

/**
 * @brief Test: ECC private key injection round-trips through validate.
 * @since 0.1.0
 */
static void test_key_inject_ecc_private(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  (void)memset(priv, (int)k_ra_sce_test_pattern_d, sizeof(priv));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_ecc(blob, k_ra_sce_ecc_curve_p256, priv, true));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_validate(blob, k_ra_sce_wrapped_type_ecc_priv));
  teardown();
}

/**
 * @brief Test: ECC public key injection round-trips through validate.
 * @since 0.1.0
 */
static void test_key_inject_ecc_public(void)
{
  prep();
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  (void)memset(pub, (int)k_ra_sce_test_pattern_h, sizeof(pub));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_ecc(blob, k_ra_sce_ecc_curve_p256, pub, false));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_validate(blob, k_ra_sce_wrapped_type_ecc_pub));
  teardown();
}

/**
 * @brief Test: protected AES encrypt/decrypt round-trip via wrapped key.
 * @since 0.1.0
 */
static void test_protected_aes_roundtrip(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_aes(blob, raw_key, k_ra_sce_aes_key_bits_128));

  uint8_t plain[k_ra_sce_test_two_blocks];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    plain[i] = (uint8_t)i;
  }
  uint8_t cipher[k_ra_sce_test_two_blocks];
  uint8_t back[k_ra_sce_test_two_blocks];

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sce_protected_aes_init(blob, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_protected_aes_encrypt(plain, cipher, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_protected_aes_finish(nullptr));

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sce_protected_aes_init(blob, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_protected_aes_decrypt(cipher, back, (uint32_t)k_ra_sce_test_two_blocks));
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_protected_aes_finish(nullptr));

  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_test_two_blocks; ++i) {
    TEST_ASSERT_EQ(plain[i], back[i]);
  }
  teardown();
}

/**
 * @brief Test: protected AES init refuses a tampered blob.
 * @since 0.1.0
 */
static void test_protected_aes_rejects_tamper(void)
{
  prep();
  uint8_t raw_key[k_ra_sce_test_aes_block];
  (void)memset(raw_key, (int)k_ra_sce_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_aes(blob, raw_key, k_ra_sce_aes_key_bits_128));
  blob[(uint32_t)k_ra_sce_wrapped_max_total - 1U] ^= 0xFFU;
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_sce_protected_aes_init(blob, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_ecb, nullptr));
  teardown();
}

/**
 * @brief Test: protected ECDSA sign uses a wrapped private key.
 * @since 0.1.0
 */
static void test_protected_ecdsa_sign(void)
{
  prep();
  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pub[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pub));

  uint8_t blob[k_ra_sce_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra_ok, ra_sce_key_inject_ecc(blob, k_ra_sce_ecc_curve_p256, priv, true));

  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  (void)memset(hash, (int)k_ra_sce_test_pattern_h, sizeof(hash));
  uint8_t sig[k_ra_sce_test_ecc_p256_sig];
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_protected_ecdsa_sign(blob,
                                             k_ra_sce_ecc_curve_p256,
                                             hash,
                                             (uint32_t)k_ra_sce_sha256_digest_bytes,
                                             sig));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sce_ecc_ecdsa_verify(k_ra_sce_ecc_curve_p256,
                                         pub,
                                         hash,
                                         (uint32_t)k_ra_sce_sha256_digest_bytes,
                                         sig));
  teardown();
}

/**
 * @brief Test: protected ECDSA sign with a NULL blob errors.
 * @since 0.1.0
 */
static void test_protected_ecdsa_null_blob(void)
{
  prep();
  uint8_t hash[k_ra_sce_sha256_digest_bytes];
  uint8_t sig[k_ra_sce_test_ecc_p256_sig];
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sce_protected_ecdsa_sign(nullptr,
                                             k_ra_sce_ecc_curve_p256,
                                             hash,
                                             (uint32_t)k_ra_sce_sha256_digest_bytes,
                                             sig));
  teardown();
}

/**
 * @brief Test: every new RSA / ECC API rejects calls before
 *        ``ra_sce_open``.
 * @since 0.1.0
 */
static void test_pk_api_requires_open(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  uint8_t                modulus[k_ra_sce_test_rsa2048_bytes] = {};
  uint8_t                exponent[k_ra_sce_test_rsa_e_bytes]  = {};
  const ra_sce_rsa_pub_t pub = {.key_bits = k_ra_sce_rsa_key_bits_2048,
                                .modulus  = modulus,
                                .exponent = exponent};
  uint8_t                buf[k_ra_sce_test_rsa2048_bytes];
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_sce_rsa_public_encrypt(&pub,
                                           buf,
                                           (uint32_t)k_ra_sce_test_rsa_msg_len,
                                           buf,
                                           (uint32_t)k_ra_sce_test_rsa2048_bytes));

  uint8_t priv[k_ra_sce_test_ecc_p256_priv];
  uint8_t pubk[k_ra_sce_test_ecc_p256_pub];
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sce_ecc_keygen(k_ra_sce_ecc_curve_p256, priv, pubk));
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
  test_rsa_encrypt_decrypt_roundtrip();
  test_rsa_sign_verify_roundtrip();
  test_rsa_verify_rejects_tamper();
  test_rsa_encrypt_arg_validation();
  test_ecc_keygen_p256();
  test_ecc_ecdsa_sign_verify_p256();
  test_ecc_ecdsa_rejects_tamper();
  test_ecc_ecdh_deterministic();
  test_ecc_keygen_rejects_bad_curve();
  test_key_inject_aes();
  test_key_inject_aes_bad_width();
  test_key_validate_detects_tamper();
  test_key_validate_wrong_type();
  test_key_inject_ecc_private();
  test_key_inject_ecc_public();
  test_protected_aes_roundtrip();
  test_protected_aes_rejects_tamper();
  test_protected_ecdsa_sign();
  test_protected_ecdsa_null_blob();
  test_pk_api_requires_open();
  return 0;
}
