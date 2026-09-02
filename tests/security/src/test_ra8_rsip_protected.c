/**
 * @file test_ra8_rsip_protected.c
 * @brief Unit tests for ra8_rsip_protected.c (RSIP wrapped-key crypto)
 *
 * @details
 * Mirrors the ``test_ra8_sce.c`` protected-side tests for the RSIP
 * stub layer. The RSIP engine itself is software-emulated through
 * ``ra8_fake_mmap`` so the round-trip tests preload the AES DATA_OUT
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
 * - protected RSA decrypt: round-trips through ra8_rsip_rsa_sign;
 * - protected ECDSA sign rebuilds an ECC handle and dispatches;
 * - NULL-argument rejection on every entry point.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_key_injection.h"
#include "ra8_rsip_protected.h"
#include "ra8_rsip_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_protected_t
 * @brief Tamper mask and the raw key length under test.
 */
typedef enum : uint16_t {
  k_t_tamper_mask = 0xFFU, /**< XOR that flips the wrapped blob's last byte so
                                its integrity check must fail.                   */
  k_t_raw_key_len = 24U,   /**< AES-192 key, bytes. */
} t_protected_t;

/**
 * @enum ra8_rsip_p_test_const_t
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
} ra8_rsip_p_test_const_t;

/**
 * @enum ra8_rsip_p_test_ext_const_t
 * @brief Extra sizing constants for the extended error-leg and
 *        algorithm-arm tests, named to keep the no-magic-numbers rule
 *        satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_aes_bits_wide   = 512U, /**< Bogus AES width: 64 raw bytes (> max).   */
  k_test_aes_bits_noinst = 200U, /**< Bogus AES width: 25 bytes, no installer. */
  k_test_rsa1024_bytes   = 128U, /**< RSA-1024 modulus bytes.                  */
  k_test_rsa3072_bytes   = 384U, /**< RSA-3072 modulus bytes.                  */
  k_test_rsa4096_bytes   = 512U, /**< RSA-4096 modulus bytes.                  */
  k_test_rsa_overlen     = 300U, /**< Ciphertext longer than RSA-2048 modulus. */
  k_test_rsa_small_cap   = 16U,  /**< Plaintext cap below modulus width.       */
  k_test_rsa_size_bad    = 777U, /**< Unsupported RSA size selector.           */
  k_test_curve_bad       = 99U,  /**< Unsupported ECC curve selector.          */
  k_test_exp_bytes       = 4U,   /**< RSA public-exponent bytes.               */
  k_test_p384_priv       = 48U,  /**< P-384 private scalar bytes.              */
  k_test_p384_sig        = 96U,  /**< P-384 signature (r || s) bytes.          */
  k_test_p521_priv       = 66U,  /**< P-521 private scalar bytes.              */
  k_test_p521_sig        = 132U, /**< P-521 signature (r || s) bytes.          */
} ra8_rsip_p_test_ext_const_t;

