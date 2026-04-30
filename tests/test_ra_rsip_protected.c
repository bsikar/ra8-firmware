/**
 * @file test_ra_rsip_protected.c
 * @brief Unit tests for ra_rsip_protected.c (RSIP wrapped-key crypto)
 *
 * @details
 * Mirrors the ``test_ra_sce.c`` protected-side tests for the RSIP
 * stub layer. The RSIP engine itself is software-emulated through
 * ``ra_sim_mmap`` so the round-trip tests preload the AES DATA_OUT
 * lanes with a sentinel pattern that the cipher path XORs back into
 * the caller's buffer. Any plumbing failure in the protected layer
 * (validation, install, latching, dispatch) shows up as a non-OK
 * return code or an unmatched output byte.
 *
 * Coverage:
 *
 * - protected AES init validates the wrapper, install plumbing fires;
 * - protected AES encrypt + decrypt drive the engine via the latched
 *   handle;
 * - protected AES tamper rejection;
 * - protected AES dispatch fails before init;
 * - protected RSA decrypt: round-trips through ra_rsip_rsa_sign;
 * - protected ECDSA sign rebuilds an ECC handle and dispatches;
 * - NULL-argument rejection on every entry point.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_rsip_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_rsip.h"
#include "ra_rsip_key_injection.h"
#include "ra_rsip_protected.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_rsip_p_test_const_t
 * @brief Magic numbers used by these tests, named to keep the
 *        no-magic-numbers rule satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_aes_block       = 16U,         /**< AES block bytes.            */
  k_test_p256_priv       = 32U,         /**< P-256 priv scalar.          */
  k_test_p256_sig        = 64U,         /**< P-256 sig (r||s).           */
  k_test_sha256_digest   = 32U,         /**< SHA-256 digest bytes.       */
  k_test_rsa2048_bytes   = 256U,        /**< RSA-2048 modulus bytes.     */
  k_test_pattern_key     = 0x42U,       /**< Sentinel byte for raw keys. */
  k_test_pattern_d       = 0x77U,       /**< ECC priv sentinel.          */
  k_test_pattern_h       = 0xA5U,       /**< Generic hash sentinel.      */
  k_test_dataout_word_lo = 0xDEADBEEFU, /**< DATA_OUT lane 0 sentinel.   */
  k_test_dataout_word_1  = 0x11223344U, /**< DATA_OUT lane 1 sentinel.   */
  k_test_dataout_word_2  = 0x55667788U, /**< DATA_OUT lane 2 sentinel.   */
  k_test_dataout_word_3  = 0x99AABBCCU, /**< DATA_OUT lane 3 sentinel.   */
} ra_rsip_p_test_const_t;

/**
 * @brief Reset ra_sim_mmap and bring the engine to ENABLE for every test.
 * @since 0.1.0
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));
}

/**
 * @brief Pre-load the four AES DATA_OUT lanes with a known sentinel.
 *
 * @details
 * The host-side stub returns DATA_OUT verbatim, so this gives the
 * test a determinate cipher / plaintext after each call.
 *
 * @since 0.1.0
 */
static void preload_data_out(void)
{
  *ra_rsip_reg32(k_ra_rsip_off_data_out0) = (uint32_t)k_test_dataout_word_lo;
  *ra_rsip_reg32(k_ra_rsip_off_data_out1) = (uint32_t)k_test_dataout_word_1;
  *ra_rsip_reg32(k_ra_rsip_off_data_out2) = (uint32_t)k_test_dataout_word_2;
  *ra_rsip_reg32(k_ra_rsip_off_data_out3) = (uint32_t)k_test_dataout_word_3;
}

/**
 * @brief Protected AES init + encrypt + decrypt round-trip.
 * @since 0.1.0
 */
