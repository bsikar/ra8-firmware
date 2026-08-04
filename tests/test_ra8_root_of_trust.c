/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_root_of_trust.c
 * @brief Host unit tests for the root-of-trust signed-image verifier.
 *
 * @details
 * Drives ``ra8_rot_verify_image`` -- the shared authenticity gate placed at the
 * copy-to-run (``ra8_dfu_launch``) and BLXNS (``ra8_tz_secure_boot_jump_ns``)
 * trust boundaries. The tests forge a correctly-signed trailer using the
 * verifier's own provisioned root key (exposed for tests via
 * ``ra8_rot_root_public_key``) and then exercise the gate's default-deny
 * decision logic:
 *
 *  - a valid signature is ALLOWED (``k_ra8_ok``);
 *  - an invalid signature is DENIED;
 *  - a tampered body (digest mismatch) is DENIED;
 *  - a missing trailer is DENIED.
 *
 * Plus MC/DC vectors for the gate's three compound decisions (trailer
 * magic/version, body-length sanity, signature-length sanity).
 *
 * @note The real ECDSA-P256 math is hardware / PSA-backed. Under
 *       ``RA8_OFF_TARGET`` the PSA facade substitutes a deterministic
 *       SHA-256-based signature stand-in, so these tests validate the gate's
 *       *decision logic around the verify result*, not the ECDSA arithmetic.
 *       On-silicon ECDSA known-answer-test validation and provisioning of the
 *       real root public key remain outstanding (hardware / key-ceremony
 *       gated).
 */

/*
 * The root of trust is opt-in behind RA8_ENABLE_ROOT_OF_TRUST (default OFF, see
 * ra8_rot.h). The globbed ``ra8_rot.c`` therefore compiles to nothing in the
 * default host build, so its symbols are not otherwise linked. Enable the flag
 * for this test and pull the flag-gated implementation in directly (the test
 * include path covers libs/ra8_dfu/src), keeping the gate exercisable.
 */
#define RA8_ENABLE_ROOT_OF_TRUST

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"
#include "ra8_rot.c" // NOLINT(bugprone-suspicious-include) -- pull in the flag-gated impl
#include "unity_minimal.h"

/**
 * @enum t_rot_t
 * @brief Image fill pattern, tamper mask and little-endian serialisation.
 */
typedef enum : uint8_t {
  k_t_body_stride   = 7U,    /**< Multiplier of the image byte pattern; co-prime
                                  with the word size so it does not repeat.     */
  k_t_tamper_mask   = 0xA5U, /**< XOR that flips a body byte to break the
                                  signature, then restores it.                  */
  k_t_byte_mask     = 0xFFU, /**< Low-byte mask while serialising a 32-bit
                                  field; also the XOR that corrupts a signature. */
  k_t_le32_hi_shift = 24U,   /**< Shift for the top byte of a 32-bit LE field. */
} t_rot_t;

/** @brief Fixed test-body sizing + corrupted-field sentinels. */
typedef enum : uint32_t {
  k_test_rot_body_len  = 64U,         /**< Representative signed-body length.     */
  k_test_rot_bad_magic = 0xDEADBEEFU, /**< Wrong trailer magic.                   */
  k_test_rot_bad_ver   = 0x000000FFU, /**< Wrong trailer version.                 */
  k_test_rot_siglen_hi = 65U,         /**< sig_len just past k_ra8_rot_sig_bytes. */
  k_test_rot_img_ver   = 5U,          /**< Anti-rollback version bound into sig.  */
} test_rot_const_t;

/** @brief NS-image-shaped fixture sizing (header at a fixed offset in the body). */
typedef enum : uint32_t {
  k_test_ns_hdr_off    = 0x40U,       /**< Header offset (k_ra8_tz_ns_rot_header_offset). */
  k_test_ns_magic      = 0x3152534EU, /**< "NSR1" (k_ra8_tz_ns_rot_header_magic).         */
  k_test_ns_body_len   = 128U,        /**< Signed body length (header lives inside it).   */
  k_test_ns_tamper_off = 0x50U,       /**< A body byte to flip (past the header).         */
  k_test_ns_lied_len   = 96U,         /**< A wrong header body_len (attacker lie).        */
} test_ns_const_t;

/** @brief Read a little-endian uint32 from ``buf`` at byte ``off`` (header word). */
static uint32_t read_u32_le(const uint8_t* buf, uint32_t off)
{
  return (uint32_t)buf[off] | ((uint32_t)buf[off + 1U] << 8U) | ((uint32_t)buf[off + 2U] << 16U) |
         ((uint32_t)buf[off + 3U] << k_t_le32_hi_shift);
}