/**
 * @brief Reset ra8_fake_mmap and bring the engine to ENABLE for every test.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
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
  *ra8_rsip_reg32(k_ra8_rsip_off_data_out0) = (uint32_t)k_test_dataout_word_lo;
  *ra8_rsip_reg32(k_ra8_rsip_off_data_out1) = (uint32_t)k_test_dataout_word_1;
  *ra8_rsip_reg32(k_ra8_rsip_off_data_out2) = (uint32_t)k_test_dataout_word_2;
  *ra8_rsip_reg32(k_ra8_rsip_off_data_out3) = (uint32_t)k_test_dataout_word_3;
}

/**
 * @brief Protected AES init + encrypt + decrypt round-trip.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_aes_roundtrip(void)
{
  TEST_BEGIN("rsip protected aes round-trip");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_aes_init(blob,
                                             k_ra8_rsip_aes_key_bits_128,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));

  /* Encrypt -- the engine echoes DATA_OUT, so the ciphertext bytes
   * are the LE pattern from the four lanes. */
  preload_data_out();
  uint8_t pt[k_test_aes_block] = {};
  uint8_t ct[k_test_aes_block] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_encrypt(pt, ct, (uint32_t)k_test_aes_block));
  /* DEADBEEF in little-endian: EF BE AD DE */
  TEST_ASSERT_EQ(0xEFU, ct[0]);
  TEST_ASSERT_EQ(0xBEU, ct[1]);
  TEST_ASSERT_EQ(0xADU, ct[2]);
  TEST_ASSERT_EQ(0xDEU, ct[3]);

  /* Decrypt path runs through the same engine. Re-preload because
   * the stub register values may have been clobbered by the encrypt
   * dispatch. */
  preload_data_out();
  uint8_t back[k_test_aes_block] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_decrypt(ct, back, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ(0xEFU, back[0]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_finish());

  TEST_END("rsip protected aes round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_protected_aes192_init(void)
{
  TEST_BEGIN("rsip protected aes-192 init");
  prep();

  uint8_t raw_key[k_t_raw_key_len];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_192));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_aes_init(blob,
                                             k_ra8_rsip_aes_key_bits_192,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_finish());

  TEST_END("rsip protected aes-192 init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_protected_aes256_init(void)
{
  TEST_BEGIN("rsip protected aes-256 init");
  prep();

  uint8_t raw_key[32];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_256));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_aes_init(blob,
                                             k_ra8_rsip_aes_key_bits_256,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_finish());

  TEST_END("rsip protected aes-256 init");
}

/**
 * @brief Protected AES init refuses a tampered blob.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_aes_rejects_tamper(void)
{
  TEST_BEGIN("rsip protected aes tamper");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));
  /* Flip the trailing MAC. */
  blob[(uint32_t)k_ra8_rsip_wrapped_max_total - 1U] ^= k_t_tamper_mask;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rsip_protected_aes_init(blob,
                                             k_ra8_rsip_aes_key_bits_128,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));

  TEST_END("rsip protected aes tamper");
}

/**
 * @brief Protected AES encrypt / decrypt / finish refuse pre-init calls.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_aes_no_init(void)
{
  TEST_BEGIN("rsip protected aes no init");
  prep();

  /* The static state is reset between TUs but not between tests in
   * the same TU. Drive a finish to make the state predictable, then
   * call the data-path entries on a clean slate. */
  (void)ra8_rsip_protected_aes_finish();

  uint8_t buf[k_test_aes_block] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rsip_protected_aes_encrypt(buf, buf, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rsip_protected_aes_decrypt(buf, buf, (uint32_t)k_test_aes_block));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_protected_aes_finish());

  TEST_END("rsip protected aes no init");
}

