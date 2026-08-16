/**
 * @file test_ra8_psa_crypto_aead_mcdc.c
 * @brief AEAD MC/DC vectors for the ra8_psa_crypto facade
 *
 * @details
 * Split sibling of the original test_ra8_psa_crypto.c suite closing
 * the DO-178C Level B / IEC 61508 SIL 3 MC/DC obligations for the
 * AEAD compound decisions in ``ra8_psa_crypto.c``:
 *
 * - ``ra8_psa_aead_encrypt`` / ``ra8_psa_aead_decrypt`` argument,
 *   AAD, handle / algorithm / usage and size-guard decisions
 * - the fake keystream scratch loops incl. the overflow leg
 *
 * Each test follows the canonical N+1 vector pattern (see
 * tests/test_ra8_xspi.c::test_set_xip_mode_mcdc_addr_bytes for the
 * reference template).
 *
 * Sibling suites: test_ra8_psa_crypto_api.c (public-API contract)
 * and test_ra8_psa_crypto_guards.c (non-AEAD MC/DC + error legs).
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
 * @enum t_aead_len_t
 * @brief Plaintext length for the multi-block AEAD vectors, in bytes.
 */
typedef enum : uint16_t {
  k_t_plain_len = 240U, /**< 15 AES blocks: past one block and not a power of
                             two, so a block-count off-by-one is visible.       */
} t_aead_len_t;

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

/**
 * @enum ra8_psa_mcdc_const_t
 * @brief Magic-number-free vector inputs for the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_psa_mcdc_short_aad    = 4U, /**< PSA mcdc short aad.    */
  k_psa_mcdc_plain_len    = 8U, /**< PSA mcdc plain length. */
  k_psa_mcdc_tiny_len     = 8U, /**< PSA mcdc tiny length.  */
  k_psa_mcdc_bad_nonce    = 8U, /**< PSA mcdc bad nonce.    */
  k_psa_mcdc_small_outcap = 2U, /**< PSA mcdc small outcap. */
} ra8_psa_mcdc_const_t;

/** @brief Helper: import a valid AES key with the requested usage mask. */
static ra8_psa_key_t mcdc_import_aes_key(ra8_psa_key_usage_t usage)
{
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = usage,
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  return k;
}

/**
 * @brief Run one AES-GCM encrypt MC/DC vector with the fixture nonce.
 *
 * @details Fixes the 12-byte nonce and a full-capacity scratch output so
 * a vector reads as one source line under the NASA Rule 4 function-size
 * cap; only the arguments the vectors vary are parameters.
 */