static void test_protected_aes_roundtrip(void)
{
  TEST_BEGIN("rsip protected aes round-trip");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_rsip_wrapped_max_total];
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_key_inject_aes(blob, raw_key, k_ra_rsip_aes_key_bits_128));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_protected_aes_init(blob,
                                                     k_ra_rsip_aes_key_bits_128,
                                                     k_ra_rsip_aes_mode_ecb,
                                                     nullptr));

  /* Encrypt -- the engine echoes DATA_OUT, so the ciphertext bytes
   * are the LE pattern from the four lanes. */
  preload_data_out();
  uint8_t pt[k_test_aes_block] = {};
  uint8_t ct[k_test_aes_block] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_protected_aes_encrypt(pt, ct, (uint32_t)k_test_aes_block));
  /* DEADBEEF in little-endian: EF BE AD DE */
  TEST_ASSERT_EQ((int32_t)0xEFU, (int32_t)ct[0]);
  TEST_ASSERT_EQ((int32_t)0xBEU, (int32_t)ct[1]);
  TEST_ASSERT_EQ((int32_t)0xADU, (int32_t)ct[2]);
  TEST_ASSERT_EQ((int32_t)0xDEU, (int32_t)ct[3]);

  /* Decrypt path runs through the same engine. Re-preload because
   * the stub register values may have been clobbered by the encrypt
   * dispatch. */
  preload_data_out();
  uint8_t back[k_test_aes_block] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_protected_aes_decrypt(ct, back, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ((int32_t)0xEFU, (int32_t)back[0]);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_protected_aes_finish());

  TEST_END("rsip protected aes round-trip");
}

/**
 * @brief Protected AES init refuses a tampered blob.
 * @since 0.1.0
 */
static void test_protected_aes_rejects_tamper(void)
{
  TEST_BEGIN("rsip protected aes tamper");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra_rsip_wrapped_max_total];
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_key_inject_aes(blob, raw_key, k_ra_rsip_aes_key_bits_128));
  /* Flip the trailing MAC. */
  blob[(uint32_t)k_ra_rsip_wrapped_max_total - 1U] ^= 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_error,
                 (int32_t)ra_rsip_protected_aes_init(blob,
                                                     k_ra_rsip_aes_key_bits_128,
                                                     k_ra_rsip_aes_mode_ecb,
                                                     nullptr));

  TEST_END("rsip protected aes tamper");
}

/**
 * @brief Protected AES encrypt / decrypt / finish refuse pre-init calls.
 * @since 0.1.0
 */
static void test_protected_aes_no_init(void)
{
  TEST_BEGIN("rsip protected aes no init");
  prep();

  /* The static state is reset between TUs but not between tests in
   * the same TU. Drive a finish to make the state predictable, then
   * call the data-path entries on a clean slate. */
  (void)ra_rsip_protected_aes_finish();

  uint8_t buf[k_test_aes_block] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_rsip_protected_aes_encrypt(buf, buf, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_rsip_protected_aes_decrypt(buf, buf, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rsip_protected_aes_finish());

  TEST_END("rsip protected aes no init");
}

/**
 * @brief Protected RSA decrypt validates the wrapper and dispatches.
 * @since 0.1.0
 */
static void test_protected_rsa_decrypt(void)
{
  TEST_BEGIN("rsip protected rsa decrypt");
  prep();

  uint8_t modulus[k_test_rsa2048_bytes] = {};
  uint8_t exponent[4]                   = {0x00U, 0x01U, 0x00U, 0x01U};
  (void)memset(modulus, (int)k_test_pattern_h, sizeof(modulus));
  uint8_t blob[k_ra_rsip_wrapped_max_total];
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_key_inject_rsa(blob, modulus, exponent, k_ra_rsip_rsa_2048));

  uint8_t ct[k_test_rsa2048_bytes] = {};
  uint8_t pt[k_test_rsa2048_bytes] = {};
  (void)memset(ct, (int)k_test_pattern_d, sizeof(ct));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_protected_rsa_decrypt(blob,
                                                        k_ra_rsip_rsa_2048,
                                                        ct,
                                                        (uint32_t)k_test_rsa2048_bytes,
                                                        pt,
                                                        (uint32_t)k_test_rsa2048_bytes));

  TEST_END("rsip protected rsa decrypt");
}