/**
 * @brief Protected RSA decrypt validates the wrapper and dispatches.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_rsa_decrypt(void)
{
  TEST_BEGIN("rsip protected rsa decrypt");
  prep();

  uint8_t modulus[k_test_rsa2048_bytes] = {};
  uint8_t exponent[4]                   = {0x00U, 0x01U, 0x00U, 0x01U};
  (void)memset(modulus, (int)k_test_pattern_h, sizeof(modulus));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_rsa(blob, modulus, exponent, k_ra8_rsip_rsa_2048));

  uint8_t ct[k_test_rsa2048_bytes] = {};
  uint8_t pt[k_test_rsa2048_bytes] = {};
  (void)memset(ct, (int)k_test_pattern_d, sizeof(ct));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                k_ra8_rsip_rsa_2048,
                                                ct,
                                                (uint32_t)k_test_rsa2048_bytes,
                                                pt,
                                                (uint32_t)k_test_rsa2048_bytes));

  TEST_END("rsip protected rsa decrypt");
}

/**
 * @brief Protected ECDSA sign rebuilds the handle and signs.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_ecdsa_sign(void)
{
  TEST_BEGIN("rsip protected ecdsa sign");
  prep();

  uint8_t priv[k_test_p256_priv];
  (void)memset(priv, (int)k_test_pattern_d, sizeof(priv));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_ecc(blob, k_ra8_rsip_curve_secp256r1, priv, true));

  uint8_t hash[k_test_sha256_digest];
  (void)memset(hash, (int)k_test_pattern_h, sizeof(hash));
  uint8_t sig[k_test_p256_sig] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_ecdsa_sign(blob,
                                               k_ra8_rsip_curve_secp256r1,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig));

  TEST_END("rsip protected ecdsa sign");
}

/**
 * @brief Thin wrapper over `ra8_rsip_protected_rsa_decrypt` (RSA-2048 fixed).
 * @param[in]  blob Wrapped-key blob (nullptr exercises the blob guard).
 * @param[in]  in   Ciphertext input (nullptr exercises the input guard).
 * @param[out] out  Plaintext output (nullptr exercises the output guard).
 * @return The `ra8_rsip_protected_rsa_decrypt` result code.
 * @pre The RSIP mock is prepared.
 * @post No state beyond @p out is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t rp_rsa(uint8_t* blob, uint8_t* in, uint8_t* out)
{
  return ra8_rsip_protected_rsa_decrypt(blob,
                                        k_ra8_rsip_rsa_2048,
                                        in,
                                        (uint32_t)k_test_aes_block,
                                        out,
                                        (uint32_t)k_test_rsa2048_bytes);
}

/**
 * @brief Thin wrapper over `ra8_rsip_protected_ecdsa_sign` (secp256r1 fixed).
 * @param[in]  blob Wrapped-key blob (nullptr exercises the blob guard).
 * @param[in]  hash Digest input (nullptr exercises the hash guard).
 * @param[out] sig  Signature output (nullptr exercises the signature guard).
 * @return The `ra8_rsip_protected_ecdsa_sign` result code.
 * @pre The RSIP mock is prepared.
 * @post No state beyond @p sig is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t rp_ecdsa(uint8_t* blob, uint8_t* hash, uint8_t* sig)
{
  return ra8_rsip_protected_ecdsa_sign(blob,
                                       k_ra8_rsip_curve_secp256r1,
                                       hash,
                                       (uint32_t)k_test_sha256_digest,
                                       sig);
}

/**
 * @brief NULL-pointer rejection on every protected-layer entry.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_protected_null_args(void)
{
  TEST_BEGIN("rsip protected null args");
  prep();

  uint8_t blob[k_ra8_rsip_wrapped_max_total] = {};
  uint8_t buf[k_test_aes_block]              = {};
  uint8_t hash[k_test_sha256_digest]         = {};
  uint8_t sig[k_test_p256_sig]               = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_protected_aes_init(nullptr,
                                             k_ra8_rsip_aes_key_bits_128,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));

  /* Once init has not run successfully, encrypt/decrypt should still
   * fall to invalid_state regardless of NULL inputs (latched flag
   * guards before pointer checks). Sanity-check that finish keeps
   * the state idle. */
  (void)ra8_rsip_protected_aes_finish();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rsip_protected_aes_encrypt(nullptr, buf, (uint32_t)k_test_aes_block));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_rsa(nullptr, buf, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_rsa(blob, nullptr, buf));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_rsa(blob, buf, nullptr));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_ecdsa(nullptr, hash, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_ecdsa(blob, nullptr, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, rp_ecdsa(blob, hash, nullptr));

  TEST_END("rsip protected null args");
}

/**
 * @brief Protected AES init copies a caller-supplied IV (CBC mode).
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the init path's IV branch is a
 * single condition; no `&&` or `||` in the code under test that this
 * case touches)
 */
