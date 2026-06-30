/**
 * @file test_ra_root_of_trust.c
 * @brief Host unit tests for the root-of-trust signed-image verifier.
 *
 * @details
 * Drives ``ra_rot_verify_image`` -- the shared authenticity gate placed at the
 * copy-to-run (``ra_dfu_launch``) and BLXNS (``ra_tz_secure_boot_jump_ns``)
 * trust boundaries. The tests forge a correctly-signed trailer using the
 * verifier's own provisioned root key (exposed for tests via
 * ``ra_rot_root_public_key``) and then exercise the gate's default-deny
 * decision logic:
 *
 *  - a valid signature is ALLOWED (``k_ra_ok``);
 *  - an invalid signature is DENIED;
 *  - a tampered body (digest mismatch) is DENIED;
 *  - a missing trailer is DENIED.
 *
 * Plus MC/DC vectors for the gate's three compound decisions (trailer
 * magic/version, body-length sanity, signature-length sanity).
 *
 * @note The real ECDSA-P256 math is hardware / PSA-backed. Under
 *       ``RA_SIMULATOR_MODE`` the PSA facade substitutes a deterministic
 *       SHA-256-based signature stand-in, so these tests validate the gate's
 *       *decision logic around the verify result*, not the ECDSA arithmetic.
 *       On-silicon ECDSA known-answer-test validation and provisioning of the
 *       real root public key remain outstanding (hardware / key-ceremony
 *       gated).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/*
 * The root of trust is opt-in behind RA_ENABLE_ROOT_OF_TRUST (default OFF, see
 * ra_rot.h). The globbed ``ra_rot.c`` therefore compiles to nothing in the
 * default host build, so its symbols are not otherwise linked. Enable the flag
 * for this test and pull the flag-gated implementation in directly (the test
 * include path covers libs/ra_dfu/src), keeping the gate exercisable.
 */
#define RA_ENABLE_ROOT_OF_TRUST

#include <stdint.h>

#include "ra_err.h"
#include "ra_psa_crypto.h"
#include "ra_rot.c" // NOLINT(bugprone-suspicious-include) -- pull in the flag-gated impl
#include "unity_minimal.h"

/** @brief Fixed test-body sizing + corrupted-field sentinels. */
typedef enum : uint32_t {
  k_test_rot_body_len  = 64U,         /**< Representative signed-body length.    */
  k_test_rot_bad_magic = 0xDEADBEEFU, /**< Wrong trailer magic.                  */
  k_test_rot_bad_ver   = 0x000000FFU, /**< Wrong trailer version.                */
  k_test_rot_siglen_hi = 65U,         /**< sig_len just past k_ra_rot_sig_bytes. */
} test_rot_const_t;

/**
 * @brief Fill ``out`` with a fully-valid, correctly-signed trailer for ``body``.
 *
 * @details
 * Computes SHA-256 over ``body`` and signs that digest with the *same* key
 * bytes the verifier trusts (imported with sign usage), so the resulting
 * trailer authenticates under ``ra_rot_verify_image``. Tests then copy and
 * mutate the trailer to drive each deny path.
 *
 * @param[in]  body     Image body bytes; non-NULL.
 * @param[in]  body_len Body length in bytes.
 * @param[out] out      Receives the signed trailer; non-NULL.
 *
 * @pre The PSA facade has been initialized.
 * @pre ``body`` and ``out`` are non-NULL.
 * @post ``*out`` is a trailer that ``ra_rot_verify_image`` accepts for ``body``.
 * @post No global state leaks (the transient signer key is destroyed).
 * @note Test-only helper.
 * @since 0.1.0
 */
static void build_signed_trailer(const uint8_t* body, uint32_t body_len, ra_rot_trailer_t* out)
{
  uint8_t digest[k_ra_rot_digest_bytes] = {};
  size_t  dlen                          = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                                     body,
                                     (size_t)body_len,
                                     digest,
                                     sizeof(digest),
                                     &dlen));

  const uint8_t* rootkey = nullptr;
  uint32_t       rootlen = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_rot_root_public_key(&rootkey, &rootlen));

  /* In RA_SIMULATOR_MODE the PSA sign primitive is SHA-256(key || hash), so
   * signing the digest with the root key bytes yields exactly what the
   * verifier recomputes for that same key. */
  const ra_psa_key_attr_t sattr = {
    .type  = k_ra_psa_key_type_ecc_p256_priv,
    .alg   = k_ra_psa_alg_ecdsa_sha_256,
    .usage = k_ra_psa_usage_sign,
  };
  ra_psa_key_t signer = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_psa_key_import(&signer, &sattr, rootkey, (size_t)rootlen));

  uint8_t sig[k_ra_rot_sig_bytes] = {};
  size_t  siglen                  = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_psa_sign_hash(signer,
                                  k_ra_psa_alg_ecdsa_sha_256,
                                  digest,
                                  sizeof(digest),
                                  sig,
                                  sizeof(sig),
                                  &siglen));
  (void)ra_psa_key_destroy(signer);

  *out          = (ra_rot_trailer_t){};
  out->magic    = (uint32_t)k_ra_rot_trailer_magic;
  out->version  = (uint32_t)k_ra_rot_version;
  out->body_len = body_len;
  out->sig_len  = (uint32_t)siglen;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_rot_digest_bytes; ++i) {
    out->digest[i] = digest[i];
  }
  for (uint32_t i = 0U; i < (uint32_t)siglen; ++i) {
    out->sig[i] = sig[i];
  }
}