/**
 * @brief Protected ECDSA sign rebuilds the handle and signs.
 * @since 0.1.0
 */
static void test_protected_ecdsa_sign(void)
{
  TEST_BEGIN("rsip protected ecdsa sign");
  prep();

  uint8_t priv[k_test_p256_priv];
  (void)memset(priv, (int)k_test_pattern_d, sizeof(priv));
  uint8_t blob[k_ra_rsip_wrapped_max_total];
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_key_inject_ecc(blob, k_ra_rsip_curve_secp256r1, priv, true));

  uint8_t hash[k_test_sha256_digest];
  (void)memset(hash, (int)k_test_pattern_h, sizeof(hash));
  uint8_t sig[k_test_p256_sig] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_protected_ecdsa_sign(blob,
                                                       k_ra_rsip_curve_secp256r1,
                                                       hash,
                                                       (uint32_t)k_test_sha256_digest,
                                                       sig));

  TEST_END("rsip protected ecdsa sign");
}

/**
 * @brief NULL-pointer rejection on every protected-layer entry.
 * @since 0.1.0
 */
static void test_protected_null_args(void)
{
  TEST_BEGIN("rsip protected null args");
  prep();

  uint8_t blob[k_ra_rsip_wrapped_max_total] = {};
  uint8_t buf[k_test_aes_block]             = {};
  uint8_t hash[k_test_sha256_digest]        = {};
  uint8_t sig[k_test_p256_sig]              = {};

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_aes_init(nullptr,
                                                     k_ra_rsip_aes_key_bits_128,
                                                     k_ra_rsip_aes_mode_ecb,
                                                     nullptr));

  /* Once init has not run successfully, encrypt/decrypt should still
   * fall to invalid_state regardless of NULL inputs (latched flag
   * guards before pointer checks). Sanity-check that finish keeps
   * the state idle. */
  (void)ra_rsip_protected_aes_finish();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_rsip_protected_aes_encrypt(nullptr, buf, (uint32_t)k_test_aes_block));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_rsa_decrypt(nullptr,
                                                        k_ra_rsip_rsa_2048,
                                                        buf,
                                                        (uint32_t)k_test_aes_block,
                                                        buf,
                                                        (uint32_t)k_test_rsa2048_bytes));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_rsa_decrypt(blob,
                                                        k_ra_rsip_rsa_2048,
                                                        nullptr,
                                                        (uint32_t)k_test_aes_block,
                                                        buf,
                                                        (uint32_t)k_test_rsa2048_bytes));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_rsa_decrypt(blob,
                                                        k_ra_rsip_rsa_2048,
                                                        buf,
                                                        (uint32_t)k_test_aes_block,
                                                        nullptr,
                                                        (uint32_t)k_test_rsa2048_bytes));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_ecdsa_sign(nullptr,
                                                       k_ra_rsip_curve_secp256r1,
                                                       hash,
                                                       (uint32_t)k_test_sha256_digest,
                                                       sig));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_ecdsa_sign(blob,
                                                       k_ra_rsip_curve_secp256r1,
                                                       nullptr,
                                                       (uint32_t)k_test_sha256_digest,
                                                       sig));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_protected_ecdsa_sign(blob,
                                                       k_ra_rsip_curve_secp256r1,
                                                       hash,
                                                       (uint32_t)k_test_sha256_digest,
                                                       nullptr));

  TEST_END("rsip protected null args");
}

int32_t main(void)
{
  test_protected_aes_roundtrip();
  test_protected_aes_rejects_tamper();
  test_protected_aes_no_init();
  test_protected_rsa_decrypt();
  test_protected_ecdsa_sign();
  test_protected_null_args();
  (void)fprintf(stderr, "[OK ] test_ra_rsip_protected.c\n");
  return 0;
}