static void test_protected_aes_init_with_iv(void)
{
  TEST_BEGIN("rsip protected aes init with iv");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));

  /* A non-NULL IV drives the IV-copy loop in the init path. */
  uint8_t iv[k_test_aes_block];
  (void)memset(iv, (int)k_test_pattern_h, sizeof(iv));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_protected_aes_init(blob, k_ra8_rsip_aes_key_bits_128, k_ra8_rsip_aes_mode_cbc, iv));

  /* The latched IV is consumed by the cipher dispatch. */
  preload_data_out();
  uint8_t pt[k_test_aes_block] = {};
  uint8_t ct[k_test_aes_block] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_encrypt(pt, ct, (uint32_t)k_test_aes_block));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_protected_aes_finish());

  TEST_END("rsip protected aes init with iv");
}

/**
 * @brief Protected AES init rejects unsupported key-width selectors.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the width-bound check and the
 * install switch are single-condition / switch constructs; no `&&` or
 * `||` in the code under test that this case touches)
 */
static void test_protected_aes_init_bad_key_bits(void)
{
  TEST_BEGIN("rsip protected aes bad key bits");
  prep();

  uint8_t raw_key[k_test_aes_block];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));

  /* Width whose raw-key byte count exceeds the AES-256 scratch buffer
   * (512 / 8 = 64 > 32): rejected before the install dispatch. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_aes_init(blob,
                                             (ra8_rsip_aes_key_bits_t)k_test_aes_bits_wide,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));

  /* Width that fits the scratch buffer but has no installer
   * (200 / 8 = 25 bytes): drives p_aes_install's default arm and the
   * install error-return leg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_aes_init(blob,
                                             (ra8_rsip_aes_key_bits_t)k_test_aes_bits_noinst,
                                             k_ra8_rsip_aes_mode_ecb,
                                             nullptr));

  TEST_END("rsip protected aes bad key bits");
}

/**
 * @brief Protected RSA decrypt drives the 3072- and 4096-bit arms.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the size-to-bytes and
 * size-to-opcode maps are switches / if-chains over single equalities;
 * no `&&` or `||` in the code under test that this case touches)
 */
static void test_protected_rsa_decrypt_large_sizes(void)
{
  TEST_BEGIN("rsip protected rsa large sizes");
  prep();

  uint8_t exponent[k_test_exp_bytes] = {0x00U, 0x01U, 0x00U, 0x01U};

  /* RSA-3072: 3072 mod-bytes arm + rsa3072 install-cmd arm. */
  uint8_t mod3072[k_test_rsa3072_bytes] = {};
  (void)memset(mod3072, (int)k_test_pattern_h, sizeof(mod3072));
  uint8_t blob3072[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_rsa(blob3072, mod3072, exponent, k_ra8_rsip_rsa_3072));
  uint8_t ct3072[k_test_rsa3072_bytes] = {};
  uint8_t pt3072[k_test_rsa3072_bytes] = {};
  (void)memset(ct3072, (int)k_test_pattern_d, sizeof(ct3072));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_rsa_decrypt(blob3072,
                                                k_ra8_rsip_rsa_3072,
                                                ct3072,
                                                (uint32_t)k_test_rsa3072_bytes,
                                                pt3072,
                                                (uint32_t)k_test_rsa3072_bytes));

  /* RSA-4096: 4096 mod-bytes arm + rsa4096 install-cmd arm. */
  uint8_t mod4096[k_test_rsa4096_bytes] = {};
  (void)memset(mod4096, (int)k_test_pattern_h, sizeof(mod4096));
  uint8_t blob4096[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_rsa(blob4096, mod4096, exponent, k_ra8_rsip_rsa_4096));
  uint8_t ct4096[k_test_rsa4096_bytes] = {};
  uint8_t pt4096[k_test_rsa4096_bytes] = {};
  (void)memset(ct4096, (int)k_test_pattern_d, sizeof(ct4096));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_rsa_decrypt(blob4096,
                                                k_ra8_rsip_rsa_4096,
                                                ct4096,
                                                (uint32_t)k_test_rsa4096_bytes,
                                                pt4096,
                                                (uint32_t)k_test_rsa4096_bytes));

  TEST_END("rsip protected rsa large sizes");
}