static ra8_err_t enc_std(ra8_psa_key_t  key,
                         const uint8_t* aad,
                         size_t         aad_len,
                         const uint8_t* plain,
                         size_t         plain_len)
{
  uint8_t out[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ol = 0U;
  return ra8_psa_aead_encrypt(key,
                              k_ra8_psa_alg_aes_gcm,
                              k_test_nonce,
                              sizeof(k_test_nonce),
                              aad,
                              aad_len,
                              plain,
                              plain_len,
                              out,
                              sizeof(out),
                              &ol);
}

/**
 * @brief Produce a reference ciphertext of `plain_len` zero bytes.
 *
 * @details Encrypts with the fixture nonce into the caller's `ct`
 * buffer, whose capacity must be `plain_len + k_ra8_psa_gcm_tag_len`.
 */
static ra8_err_t enc_ct(ra8_psa_key_t  key,
                        const uint8_t* aad,
                        size_t         aad_len,
                        size_t         plain_len,
                        uint8_t*       ct,
                        size_t*        ct_len)
{
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  return ra8_psa_aead_encrypt(key,
                              k_ra8_psa_alg_aes_gcm,
                              k_test_nonce,
                              sizeof(k_test_nonce),
                              aad,
                              aad_len,
                              (plain_len == 0U) ? nullptr : plain,
                              plain_len,
                              ct,
                              plain_len + (size_t)k_ra8_psa_gcm_tag_len,
                              ct_len);
}

/**
 * @brief Run one AES-GCM decrypt MC/DC vector with the fixture nonce.
 */
static ra8_err_t dec_std(ra8_psa_key_t  key,
                         const uint8_t* aad,
                         size_t         aad_len,
                         const uint8_t* ct,
                         size_t         ct_len,
                         uint8_t*       out,
                         size_t         out_cap,
                         size_t*        out_len)
{
  return ra8_psa_aead_decrypt(key,
                              k_ra8_psa_alg_aes_gcm,
                              k_test_nonce,
                              sizeof(k_test_nonce),
                              aad,
                              aad_len,
                              ct,
                              ct_len,
                              out,
                              out_cap,
                              out_len);
}

/**
 * @test test_mcdc_aead_encrypt_check_triple
 * @par MC/DC:
 * Decision: `if ((nonce == NULL) || (out == NULL) || (out_len == NULL))`
 * (3 conditions, libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 894,
 * `internal_aead_encrypt_check`). N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_encrypt_check_triple(void)
{
  TEST_BEGIN("psa MC/DC: aead_encrypt_check (nonce||out||out_len) NULL triple");
  prep_init();
  ra8_psa_key_t k = mcdc_import_aes_key((ra8_psa_key_usage_t)k_ra8_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(k);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      nullptr,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      nullptr,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      nullptr));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: aead_encrypt_check (nonce||out||out_len) NULL triple");
}

/**
 * @test test_mcdc_aead_encrypt_check_plain_aad_quad
 * @par MC/DC:
 * Decision: `if (((plain == NULL) && (plain_len != 0U)) ||
 *               ((aad == NULL) && (aad_len != 0U)))` (4 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 897). Conditions
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
  ra8_psa_key_t k = mcdc_import_aes_key((ra8_psa_key_usage_t)k_ra8_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(k);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  const uint8_t aad[k_psa_mcdc_short_aad]   = {0U};
  /* V1: plain!=NULL, aad=NULL/0 -> both sub-decisions false. */
  TEST_ASSERT_EQ(k_ra8_ok, enc_std(k, nullptr, 0U, plain, sizeof(plain)));
  /* V2: plain=NULL with plain_len!=0 -> A=T, B=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, enc_std(k, nullptr, 0U, nullptr, sizeof(plain)));
  /* V3: plain=NULL with plain_len==0 -> A=T, B=F. */
  TEST_ASSERT_EQ(k_ra8_ok, enc_std(k, nullptr, 0U, nullptr, 0U));
  /* V4: aad=NULL with aad_len!=0 -> C=T, D=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, enc_std(k, nullptr, sizeof(aad), plain, sizeof(plain)));
  /* V5: aad!=NULL with aad_len==0 -> C=F. */
  TEST_ASSERT_EQ(k_ra8_ok, enc_std(k, aad, 0U, plain, sizeof(plain)));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: encrypt_check (plain==NULL&&len) || (aad==NULL&&len)");
}

/**
 * @test test_mcdc_aead_encrypt_handle_alg_usage
 * @par MC/DC:
 * Decision: `if (!internal_handle_valid(handle) ||
 *               (alg != k_ra8_psa_alg_aes_gcm) ||
 *               ((handle->attr.usage & k_ra8_psa_usage_encrypt) == 0U))`
 * (3 conditions, libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 903).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_encrypt_handle_alg_usage(void)
{
  TEST_BEGIN("psa MC/DC: encrypt (!valid || alg!=gcm || !usage_encrypt)");
  prep_init();
  ra8_psa_key_t k                           = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ol = 0U;
  /* V1: valid handle, GCM alg, encrypt usage -> all conditions false. */
  TEST_ASSERT_EQ(k_ra8_ok, enc_std(k, nullptr, 0U, plain, sizeof(plain)));
  /* V2: invalid handle short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, enc_std(nullptr, nullptr, 0U, plain, sizeof(plain)));
  /* V3: wrong algorithm tag. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_sha_256,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  /* V4: decrypt-only key lacks the encrypt usage bit. */
  ra8_psa_key_t k2 = mcdc_import_aes_key(k_ra8_psa_usage_decrypt);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, enc_std(k2, nullptr, 0U, plain, sizeof(plain)));
  (void)ra8_psa_key_destroy(k2);
  teardown();
  TEST_END("psa MC/DC: encrypt (!valid || alg!=gcm || !usage_encrypt)");
}