/** @brief Write a little-endian uint32 ``val`` into ``buf`` at byte ``off``. */
static void write_u32_le(uint8_t* buf, uint32_t off, uint32_t val)
{
  buf[off]      = (uint8_t)(val & k_t_byte_mask);
  buf[off + 1U] = (uint8_t)((val >> 8U) & k_t_byte_mask);
  buf[off + 2U] = (uint8_t)((val >> 16U) & k_t_byte_mask);
  buf[off + 3U] = (uint8_t)((val >> k_t_le32_hi_shift) & k_t_byte_mask);
}

/**
 * @brief Fill ``out`` with a fully-valid, correctly-signed trailer for ``body``.
 *
 * @details
 * Computes SHA-256 over ``body`` and signs that digest with the *same* key
 * bytes the verifier trusts (imported with sign usage), so the resulting
 * trailer authenticates under ``ra8_rot_verify_image``. Tests then copy and
 * mutate the trailer to drive each deny path.
 *
 * @param[in]  body     Image body bytes; non-NULL.
 * @param[in]  body_len Body length in bytes.
 * @param[out] out      Receives the signed trailer; non-NULL.
 *
 * @pre The PSA facade has been initialized.
 * @pre ``body`` and ``out`` are non-NULL.
 * @post ``*out`` is a trailer that ``ra8_rot_verify_image`` accepts for ``body``.
 * @post No global state leaks (the transient signer key is destroyed).
 * @note Test-only helper.
 * @since 0.1.0
 */
static void build_signed_trailer(const uint8_t* body, uint32_t body_len, ra8_rot_trailer_t* out)
{
  uint8_t digest[k_ra8_rot_digest_bytes] = {};
  size_t  dlen                           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      body,
                                      (size_t)body_len,
                                      digest,
                                      sizeof(digest),
                                      &dlen));

  const uint8_t* rootkey = nullptr;
  uint32_t       rootlen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_root_public_key(&rootkey, &rootlen));

  /* In RA8_OFF_TARGET the PSA sign primitive is SHA-256(key || hash), so
   * signing the digest with the root key bytes yields exactly what the
   * verifier recomputes for that same key. */
  const ra8_psa_key_attr_t sattr = {
    .type  = k_ra8_psa_key_type_ecc_p256_priv,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = k_ra8_psa_usage_sign,
  };
  ra8_psa_key_t signer = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&signer, &sattr, rootkey, (size_t)rootlen));

  uint8_t sig[k_ra8_rot_sig_bytes] = {};
  size_t  siglen                   = 0U;
  /* Sign the version-bound material SHA-256(img_version_le || digest) -- the
   * exact bytes ra8_rot_verify_image reconstructs via internal_bind_version (the
   * white-box include gives us the same helper), so the anti-rollback version
   * is authenticated (T5-05). */
  uint8_t signed_material[k_ra8_rot_digest_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_bind_version((uint32_t)k_test_rot_img_ver, digest, signed_material));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_sign_hash(signer,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   signed_material,
                                   sizeof(signed_material),
                                   sig,
                                   sizeof(sig),
                                   &siglen));
  (void)ra8_psa_key_destroy(signer);

  *out             = (ra8_rot_trailer_t){};
  out->magic       = (uint32_t)k_ra8_rot_trailer_magic;
  out->version     = (uint32_t)k_ra8_rot_version;
  out->img_version = (uint32_t)k_test_rot_img_ver;
  out->body_len    = body_len;
  out->sig_len     = (uint32_t)siglen;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rot_digest_bytes; ++i) {
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
    body[i] = (uint8_t)((i * k_t_body_stride) + 3U);
  }
}

/**
 * @brief A correctly-signed image is ALLOWED (the gate returns ``k_ra8_ok``).
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
  TEST_BEGIN("ra8_rot: valid signature -> launch allowed");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra8_rot: valid signature -> launch allowed");
}

/**
 * @brief An invalid signature is DENIED (``k_ra8_err_crc_mismatch``).
 *
 * @par MC/DC:
 * Decision: signature verify result ``sig_err != k_ra8_ok`` (1 condition).
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
  TEST_BEGIN("ra8_rot: invalid signature -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Corrupt the signature -- digest still matches, so the gate reaches the
   * ECDSA verify and rejects there. */
  trailer.sig[0] = (uint8_t)(trailer.sig[0] ^ k_t_byte_mask);

  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra8_rot: invalid signature -> denied");
}