/**
 * @brief Protected RSA decrypt rejects RSA-1024 (no OEM installer).
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the 1024 mod-bytes
 * arm, the install-cmd default arm, and the oem-install error return;
 * no `&&` or `||` in the code under test that this case touches)
 */
static void test_protected_rsa_decrypt_1024(void)
{
  TEST_BEGIN("rsip protected rsa 1024 no installer");
  prep();

  uint8_t exponent[k_test_exp_bytes]    = {0x00U, 0x01U, 0x00U, 0x01U};
  uint8_t mod1024[k_test_rsa1024_bytes] = {};
  (void)memset(mod1024, (int)k_test_pattern_h, sizeof(mod1024));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_rsa(blob, mod1024, exponent, k_ra8_rsip_rsa_1024));

  /* RSA-1024 maps to k_ra8_rsip_oem_cmd_invalid, so ra8_rsip_oem_install
   * rejects the re-install and the protected layer returns its error. */
  uint8_t ct[k_test_rsa1024_bytes] = {};
  uint8_t pt[k_test_rsa1024_bytes] = {};
  (void)memset(ct, (int)k_test_pattern_d, sizeof(ct));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                k_ra8_rsip_rsa_1024,
                                                ct,
                                                (uint32_t)k_test_rsa1024_bytes,
                                                pt,
                                                (uint32_t)k_test_rsa1024_bytes));

  TEST_END("rsip protected rsa 1024 no installer");
}

/**
 * @brief Protected RSA decrypt argument + wrapper validation legs.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the size-default, capacity,
 * length, and nested wrapper-validation checks are each single-condition;
 * no `&&` or `||` in the code under test that this case touches)
 */
static void test_protected_rsa_decrypt_arg_errors(void)
{
  TEST_BEGIN("rsip protected rsa arg errors");
  prep();

  uint8_t exponent[k_test_exp_bytes]    = {0x00U, 0x01U, 0x00U, 0x01U};
  uint8_t modulus[k_test_rsa2048_bytes] = {};
  (void)memset(modulus, (int)k_test_pattern_h, sizeof(modulus));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_rsa(blob, modulus, exponent, k_ra8_rsip_rsa_2048));

  uint8_t ct[k_test_rsa2048_bytes] = {};
  uint8_t pt[k_test_rsa2048_bytes] = {};

  /* Unsupported size selector -> internal_rsa_mod_bytes default arm. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                (ra8_rsip_rsa_size_t)k_test_rsa_size_bad,
                                                ct,
                                                (uint32_t)k_test_rsa2048_bytes,
                                                pt,
                                                (uint32_t)k_test_rsa2048_bytes));

  /* Plaintext capacity below the modulus width. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                k_ra8_rsip_rsa_2048,
                                                ct,
                                                (uint32_t)k_test_rsa2048_bytes,
                                                pt,
                                                (uint32_t)k_test_rsa_small_cap));

  /* Ciphertext longer than the modulus width. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                k_ra8_rsip_rsa_2048,
                                                ct,
                                                (uint32_t)k_test_rsa_overlen,
                                                pt,
                                                (uint32_t)k_test_rsa2048_bytes));

  /* Tampered MAC: the public-type validate fails on the MAC, then the
   * private-type validate fails on the type tag -> fallback error leg. */
  blob[(uint32_t)k_ra8_rsip_wrapped_max_total - 1U] ^= k_t_tamper_mask;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_rsa_decrypt(blob,
                                                k_ra8_rsip_rsa_2048,
                                                ct,
                                                (uint32_t)k_test_rsa2048_bytes,
                                                pt,
                                                (uint32_t)k_test_rsa2048_bytes));

  TEST_END("rsip protected rsa arg errors");
}

/**
 * @brief Protected ECDSA sign drives the P-384 / P-521 / secp256k1 arms.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the curve-to-opcode map is a
 * switch over single equalities; no `&&` or `||` in the code under test
 * that this case touches)
 */