/**
 * @test test_mcdc_aead_encrypt_size_pair
 * @par MC/DC:
 * Decision: `if ((nonce_len != gcm_nonce_len) ||
 *               (out_cap < (plain_len + gcm_tag_len)))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 907). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_encrypt_size_pair(void)
{
  TEST_BEGIN("psa MC/DC: encrypt sizes (nonce_len||out_cap)");
  prep_init();
  ra8_psa_key_t k                           = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  uint8_t       out[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ol = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      (size_t)k_psa_mcdc_bad_nonce,
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      plain,
                                      sizeof(plain),
                                      out,
                                      (size_t)k_psa_mcdc_small_outcap,
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: encrypt sizes (nonce_len||out_cap)");
}

/**
 * @test test_mcdc_aead_decrypt_check_triple
 * @par MC/DC:
 * Decision: `if ((nonce == NULL) || (cipher == NULL) || (out_len == NULL))`
 * (3 conditions, libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 994,
 * `internal_aead_decrypt_check`). N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_decrypt_check_triple(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (nonce||cipher||out_len) NULL triple");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  TEST_ASSERT_NOT_NULL(k);
  uint8_t ct[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ctl = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, nullptr, 0U, (size_t)k_psa_mcdc_plain_len, ct, &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  /* V1: every pointer valid -> all conditions false. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, nullptr, 0U, ct, ctl, out, sizeof(out), &ol));
  /* V2: nonce=NULL short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      nullptr,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      ct,
                                      ctl,
                                      out,
                                      sizeof(out),
                                      &ol));
  /* V3: cipher=NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 dec_std(k, nullptr, 0U, nullptr, ctl, out, sizeof(out), &ol));
  /* V4: out_len=NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 dec_std(k, nullptr, 0U, ct, ctl, out, sizeof(out), nullptr));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (nonce||cipher||out_len) NULL triple");
}

/**
 * @test test_mcdc_aead_decrypt_aad_pair
 * @par MC/DC:
 * Decision: `if ((aad == NULL) && (aad_len != 0U))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 997). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_aad_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (aad==NULL && aad_len!=0)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  const uint8_t aad[k_psa_mcdc_short_aad] = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ctl = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, aad, sizeof(aad), (size_t)k_psa_mcdc_plain_len, ct, &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  /* V1: aad=NULL, aad_len=0 -> C1=T, C2=F -> F (tag mismatch is fine). */
  const ra8_err_t v1 = dec_std(k, nullptr, 0U, ct, ctl, out, sizeof(out), &ol);
  TEST_ASSERT(v1 == k_ra8_ok || v1 == k_ra8_err_crc_mismatch);
  /* V2: aad valid -> C1=F. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, aad, sizeof(aad), ct, ctl, out, sizeof(out), &ol));
  /* V3: aad=NULL with aad_len!=0 -> C1=T, C2=T -> T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 dec_std(k, nullptr, sizeof(aad), ct, ctl, out, sizeof(out), &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (aad==NULL && aad_len!=0)");
}

/**
 * @test test_mcdc_aead_decrypt_handle_alg_usage
 * @par MC/DC:
 * Decision: `if (!internal_handle_valid(handle) ||
 *               (alg != k_ra8_psa_alg_aes_gcm) ||
 *               ((handle->attr.usage & k_ra8_psa_usage_decrypt) == 0U))`
 * (3 conditions, libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 1003).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_aead_decrypt_handle_alg_usage(void)
{
  TEST_BEGIN("psa MC/DC: decrypt (!valid || alg!=gcm || !usage_decrypt)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  uint8_t ct[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ctl = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, nullptr, 0U, (size_t)k_psa_mcdc_plain_len, ct, &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  /* V1: valid handle, GCM alg, decrypt usage -> all conditions false. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, nullptr, 0U, ct, ctl, out, sizeof(out), &ol));
  /* V2: invalid handle short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 dec_std(nullptr, nullptr, 0U, ct, ctl, out, sizeof(out), &ol));
  /* V3: wrong algorithm tag. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_sha_256,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      ct,
                                      ctl,
                                      out,
                                      sizeof(out),
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  /* V4: encrypt-only key lacks the decrypt usage bit. */
  ra8_psa_key_t k2 = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, dec_std(k2, nullptr, 0U, ct, ctl, out, sizeof(out), &ol));
  (void)ra8_psa_key_destroy(k2);
  teardown();
  TEST_END("psa MC/DC: decrypt (!valid || alg!=gcm || !usage_decrypt)");
}

