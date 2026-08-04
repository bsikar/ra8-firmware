/**
 * @file test_psa_real_kat.c
 * @brief External NIST/RFC known-answer tests against the REAL crypto backend.
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * The regular host test build compiles ``ra8_psa_crypto.c`` under
 * ``RA8_OFF_TARGET``, which substitutes a deterministic stand-in for AES-GCM
 * and an HMAC tautology for the ECDSA "verify" -- so the AEAD / signature paths
 * are never checked against a real cipher. This test instead links the vendored
 * TF-PSA-Crypto library (the exact primitives the production ``#ifndef
 * RA8_OFF_TARGET`` path calls through ``psa_*``) directly, with NO
 * RA8_OFF_TARGET, and pins it to published vectors:
 *
 *   - SHA-256("abc")                         FIPS 180-4
 *   - AES-128-GCM encrypt + decrypt          GCM spec (McGrew/Viega) Test Case 3
 *   - ECDSA-P256 / SHA-256 verify            RFC 6979 A.2.5 ("sample")
 *   - ECDSA verify of a tampered signature   must be rejected (not a stub)
 *
 * A stub backend cannot satisfy these; the fake ``ra8_psa_crypto_fake.c`` would
 * fail every one, which is precisely the placeholder-vs-production distinction
 * the audit asked the build to make. The test is self-contained (psa_* + libc
 * only) and does not link the fake'd ``ra8_core_hal``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

/**
 * @enum t_kat_len_t
 * @brief Buffer lengths of the published known-answer test vectors, in bytes.
 *
 * @details
 * Every length is fixed by the standard the vector comes from -- GCM spec Test
 * Case 3 for the AEAD arm, RFC 6979 A.2.5 for the ECDSA arm -- so these size
 * the fixtures rather than parameterise them. Changing one invalidates the
 * vector it belongs to.
 */
typedef enum : uint8_t {
  k_t_kat_nonce_len = 12U, /**< GCM nonce: the 96-bit form. */
  k_t_kat_pt_len    = 64U, /**< Test Case 3 plaintext; also the expected
                                decrypted length and the ECDSA r||s signature. */
  k_t_kat_ct_len    = 80U, /**< Its ciphertext: 64 payload bytes + a 16-byte tag. */
  k_t_kat_pub_len   = 65U, /**< Uncompressed P-256 public key: 0x04 || X || Y.    */
  k_t_kat_pub_off_y = 33U, /**< Offset of Y within it: past the 0x04 prefix and
                                the 32-byte X coordinate.                      */
} t_kat_len_t;

/** @brief Running failure count; a non-zero exit fails the ctest. */
static int s_failures = 0;

/** @brief Report a check result and accumulate failures. */
static void check(const char* what, int ok)
{
  if (ok) {
    (void)fprintf(stderr, "[PASS] %s\n", what);
  } else {
    (void)fprintf(stderr, "[FAIL] %s\n", what);
    s_failures += 1;
  }
}

/** @brief Decode ``2*n`` hex chars into ``out`` (n bytes). */
static void unhex(const char* hex, uint8_t* out, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    /* strtoul rather than sscanf("%2x"): sscanf cannot report a
     * conversion error, so a malformed vector would decode as 0x00. */
    const char          pair[3] = {hex[2U * i], hex[(2U * i) + 1U], '\0'};
    char*               end     = nullptr;
    const unsigned long byte    = strtoul(pair, &end, 16);
    check("hex vector decodes", end == &pair[2]);
    out[i] = (uint8_t)byte;
  }
}

/** @brief FIPS 180-4 SHA-256("abc") sanity against the real backend. */
static void kat_sha256(void)
{
  uint8_t want[32];
  unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
  uint8_t      got[32];
  size_t       got_len = 0U;
  psa_status_t st =
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)"abc", 3U, got, sizeof(got), &got_len);
  check("SHA-256(\"abc\") FIPS 180-4",
        st == PSA_SUCCESS && got_len == 32U && memcmp(got, want, 32) == 0);
}