/** @brief Fill ``body`` with a deterministic, non-trivial test pattern. */
static void fill_body(uint8_t* body, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    body[i] = (uint8_t)((i * 7U) + 3U);
  }
}

/**
 * @brief A correctly-signed image is ALLOWED (the gate returns ``k_ra_ok``).
 *
 * @par MC/DC: not applicable -- happy-path control vector. The compound
 *      decisions are isolated in the dedicated MC/DC tests below; this
 *      establishes the all-conditions-false control they reference.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_valid_image_allowed(void)
{
  TEST_BEGIN("ra_rot: valid signature -> launch allowed");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra_rot: valid signature -> launch allowed");
}

/**
 * @brief An invalid signature is DENIED (``k_ra_err_crc_mismatch``).
 *
 * @par MC/DC:
 * Decision: signature verify result ``sig_err != k_ra_ok`` (1 condition).
 * - Vector 1: untouched signature -> false -> ALLOW (the valid-image test).
 * - Vector 2: one signature byte flipped -> true -> DENY (this test).
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_invalid_signature_denied(void)
{
  TEST_BEGIN("ra_rot: invalid signature -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Corrupt the signature -- digest still matches, so the gate reaches the
   * ECDSA verify and rejects there. */
  trailer.sig[0] = (uint8_t)(trailer.sig[0] ^ 0xFFU);

  TEST_ASSERT_EQ(k_ra_err_crc_mismatch,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra_rot: invalid signature -> denied");
}

/**
 * @brief A tampered body (digest mismatch) is DENIED.
 *
 * @par MC/DC:
 * Decision: digest pre-check ``!internal_ct_equal(computed, trailer.digest)``
 * (1 condition).
 * - Vector 1: body unchanged -> equal -> false -> proceed (valid-image test).
 * - Vector 2: one body byte flipped after signing -> not equal -> true ->
 *   DENY (this test).
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC. The signature is left
 * intact to prove the *hash* gate (not the signature gate) catches the
 * tamper.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_tampered_body_denied(void)
{
  TEST_BEGIN("ra_rot: tampered body (hash mismatch) -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Tamper with the body AFTER signing: trailer.digest is now stale. */
  body[0] = (uint8_t)(body[0] ^ 0xA5U);

  TEST_ASSERT_EQ(k_ra_err_checksum_mismatch,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra_rot: tampered body (hash mismatch) -> denied");
}

/**
 * @brief A missing or NULL trailer/body is DENIED (``k_ra_err_null_ptr``).
 *
 * @par MC/DC:
 * Two independent NULL guards (``RA_CHECK_NULL_PTR``), each a single
 * condition:
 * - body=valid, trailer=NULL -> deny (varies the trailer guard).
 * - body=NULL,  trailer=valid -> deny (varies the body guard).
 * Combined with the valid-image control these prove each guard independently
 * forces the deny outcome.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_missing_trailer_denied(void)
{
  TEST_BEGIN("ra_rot: missing trailer / body -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Missing trailer. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, nullptr));

  /* Missing body. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_rot_verify_image(nullptr, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra_rot: missing trailer / body -> denied");
}

/**
 * @brief Trailer magic/version gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: ``magic != MAGIC || version != VERSION`` (2 conditions, ``||``).
 * - V1: magic ok, version ok   -> false -> ALLOW   (control: both false)
 * - V2: magic bad, version ok  -> true  -> DENY    (varies magic)
 * - V3: magic ok, version bad  -> true  -> DENY    (varies version)
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC. Deny returns
 * ``k_ra_err_validation_failed``.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_trailer_header_mcdc(void)
{
  TEST_BEGIN("ra_rot: trailer magic/version (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: both conditions false -> allow. */
  TEST_ASSERT_EQ(k_ra_ok, ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: bad magic. */
  ra_rot_trailer_t bad_magic = good;
  bad_magic.magic            = (uint32_t)k_test_rot_bad_magic;
  TEST_ASSERT_EQ(k_ra_err_validation_failed,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &bad_magic));

  /* V3: bad version. */
  ra_rot_trailer_t bad_ver = good;
  bad_ver.version          = (uint32_t)k_test_rot_bad_ver;
  TEST_ASSERT_EQ(k_ra_err_validation_failed,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &bad_ver));

  TEST_END("ra_rot: trailer magic/version (MC/DC)");
}