/**
 * @brief A forged ``img_version`` is DENIED even on a validly-signed body (T5-05).
 *
 * @details
 * The signature authenticates ``SHA-256(img_version_le || body_digest)``. Raising
 * ``img_version`` after signing changes the material ``ra8_rot_verify_image``
 * reconstructs, so the original signature no longer verifies -- an attacker
 * cannot relabel an older validly-signed image to defeat anti-rollback. The body
 * and its digest are left intact, so the deny comes from the signature gate.
 *
 * @par MC/DC: not applicable -- drives the (unchanged) sig-verify decision with a
 * forged-metadata input; the decision's MC/DC pair is covered by the valid-image
 * (allow) and invalid-signature (deny) tests.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_forged_version_denied(void)
{
  TEST_BEGIN("ra8_rot: forged img_version -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Forge an anti-rollback bump. Body, digest, and signature are untouched, but
   * the signature covers the version, so verification must fail. */
  trailer.img_version = trailer.img_version + 1U;

  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra8_rot: forged img_version -> denied");
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
  TEST_BEGIN("ra8_rot: tampered body (hash mismatch) -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Tamper with the body AFTER signing: trailer.digest is now stale. */
  body[0] = (uint8_t)(body[0] ^ k_t_tamper_mask);

  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra8_rot: tampered body (hash mismatch) -> denied");
}

/**
 * @brief A missing or NULL trailer/body is DENIED (``k_ra8_err_null_ptr``).
 *
 * @par MC/DC:
 * Two independent NULL guards (``RA8_CHECK_NULL_PTR``), each a single
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
  TEST_BEGIN("ra8_rot: missing trailer / body -> denied");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t trailer;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &trailer);

  /* Missing trailer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, nullptr));

  /* Missing body. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_verify_image(nullptr, (uint32_t)k_test_rot_body_len, &trailer));

  TEST_END("ra8_rot: missing trailer / body -> denied");
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
 * ``k_ra8_err_validation_failed``.
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
  TEST_BEGIN("ra8_rot: trailer magic/version (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: both conditions false -> allow. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: bad magic. */
  ra8_rot_trailer_t bad_magic = good;
  bad_magic.magic             = (uint32_t)k_test_rot_bad_magic;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &bad_magic));

  /* V3: bad version. */
  ra8_rot_trailer_t bad_ver = good;
  bad_ver.version           = (uint32_t)k_test_rot_bad_ver;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &bad_ver));

  TEST_END("ra8_rot: trailer magic/version (MC/DC)");
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
 * ``k_ra8_err_invalid_size``. Each non-control vector keeps the other two
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
  TEST_BEGIN("ra8_rot: body_len sanity/cross-check (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: all three conditions false -> allow. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: body_len == 0 (c1 true). trailer.body_len also 0 so c3 stays false. */
  ra8_rot_trailer_t zero = good;
  zero.body_len          = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rot_verify_image(body, 0U, &zero));

  /* V3: body_len > MAX (c2 true). trailer.body_len matches so c3 stays false. */
  ra8_rot_trailer_t toobig = good;
  toobig.body_len          = (uint32_t)k_ra8_rot_body_max + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_ra8_rot_body_max + 1U, &toobig));

  /* V4: trailer.body_len != body_len (c3 true). len in range so c1/c2 false. */
  ra8_rot_trailer_t mismatch = good;
  mismatch.body_len          = (uint32_t)k_test_rot_body_len / 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &mismatch));

  TEST_END("ra8_rot: body_len sanity/cross-check (MC/DC)");
}

/**
 * @brief Signature-length sanity gate, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: ``sig_len == 0 || sig_len > k_ra8_rot_sig_bytes`` (2 conditions,
 * ``||``).
 * - V1: F,F (sig_len=valid)        -> false -> ALLOW (control)
 * - V2: T,F (sig_len=0)            -> true  -> DENY (varies c1)
 * - V3: F,T (sig_len=65 > 64)      -> true  -> DENY (varies c2)
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC. Deny returns
 * ``k_ra8_err_invalid_size``.
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
  TEST_BEGIN("ra8_rot: sig_len sanity (MC/DC)");

  uint8_t body[k_test_rot_body_len];
  fill_body(body, (uint32_t)k_test_rot_body_len);

  ra8_rot_trailer_t good;
  build_signed_trailer(body, (uint32_t)k_test_rot_body_len, &good);

  /* V1 control: valid sig_len -> allow. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &good));

  /* V2: sig_len == 0. */
  ra8_rot_trailer_t zero = good;
  zero.sig_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &zero));

  /* V3: sig_len > k_ra8_rot_sig_bytes. */
  ra8_rot_trailer_t toobig = good;
  toobig.sig_len           = (uint32_t)k_test_rot_siglen_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_rot_body_len, &toobig));

  TEST_END("ra8_rot: sig_len sanity (MC/DC)");
}