/**
 * @test test_mcdc_aead_decrypt_size_pair
 * @par MC/DC:
 * Decision: `if ((nonce_len != gcm_nonce_len) ||
 *               (cipher_len < gcm_tag_len))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 1007). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_size_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt sizes (nonce_len||cipher_len)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  uint8_t ct[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ctl = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, nullptr, 0U, (size_t)k_psa_mcdc_plain_len, ct, &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  /* V1: nonce_len ok, cipher_len ok -> both false. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, nullptr, 0U, ct, ctl, out, sizeof(out), &ol));
  /* V2: bad nonce length short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      (size_t)k_psa_mcdc_bad_nonce,
                                      nullptr,
                                      0U,
                                      ct,
                                      ctl,
                                      out,
                                      sizeof(out),
                                      &ol));
  /* V3: cipher shorter than the GCM tag. */
  uint8_t tiny[k_psa_mcdc_tiny_len] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 dec_std(k, nullptr, 0U, tiny, sizeof(tiny), out, sizeof(out), &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt sizes (nonce_len||cipher_len)");
}

/**
 * @test test_mcdc_aead_decrypt_out_pair
 * @par MC/DC:
 * Decision: `if ((out == NULL) && (plain_len != 0U))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 1012).
 * V1 out=NULL,plain_len=0 (cipher = exactly tag) -> C1=T,C2=F -> F.
 * V2 out!=NULL,plain_len=8 -> C1=F -> F. V3 out=NULL,plain_len=8 -> T.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_aead_decrypt_out_pair(void)
{
  TEST_BEGIN("psa MC/DC: decrypt_check (out==NULL && plain_len!=0)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  uint8_t ct8[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ctl8 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, nullptr, 0U, (size_t)k_psa_mcdc_plain_len, ct8, &ctl8));
  uint8_t ct0[k_ra8_psa_gcm_tag_len];
  size_t  ctl0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, enc_ct(k, nullptr, 0U, 0U, ct0, &ctl0));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  /* V1: out=NULL with plain_len==0 (cipher is exactly the tag) -> F. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, nullptr, 0U, ct0, ctl0, nullptr, 0U, &ol));
  /* V2: out!=NULL -> C1=F. */
  TEST_ASSERT_EQ(k_ra8_ok, dec_std(k, nullptr, 0U, ct8, ctl8, out, sizeof(out), &ol));
  /* V3: out=NULL with plain_len!=0 -> T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, dec_std(k, nullptr, 0U, ct8, ctl8, nullptr, 0U, &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: decrypt_check (out==NULL && plain_len!=0)");
}

/**
 * @test test_mcdc_fake_aead_buf_loops
 * @par MC/DC:
 * Loop guards in `internal_fake_aead_tag` (lines 381, 384, 387, 390) and
 * `internal_fake_keystream` (lines 416, 419) -- each is a 2-condition AND
 * `(i < N) && (off < sizeof(buf))`. The C2 flip (off >= 256) is structurally
 * unreachable through the public API: max(key + nonce + aad + cipher) is
 * bounded by the input-validation checks at lines 894-908 well below 256.
 * Documented as a deactivated condition under DO-178C 6.4.4.3. We exercise
 * C1's entered-body and exit transitions for all six loops via an encrypt +
 * decrypt round-trip with non-trivial inputs.
 */