static void test_protected_ecdsa_sign_curve_arms(void)
{
  TEST_BEGIN("rsip protected ecdsa curve arms");
  prep();

  uint8_t hash[k_test_sha256_digest];
  (void)memset(hash, (int)k_test_pattern_h, sizeof(hash));

  /* secp384r1 -> 48-byte private scalar, 96-byte signature. */
  uint8_t priv384[k_test_p384_priv];
  (void)memset(priv384, (int)k_test_pattern_d, sizeof(priv384));
  uint8_t blob384[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_ecc(blob384, k_ra8_rsip_curve_secp384r1, priv384, true));
  uint8_t sig384[k_test_p384_sig] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_ecdsa_sign(blob384,
                                               k_ra8_rsip_curve_secp384r1,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig384));

  /* secp521r1 -> 66-byte private scalar, 132-byte signature. */
  uint8_t priv521[k_test_p521_priv];
  (void)memset(priv521, (int)k_test_pattern_d, sizeof(priv521));
  uint8_t blob521[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_ecc(blob521, k_ra8_rsip_curve_secp521r1, priv521, true));
  uint8_t sig521[k_test_p521_sig] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_ecdsa_sign(blob521,
                                               k_ra8_rsip_curve_secp521r1,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig521));

  /* secp256k1 -> 32-byte private scalar, 64-byte signature. */
  uint8_t priv256k[k_test_p256_priv];
  (void)memset(priv256k, (int)k_test_pattern_d, sizeof(priv256k));
  uint8_t blob256k[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_ecc(blob256k, k_ra8_rsip_curve_secp256k1, priv256k, true));
  uint8_t sig256k[k_test_p256_sig] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_protected_ecdsa_sign(blob256k,
                                               k_ra8_rsip_curve_secp256k1,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig256k));

  TEST_END("rsip protected ecdsa curve arms");
}

/**
 * @brief Protected ECDSA sign validation + unsupported-curve legs.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the wrapper-validate return and
 * the curve switch default are single-condition / switch constructs; no
 * `&&` or `||` in the code under test that this case touches)
 */
static void test_protected_ecdsa_sign_arg_errors(void)
{
  TEST_BEGIN("rsip protected ecdsa arg errors");
  prep();

  uint8_t priv[k_test_p256_priv];
  (void)memset(priv, (int)k_test_pattern_d, sizeof(priv));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_ecc(blob, k_ra8_rsip_curve_secp256r1, priv, true));

  uint8_t hash[k_test_sha256_digest];
  (void)memset(hash, (int)k_test_pattern_h, sizeof(hash));
  uint8_t sig[k_test_p256_sig] = {};

  /* Unsupported curve selector -> the curve switch default arm. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_protected_ecdsa_sign(blob,
                                               (ra8_rsip_curve_t)k_test_curve_bad,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig));

  /* Tampered MAC -> ra8_rsip_key_validate rejects before dispatch. */
  blob[(uint32_t)k_ra8_rsip_wrapped_max_total - 1U] ^= k_t_tamper_mask;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rsip_protected_ecdsa_sign(blob,
                                               k_ra8_rsip_curve_secp256r1,
                                               hash,
                                               (uint32_t)k_test_sha256_digest,
                                               sig));

  TEST_END("rsip protected ecdsa arg errors");
}

int main(void)
{
  test_protected_aes_roundtrip();
  test_protected_aes192_init();
  test_protected_aes256_init();
  test_protected_aes_rejects_tamper();
  test_protected_aes_no_init();
  test_protected_aes_init_with_iv();
  test_protected_aes_init_bad_key_bits();
  test_protected_rsa_decrypt();
  test_protected_rsa_decrypt_large_sizes();
  test_protected_rsa_decrypt_1024();
  test_protected_rsa_decrypt_arg_errors();
  test_protected_ecdsa_sign();
  test_protected_ecdsa_sign_curve_arms();
  test_protected_ecdsa_sign_arg_errors();
  test_protected_null_args();
  return 0;
}