/** @brief GCM spec Test Case 3: AES-128-GCM encrypt then decrypt round-trip. */
static void kat_aes_gcm(void)
{
  uint8_t key[16];
  unhex("feffe9928665731c6d6a8f9467308308", key, 16);
  uint8_t nonce[k_t_kat_nonce_len];
  unhex("cafebabefacedbaddecaf888", nonce, k_t_kat_nonce_len);
  uint8_t pt[k_t_kat_pt_len];
  unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e24"
        "49a6b525b16aedf5aa0de657ba637b391aafd255",
        pt,
        k_t_kat_pt_len);
  uint8_t want_ct[k_t_kat_ct_len]; /* 64 ciphertext + 16 tag */
  unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5a"
        "ac84aa051ba30b396a0aac973d58e091473f59854d5c2af327cd64a62cf35abd2ba6fab4",
        want_ct,
        k_t_kat_ct_len);

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_GCM);
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_key_id_t kid = 0;
  psa_status_t st  = psa_import_key(&attr, key, sizeof(key), &kid);
  check("AES-128-GCM import key", st == PSA_SUCCESS);

  uint8_t ct[k_t_kat_ct_len];
  size_t  ct_len = 0U;
  st             = psa_aead_encrypt(kid,
                                    PSA_ALG_GCM,
                                    nonce,
                                    sizeof(nonce),
                                    nullptr,
                                    0U,
                                    pt,
                                    sizeof(pt),
                                    ct,
                                    sizeof(ct),
                                    &ct_len);
  check("AES-128-GCM encrypt == GCM TC3 ciphertext+tag",
        st == PSA_SUCCESS && ct_len == k_t_kat_ct_len && memcmp(ct, want_ct, k_t_kat_ct_len) == 0);

  uint8_t dec[k_t_kat_pt_len];
  size_t  dec_len = 0U;
  st              = psa_aead_decrypt(kid,
                                     PSA_ALG_GCM,
                                     nonce,
                                     sizeof(nonce),
                                     nullptr,
                                     0U,
                                     want_ct,
                                     sizeof(want_ct),
                                     dec,
                                     sizeof(dec),
                                     &dec_len);
  check("AES-128-GCM decrypt round-trip == plaintext",
        st == PSA_SUCCESS && dec_len == k_t_kat_pt_len && memcmp(dec, pt, k_t_kat_pt_len) == 0);

  (void)psa_destroy_key(kid);
}

/** @brief RFC 6979 A.2.5 ECDSA-P256/SHA-256 verify of "sample" + a tamper check. */
static void kat_ecdsa_p256(void)
{
  /* Uncompressed public key 0x04 || Ux || Uy. */
  uint8_t pub[k_t_kat_pub_len];
  pub[0] = 0x04U;
  unhex("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6", &pub[1], 32);
  unhex("7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299",
        &pub[k_t_kat_pub_off_y],
        32);
  /* Signature r || s. */
  uint8_t sig[k_t_kat_pt_len];
  unhex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716", &sig[0], 32);
  unhex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8", &sig[32], 32);

  uint8_t hash[32];
  size_t  hl = 0U;
  (void)psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)"sample", 6U, hash, sizeof(hash), &hl);

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_key_id_t kid = 0;
  psa_status_t st  = psa_import_key(&attr, pub, sizeof(pub), &kid);
  check("ECDSA-P256 import public key", st == PSA_SUCCESS);

  st = psa_verify_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig));
  check("ECDSA-P256 verify RFC 6979 A.2.5 signature", st == PSA_SUCCESS);

  sig[0] ^= 0x01U; /* flip one bit -> must be rejected */
  st = psa_verify_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig));
  check("ECDSA-P256 rejects a tampered signature", st == PSA_ERROR_INVALID_SIGNATURE);

  (void)psa_destroy_key(kid);
}

int main(void)
{
  psa_status_t st = psa_crypto_init();
  check("psa_crypto_init", st == PSA_SUCCESS);
  if (st != PSA_SUCCESS) {
    return 1;
  }
  kat_sha256();
  kat_aes_gcm();
  kat_ecdsa_p256();
  if (s_failures == 0) {
    (void)fprintf(stderr, "[OK ] test_psa_real_kat.c -- all real-backend KATs passed\n");
    return 0;
  }
  (void)fprintf(stderr, "[ERR] test_psa_real_kat.c -- %d KAT(s) failed\n", s_failures);
  return 1;
}