static void test_mcdc_fake_aead_buf_loops(void)
{
  TEST_BEGIN("psa MC/DC: fake AEAD/keystream buffer loop guards (C1)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  const uint8_t plain[k_psa_mcdc_plain_len] = {0U};
  const uint8_t aad[k_psa_mcdc_short_aad]   = {0U};
  uint8_t       ct[k_psa_mcdc_plain_len + k_ra8_psa_gcm_tag_len];
  size_t        ctl = 0U;
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
                                      &ctl));
  uint8_t out[k_psa_mcdc_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      aad,
                                      sizeof(aad),
                                      ct,
                                      ctl,
                                      out,
                                      sizeof(out),
                                      &ol));
  uint8_t ct0[k_ra8_psa_gcm_tag_len];
  size_t  ctl0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      nullptr,
                                      0U,
                                      ct0,
                                      sizeof(ct0),
                                      &ctl0));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: fake AEAD/keystream buffer loop guards (C1)");
}

/**
 * @test test_mcdc_fake_aead_buf_loops_overflow
 *
 * @par MC/DC:
 * Decisions in libs/ra8_psa_crypto/src/ra8_psa_crypto.c (fake AEAD path):
 * lines 476/479/482/485 (encrypt scratch fill loops) and 524/527 (HKDF
 * keystream seed fill loops). Each is a 2-cond AND short-circuit:
 *   ``(i < <len>) && (off < sizeof(buf))``
 * with sizeof(buf) == k_ra8_psa_fake_scratch_bytes (256). The pre-existing
 * ``test_mcdc_fake_aead_buf_loops`` covers C1 by varying lengths but never
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
 *   exercised through ra8_psa_key_derive (HKDF) with similarly large
 *   key+nonce -- here we focus on the encrypt path because it's the
 *   only one with a public API that lets us drive the lengths.
 */
static void test_mcdc_fake_aead_buf_loops_overflow(void)
{
  TEST_BEGIN("psa MC/DC: fake AEAD scratch overflow (C2 for line 485)");
  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  /* 240-byte plaintext: 16(key) + 12(nonce) + 0(aad) + 240(cipher) = 268 > 256. */
  uint8_t plain[k_t_plain_len] = {};
  uint8_t ct[k_t_plain_len + k_ra8_psa_gcm_tag_len];
  size_t  ctl = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
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
  uint8_t out[k_t_plain_len];
  size_t  ol = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      k_test_nonce,
                                      sizeof(k_test_nonce),
                                      nullptr,
                                      0U,
                                      ct,
                                      ctl,
                                      out,
                                      sizeof(out),
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: fake AEAD scratch overflow (C2 for line 485)");
}

int main(void)
{
  test_mcdc_aead_encrypt_check_triple();
  test_mcdc_aead_encrypt_check_plain_aad_quad();
  test_mcdc_aead_encrypt_handle_alg_usage();
  test_mcdc_aead_encrypt_size_pair();
  test_mcdc_aead_decrypt_check_triple();
  test_mcdc_aead_decrypt_aad_pair();
  test_mcdc_aead_decrypt_handle_alg_usage();
  test_mcdc_aead_decrypt_size_pair();
  test_mcdc_aead_decrypt_out_pair();
  test_mcdc_fake_aead_buf_loops();
  test_mcdc_fake_aead_buf_loops_overflow();
  return 0;
}