/**
 * @brief Body-length sanity + cross-check gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: ``body_len == 0 || body_len > MAX || trailer.body_len != body_len``
 * (3 conditions, all ``||``).
 * - V1: F,F,F (len=64, trailer.body_len=64)         -> false -> ALLOW (control)
 * - V2: T,F,F (len=0, trailer.body_len=0)           -> true  -> DENY (varies c1)
 * - V3: F,T,F (len=MAX+1, trailer.body_len=MAX+1)   -> true  -> DENY (varies c2)
 * - V4: F,F,T (len=64, trailer.body_len=32)         -> true  -> DENY (varies c3)
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC. Deny returns
 * ``k_ra_err_invalid_size``. Each non-control vector keeps the other two
 * conditions false so the varied condition independently drives the outcome.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_length_mcdc(void)
{
  TEST_BEGIN("ra_rot: body_len sanity/cross-check (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: all three conditions false -> allow. */
  TEST_ASSERT_EQ(k_ra_ok, ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: body_len == 0 (c1 true). trailer.body_len also 0 so c3 stays false. */
  ra_rot_trailer_t zero = good;
  zero.body_len         = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_rot_verify_image(body, 0U, &zero));

  /* V3: body_len > MAX (c2 true). trailer.body_len matches so c3 stays false. */
  ra_rot_trailer_t toobig = good;
  toobig.body_len         = (uint32_t)k_ra_rot_body_max + 1U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_rot_verify_image(body, (uint32_t)k_ra_rot_body_max + 1U, &toobig));

  /* V4: trailer.body_len != body_len (c3 true). len in range so c1/c2 false. */
  ra_rot_trailer_t mismatch = good;
  mismatch.body_len         = (uint32_t)k_test_rot_body_len / 2U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &mismatch));

  TEST_END("ra_rot: body_len sanity/cross-check (MC/DC)");
}

/**
 * @brief Signature-length sanity gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: ``sig_len == 0 || sig_len > k_ra_rot_sig_bytes`` (2 conditions,
 * ``||``).
 * - V1: F,F (sig_len=valid)        -> false -> ALLOW (control)
 * - V2: T,F (sig_len=0)            -> true  -> DENY (varies c1)
 * - V3: F,T (sig_len=65 > 64)      -> true  -> DENY (varies c2)
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC. Deny returns
 * ``k_ra_err_invalid_size``.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_siglen_mcdc(void)
{
  TEST_BEGIN("ra_rot: sig_len sanity (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: valid sig_len -> allow. */
  TEST_ASSERT_EQ(k_ra_ok, ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: sig_len == 0. */
  ra_rot_trailer_t zero = good;
  zero.sig_len          = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &zero));

  /* V3: sig_len > k_ra_rot_sig_bytes. */
  ra_rot_trailer_t toobig = good;
  toobig.sig_len          = (uint32_t)k_test_rot_siglen_hi;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &toobig));

  TEST_END("ra_rot: sig_len sanity (MC/DC)");
}

/**
 * @brief ``ra_rot_trailer_after`` + ``ra_rot_root_public_key`` accessors.
 *
 * @par MC/DC:
 * ``ra_rot_trailer_after`` guard ``image_base == NULL`` and
 * ``body_len == 0 || body_len > MAX`` -- each driven to its deny (NULL)
 * return and to its pass return. ``ra_rot_root_public_key`` NULL guards on
 * both out-parameters.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_helpers(void)
{
  TEST_BEGIN("ra_rot: trailer_after + root_public_key accessors");

  uint8_t image[k_test_rot_body_len + sizeof(ra_rot_trailer_t)] = {};

  /* trailer_after: valid -> points at body end. */
  const ra_rot_trailer_t* at =
    ra_rot_trailer_after(image, (uint32_t)k_test_rot_body_len);
  TEST_ASSERT(at == (const ra_rot_trailer_t*)(const void*)(image + k_test_rot_body_len));

  /* trailer_after: NULL base / out-of-range length -> nullptr. */
  TEST_ASSERT(ra_rot_trailer_after(nullptr, (uint32_t)k_test_rot_body_len) == nullptr);
  TEST_ASSERT(ra_rot_trailer_after(image, 0U) == nullptr);
  TEST_ASSERT(ra_rot_trailer_after(image, (uint32_t)k_ra_rot_body_max + 1U) == nullptr);

  /* root_public_key: success exposes a 65-byte uncompressed P-256 key. */
  const uint8_t* key = nullptr;
  uint32_t       len = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_rot_root_public_key(&key, &len));
  TEST_ASSERT(key != nullptr);
  TEST_ASSERT_EQ((uint32_t)k_ra_rot_pubkey_bytes, len);

  /* root_public_key: NULL out-parameters -> null_ptr. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rot_root_public_key(nullptr, &len));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rot_root_public_key(&key, nullptr));

  TEST_END("ra_rot: trailer_after + root_public_key accessors");
}

int main(void)
{
  /* The verifier ensures the PSA facade is ready, but the test's signing
   * helpers need it too; initialize once up front. */
  (void)ra_psa_crypto_init();

  test_rot_valid_image_allowed();
  test_rot_invalid_signature_denied();
  test_rot_tampered_body_denied();
  test_rot_missing_trailer_denied();
  test_rot_trailer_header_mcdc();
  test_rot_length_mcdc();
  test_rot_siglen_mcdc();
  test_rot_helpers();
  return 0;
}