/**
 * @brief ``ra8_rot_trailer_after`` + ``ra8_rot_root_public_key`` accessors.
 *
 * @par MC/DC:
 * ``ra8_rot_trailer_after`` guard ``image_base == NULL`` and
 * ``body_len == 0 || body_len > MAX`` -- each driven to its deny (NULL)
 * return and to its pass return. ``ra8_rot_root_public_key`` NULL guards on
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
  TEST_BEGIN("ra8_rot: trailer_after + root_public_key accessors");

  uint8_t image[k_test_rot_body_len + sizeof(ra8_rot_trailer_t)] = {};

  /* trailer_after: valid -> points at body end. */
  const ra8_rot_trailer_t* at = ra8_rot_trailer_after(image, (uint32_t)k_test_rot_body_len);
  TEST_ASSERT(at == (const ra8_rot_trailer_t*)(image + k_test_rot_body_len));

  /* trailer_after: NULL base / out-of-range length -> nullptr. */
  TEST_ASSERT(ra8_rot_trailer_after(nullptr, (uint32_t)k_test_rot_body_len) == nullptr);
  TEST_ASSERT(ra8_rot_trailer_after(image, 0U) == nullptr);
  TEST_ASSERT(ra8_rot_trailer_after(image, (uint32_t)k_ra8_rot_body_max + 1U) == nullptr);

  /* root_public_key: success exposes a 65-byte uncompressed P-256 key. */
  const uint8_t* key = nullptr;
  uint32_t       len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_root_public_key(&key, &len));
  TEST_ASSERT(key != nullptr);
  TEST_ASSERT_EQ(k_ra8_rot_pubkey_bytes, len);

  /* root_public_key: NULL out-parameters -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rot_root_public_key(nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rot_root_public_key(&key, nullptr));

  TEST_END("ra8_rot: trailer_after + root_public_key accessors");
}

/**
 * @brief body_len-from-header path: the BLXNS layout the secure boot now uses.
 *
 * @details
 * Mirrors ``internal_ns_verify_or_deny``: an NS-image-shaped buffer carries a
 * ``{ magic, body_len }`` header at a fixed offset (``ns_base + 0x40``) INSIDE
 * the signed body; the verifier reads ``body_len`` from that header, locates the
 * trailer via ``ra8_rot_trailer_after(base, body_len)`` (== base + body_len), and
 * authenticates. This proves the self-describing ``[ body ][ trailer ]`` layout
 * verifies exactly like the copy-to-run boundary, that a tampered body is
 * rejected, and that a LIED header ``body_len`` default-denies (the security
 * argument for trusting a length read from the untrusted image).
 *
 * @par MC/DC:
 * Integration of three already-unit-covered deny decisions, each driven here via
 * a header-derived length:
 * - genuine  (V1): correct body_len -> trailer at base+len -> ALLOW (control).
 * - tampered (V2): flip a body byte -> digest pre-check
 *   ``!internal_ct_equal(...)`` true -> DENY (k_ra8_err_checksum_mismatch).
 * - lied len (V3): wrong header body_len -> trailer_after points at body bytes ->
 *   trailer magic decision ``magic != MAGIC`` true -> DENY
 *   (k_ra8_err_validation_failed).
 * Each vector varies exactly one deny condition against the genuine control:
 * minimal MC/DC for the composed path.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_body_len_from_header(void)
{
  TEST_BEGIN("ra8_rot: body_len-from-header path (genuine / tampered / lied len)");

  /* NS-image-shaped buffer: [ body (with header at 0x40) ][ trailer ]. */
  alignas(4) uint8_t img[k_test_ns_body_len + sizeof(ra8_rot_trailer_t)] = {};
  fill_body(img, (uint32_t)k_test_ns_body_len);
  write_u32_le(img, (uint32_t)k_test_ns_hdr_off, (uint32_t)k_test_ns_magic);
  write_u32_le(img, (uint32_t)k_test_ns_hdr_off + 4U, (uint32_t)k_test_ns_body_len);

  /* Sign the whole body (the embedded header is inside the signed region), and
   * place the trailer at base + body_len so trailer_after finds it. */
  build_signed_trailer(img,
                       (uint32_t)k_test_ns_body_len,
                       (ra8_rot_trailer_t*)&img[k_test_ns_body_len]);

  /* V1 genuine: read body_len from the header, locate the trailer, verify. */
  const uint32_t len = read_u32_le(img, (uint32_t)k_test_ns_hdr_off + 4U);
  TEST_ASSERT_EQ(k_test_ns_body_len, len);
  const ra8_rot_trailer_t* trailer = ra8_rot_trailer_after(img, len);
  TEST_ASSERT(trailer == (const ra8_rot_trailer_t*)&img[k_test_ns_body_len]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_verify_image(img, len, trailer));

  /* V2 tampered body: flip a body byte past the header -> digest mismatch. */
  img[k_test_ns_tamper_off] ^= k_t_tamper_mask;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, ra8_rot_verify_image(img, len, trailer));
  img[k_test_ns_tamper_off] ^= k_t_tamper_mask; /* restore */

  /* V3 lied header body_len: trailer_after points into the body (not the real
   * trailer), whose bytes are not a valid trailer -> default-deny. */
  const uint32_t           lied  = (uint32_t)k_test_ns_lied_len;
  const ra8_rot_trailer_t* wrong = ra8_rot_trailer_after(img, lied);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_rot_verify_image(img, lied, wrong));

  TEST_END("ra8_rot: body_len-from-header path (genuine / tampered / lied len)");
}

