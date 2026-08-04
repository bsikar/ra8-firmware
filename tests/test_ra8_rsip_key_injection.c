/**
 * @file test_ra8_rsip_key_injection.c
 * @brief Unit tests for ra8_rsip_key_injection.c (RSIP wrapped-key wrap)
 *
 * @details
 * Mirrors the ``test_ra8_sce.c`` injection-side tests for the RSIP
 * stub layer:
 *
 * - AES key inject / validate round-trip;
 * - bad AES width is rejected;
 * - ECC private and public injection round-trip;
 * - validate detects a single-byte tamper;
 * - validate refuses the wrong type tag;
 * - NULL-argument rejection on every entry point.
 *
 * Each test resets ``ra8_fake_mmap`` first so cases stay independent.
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
#include "ra8_rsip_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_inject_t
 * @brief Tamper mask and the RSA modulus buffer capacity.
 */
typedef enum : uint16_t {
  k_t_tamper_mask = 0xFFU, /**< XOR that flips the wrapped blob's last byte so
                                its integrity check must fail.                   */
  k_t_modulus_cap = 256U,  /**< RSA-2048 modulus, bytes. */
} t_inject_t;

/**
 * @enum ra8_rsip_ki_test_const_t
 * @brief Magic numbers used by these tests, named to keep the
 *        no-magic-numbers rule satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_aes128_bytes = 16U,    /**< AES-128 raw-key length.     */
  k_test_p256_priv    = 32U,    /**< P-256 private scalar bytes. */
  k_test_p256_pub     = 64U,    /**< P-256 public point bytes.   */
  k_test_pattern_key  = 0x42U,  /**< Sentinel byte for raw keys. */
  k_test_pattern_d    = 0x77U,  /**< Sentinel byte for ECC priv. */
  k_test_pattern_h    = 0xA5U,  /**< Sentinel byte for ECC pub.  */
  k_test_bad_key_bits = 0xABCDU /**< Out-of-range AES width.     */
} ra8_rsip_ki_test_const_t;

/**
 * @brief Reset ra8_fake_mmap before every test so cases stay
 *        independent of each other.
 *
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief AES-128 inject populates a self-validating blob.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_inject_aes128_roundtrip(void)
{
  TEST_BEGIN("rsip ki aes128 round-trip");
  prep();

  uint8_t raw_key[k_test_aes128_bytes];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_validate(blob, k_ra8_rsip_wrapped_type_aes));

  TEST_END("rsip ki aes128 round-trip");
}

/**
 * @brief Inject rejects an unsupported AES key width.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_inject_aes_bad_width(void)
{
  TEST_BEGIN("rsip ki aes bad width");
  prep();

  uint8_t raw_key[k_test_aes128_bytes];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rsip_key_inject_aes(blob, raw_key, (ra8_rsip_aes_key_bits_t)k_test_bad_key_bits));

  TEST_END("rsip ki aes bad width");
}

/**
 * @brief RSA-2048 inject populates a public-tagged blob.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_inject_rsa2048(void)
{
  TEST_BEGIN("rsip ki rsa2048 round-trip");
  prep();

  uint8_t modulus[k_t_modulus_cap] = {};
  uint8_t exponent[4]              = {0x00U, 0x01U, 0x00U, 0x01U};
  (void)memset(modulus, (int)k_test_pattern_h, sizeof(modulus));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_rsa(blob, modulus, exponent, k_ra8_rsip_rsa_2048));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_validate(blob, k_ra8_rsip_wrapped_type_rsa_pub));

  TEST_END("rsip ki rsa2048 round-trip");
}

/**
 * @brief ECC P-256 private + public injection both validate.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_inject_ecc_priv_pub(void)
{
  TEST_BEGIN("rsip ki ecc priv+pub round-trip");
  prep();

  uint8_t priv[k_test_p256_priv];
  uint8_t pub[k_test_p256_pub];
  (void)memset(priv, (int)k_test_pattern_d, sizeof(priv));
  (void)memset(pub, (int)k_test_pattern_h, sizeof(pub));

  uint8_t blob_priv[k_ra8_rsip_wrapped_max_total];
  uint8_t blob_pub[k_ra8_rsip_wrapped_max_total];

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_ecc(blob_priv, k_ra8_rsip_curve_secp256r1, priv, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_validate(blob_priv, k_ra8_rsip_wrapped_type_ecc_priv));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_key_inject_ecc(blob_pub, k_ra8_rsip_curve_secp256r1, pub, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_validate(blob_pub, k_ra8_rsip_wrapped_type_ecc_pub));

  TEST_END("rsip ki ecc priv+pub round-trip");
}

/**
 * @brief A single-byte tamper trips the validate MAC check.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_validate_detects_tamper(void)
{
  TEST_BEGIN("rsip ki validate detects tamper");
  prep();

  uint8_t raw_key[k_test_aes128_bytes];
  (void)memset(raw_key, (int)k_test_pattern_key, sizeof(raw_key));
  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(blob, raw_key, k_ra8_rsip_aes_key_bits_128));
  /* Flip a byte inside the trailing MAC region. */
  blob[(uint32_t)k_ra8_rsip_wrapped_max_total - 1U] ^= k_t_tamper_mask;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rsip_key_validate(blob, k_ra8_rsip_wrapped_type_aes));

  /* And the type-tag mismatch path. */
  uint8_t fresh[k_ra8_rsip_wrapped_max_total];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_inject_aes(fresh, raw_key, k_ra8_rsip_aes_key_bits_128));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_key_validate(fresh, k_ra8_rsip_wrapped_type_ecc_priv));

  TEST_END("rsip ki validate detects tamper");
}

/**
 * @brief NULL pointers are rejected by every public entry.
 * @since 0.1.0
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_null_args(void)
{
  TEST_BEGIN("rsip ki null args");
  prep();

  uint8_t blob[k_ra8_rsip_wrapped_max_total];
  uint8_t key[k_test_aes128_bytes] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_aes(nullptr, key, k_ra8_rsip_aes_key_bits_128));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_aes(blob, nullptr, k_ra8_rsip_aes_key_bits_128));

  uint8_t mod[k_t_modulus_cap] = {};
  uint8_t exp[4]               = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_rsa(nullptr, mod, exp, k_ra8_rsip_rsa_2048));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_rsa(blob, nullptr, exp, k_ra8_rsip_rsa_2048));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_rsa(blob, mod, nullptr, k_ra8_rsip_rsa_2048));

  uint8_t scalar[k_test_p256_priv] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_ecc(nullptr, k_ra8_rsip_curve_secp256r1, scalar, true));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_key_inject_ecc(blob, k_ra8_rsip_curve_secp256r1, nullptr, true));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_validate(nullptr, k_ra8_rsip_wrapped_type_aes));

  TEST_END("rsip ki null args");
}

int32_t main(void)
{
  test_inject_aes128_roundtrip();
  test_inject_aes_bad_width();
  test_inject_rsa2048();
  test_inject_ecc_priv_pub();
  test_validate_detects_tamper();
  test_null_args();
  (void)fprintf(stderr, "[OK ] test_ra8_rsip_key_injection.c\n");
  return 0;
}