/**
 * @brief ``internal_ct_equal`` NULL-guard OR decision, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: ``(a == nullptr) || (b == nullptr)`` (2 conditions, ``||``) -- the
 * constant-time digest compare's NULL guard.
 * - V1: a!=NULL, b!=NULL -> F,F -> false (proceeds; equal buffers -> true).
 * - V2: a==NULL, b!=NULL -> T,. -> true  (returns false; varies a).
 * - V3: a!=NULL, b==NULL -> F,T -> true  (returns false; varies b).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC. The static helper is
 * reached white-box via this file's ``#include "ra8_rot.c"``; the guard's true
 * arms are defensive and not presentable through ``ra8_rot_verify_image`` (which
 * only ever passes two live stack buffers), so a direct call is the only path.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_rot_ct_equal_mcdc(void)
{
  TEST_BEGIN("ra8_rot: internal_ct_equal NULL guard (MC/DC)");

  uint8_t a[k_ra8_rot_digest_bytes];
  uint8_t b[k_ra8_rot_digest_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rot_digest_bytes; ++i) {
    a[i] = (uint8_t)i;
    b[i] = (uint8_t)i;
  }

  /* V1 control: both non-NULL, equal buffers -> guard false -> true. */
  TEST_ASSERT(internal_ct_equal(a, b, (uint32_t)k_ra8_rot_digest_bytes));
  /* V2: a == NULL -> guard true -> false (varies the first condition). */
  TEST_ASSERT(!internal_ct_equal(nullptr, b, (uint32_t)k_ra8_rot_digest_bytes));
  /* V3: b == NULL -> guard true -> false (varies the second condition). */
  TEST_ASSERT(!internal_ct_equal(a, nullptr, (uint32_t)k_ra8_rot_digest_bytes));
  /* Non-compound branches: zero length is rejected, a single differing byte
   * makes the constant-time fold report inequality. */
  TEST_ASSERT(!internal_ct_equal(a, b, 0U));
  b[0] = (uint8_t)(b[0] ^ k_t_byte_mask);
  TEST_ASSERT(!internal_ct_equal(a, b, (uint32_t)k_ra8_rot_digest_bytes));

  TEST_END("ra8_rot: internal_ct_equal NULL guard (MC/DC)");
}

int main(void)
{
  /* The verifier ensures the PSA facade is ready, but the test's signing
   * helpers need it too; initialize once up front. */
  (void)ra8_psa_crypto_init();

  test_rot_ct_equal_mcdc();
  test_rot_valid_image_allowed();
  test_rot_invalid_signature_denied();
  test_rot_forged_version_denied();
  test_rot_tampered_body_denied();
  test_rot_missing_trailer_denied();
  test_rot_trailer_header_mcdc();
  test_rot_length_mcdc();
  test_rot_siglen_mcdc();
  test_rot_helpers();
  test_rot_body_len_from_header();
  return 0;
}
