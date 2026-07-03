/**
 * @file ra_rot.c
 * @brief Root-of-trust signed-image verifier -- SHA-256 + ECDSA-P256 gate.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * See ``ra_rot.h`` for the full contract and signed-image format. This file
 * owns the provisioned root public key, the digest re-computation (RSIP on
 * target, ``ra_psa`` software hash under ``RA_SIMULATOR_MODE``), and the
 * default-deny decision logic shared by the copy-to-run
 * (``ra_dfu_launch``) and BLXNS (``ra_tz_secure_boot_jump_ns``) boundaries.
 *
 * No raw MMIO is performed here: the hash and the ECDSA verify are reached
 * through ``ra_rsip_sha256`` and ``ra_psa_verify_hash`` respectively, both of
 * which carry their own register-level HUM citations.
 *
 * The whole module is opt-in behind ``RA_ENABLE_ROOT_OF_TRUST`` (default OFF),
 * mirroring ``RA_BOOT_ENABLE_CACHE_MPU``. With the flag OFF this file compiles
 * to essentially nothing -- only the link-free declarations from ``ra_rot.h``
 * -- so an app that links ``ra_dfu`` but not ``ra_psa_crypto`` / the RSIP HAL
 * does not break, and the launch boundaries skip verification (existing
 * unsigned apps are unchanged). See ``ra_rot.h`` for the enable contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rot.h"

#ifdef RA_ENABLE_ROOT_OF_TRUST

#include "ra_check.h"
#include "ra_psa_crypto.h"
#include "ra_rsip_core.h"

/**
 * @var s_tag
 * @brief Logger tag for the root-of-trust module.
 * @details Component identifier used by ``ra_log_*`` / ``RA_CHECK_*`` so
 *          root-of-trust log lines are easy to grep for in RTT output.
 * @note    File-private; never accessed from outside.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const char* s_tag = "ROT";

/**
 * @var s_rot_root_pubkey
 * @brief Provisioned root public key trusted to authenticate launched images.
 *
 * @details
 * Uncompressed NIST P-256 public key ``0x04 || X(32) || Y(32)`` (SEC1 v2
 * Sec 2.3.3). The verifier imports this key with verify-only usage and
 * checks every image's ECDSA-P256 signature against it. In the Secure
 * firmware this is the single anchor of the chain of trust.
 *
 * @warning Do not modify at runtime; the key is the trust anchor.
 * @note    TODO(provision real root public key): the bytes below are a
 *          deterministic placeholder so the verify path is exercised
 *          end-to-end on host. Before any production / silicon use this MUST
 *          be replaced with the real root public key from the signing key
 *          ceremony, and on-silicon ECDSA known-answer-test validation MUST
 *          be added.
 * @since   0.1.0
 */
static const uint8_t s_rot_root_pubkey[k_ra_rot_pubkey_bytes] = {
  0x04U, /* uncompressed-point marker */
  /* X (32 bytes) -- TODO(provision real root public key). */
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
  0x77U,
  0x88U,
  0x99U,
  0xAAU,
  0xBBU,
  0xCCU,
  0xDDU,
  0xEEU,
  0xFFU,
  0x00U,
  0x10U,
  0x20U,
  0x30U,
  0x40U,
  0x50U,
  0x60U,
  0x70U,
  0x80U,
  0x90U,
  0xA0U,
  0xB0U,
  0xC0U,
  0xD0U,
  0xE0U,
  0xF0U,
  0x01U,
  /* Y (32 bytes) -- TODO(provision real root public key). */
  0x02U,
  0x03U,
  0x04U,
  0x05U,
  0x06U,
  0x07U,
  0x08U,
  0x09U,
  0x0AU,
  0x0BU,
  0x0CU,
  0x0DU,
  0x0EU,
  0x0FU,
  0x1AU,
  0x2BU,
  0x3CU,
  0x4DU,
  0x5EU,
  0x6FU,
  0x7AU,
  0x8BU,
  0x9CU,
  0xADU,
  0xBEU,
  0xCFU,
  0xDAU,
  0xEBU,
  0xFCU,
  0x0DU,
  0x1EU,
  0x2FU,
};

/**
 * @brief Constant-time byte-array equality (no early-out on first mismatch).
 *
 * @details
 * Folds the XOR of every byte pair into an accumulator so the comparison
 * time does not depend on where the first differing byte sits. Used to
 * compare the re-computed digest against the trailer digest.
 *
 * @param[in] a   First buffer; non-NULL.
 * @param[in] b   Second buffer; non-NULL.
 * @param[in] len Number of bytes to compare; must be > 0.
 *
 * @return ``true`` iff the two buffers are byte-for-byte equal.
 * @retval true  ``len`` bytes match.
 * @retval false A pointer is NULL, ``len`` is 0, or any byte differs.
 *
 * @pre ``a`` and ``b`` each address at least ``len`` readable bytes.
 * @pre ``len`` is bounded by the caller (here ``k_ra_rot_digest_bytes``).
 * @post No state is mutated.
 * @post The result depends only on the inputs.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
static bool internal_ct_equal(const uint8_t* a, const uint8_t* b, uint32_t len)
{
  if ((a == nullptr) || (b == nullptr)) {
    return false;
  }
  if (len == 0U) {
    return false;
  }
  uint8_t diff = 0U;
  /* Bounded by ``len`` (<= k_ra_rot_digest_bytes at the only call site):
   * a statically bounded fold -- NASA Rule 2 compliant. */
  for (uint32_t i = 0U; i < len; ++i) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0U;
}

/**
 * @brief Re-compute the SHA-256 digest of an image body.
 *
 * @details
 * On target the Secure RSIP HASH engine (``ra_rsip_sha256``) is used. Under
 * ``RA_SIMULATOR_MODE`` the engine is unavailable, so the ``ra_psa`` software
 * SHA-256 stand-in is used instead; both produce an identical 32-byte digest.
 *
 * @param[in]  body     Image body; non-NULL.
 * @param[in]  body_len Body length in bytes.
 * @param[out] digest   ::k_ra_rot_digest_bytes output buffer; non-NULL.
 *
 * @return ra_err_t Error code from the underlying hash primitive.
 * @retval k_ra_ok            Digest written.
 * @retval k_ra_err_null_ptr  ``body`` or ``digest`` was NULL.
 * @retval k_ra_err_hw_error  Underlying hash engine reported a fault.
 *
 * @pre On target, ``ra_rsip_init`` ran; under sim, ``ra_psa_crypto_init`` ran.
 * @pre ``digest`` addresses at least ::k_ra_rot_digest_bytes bytes.
 * @post On ``k_ra_ok``, ``digest`` holds SHA-256 of ``body``.
 * @post No image bytes are modified.
 *
 * @note Not thread-safe (the hash engines are single-context).
 * @since 0.1.0
 */
static ra_err_t internal_compute_digest(const uint8_t* body, uint32_t body_len, uint8_t* digest)
{
  RA_CHECK_NULL_PTR(body, s_tag, "body");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest");

#ifdef RA_SIMULATOR_MODE
  size_t produced = 0U;
  return ra_psa_hash_compute(k_ra_psa_alg_sha_256,
                             body,
                             (size_t)body_len,
                             digest,
                             (size_t)k_ra_rot_digest_bytes,
                             &produced);
#else
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 -- RSIP HASH engine, fully
   * abstracted by ra_rsip_sha256; no raw register access is added here. */
  return ra_rsip_sha256(body, body_len, digest);
#endif
}

/**
 * @enum rot_serial_t
 * @brief Little-endian serialisation constant for anti-rollback binding.
 * @details Bit width of one octet, used to spread ``img_version`` across its
 *          four leading bytes in the signed material.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rot_octet_bits = 8U, /**< Bits per octet for little-endian img_version. */
} rot_serial_t;

/**
 * @brief Bind the anti-rollback ``img_version`` into the signed digest.
 *
 * @details
 * The ECDSA signature authenticates ``SHA-256(img_version_le32 || body_digest)``
 * rather than the bare body digest, so a forged ``img_version`` presented on an
 * otherwise validly-signed body no longer verifies (the recomputed signed
 * material changes, and the signature over the original material fails). The
 * 36-byte concatenation lives on the stack -- no dynamic allocation -- and is
 * hashed by the same engine as the body digest.
 *
 * @param[in]  img_version Anti-rollback image version from the trailer.
 * @param[in]  digest      ::k_ra_rot_digest_bytes body digest; non-NULL.
 * @param[out] out         ::k_ra_rot_digest_bytes signed-material buffer; non-NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           ``out`` holds the version-bound digest.
 * @retval k_ra_err_null_ptr ``digest`` or ``out`` was NULL.
 * @retval k_ra_err_*        Hash-engine error propagated from the digest step.
 *
 * @pre ``digest`` addresses ::k_ra_rot_digest_bytes readable bytes.
 * @pre ``out`` addresses ::k_ra_rot_digest_bytes writable bytes.
 * @post On ``k_ra_ok``, ``out`` == SHA-256(img_version_le || digest).
 * @post No image bytes are modified.
 *
 * @note Not thread-safe (shares the single-context hash engine).
 * @since 0.1.0
 */
static ra_err_t internal_bind_version(uint32_t img_version, const uint8_t* digest, uint8_t* out)
{
  RA_CHECK_NULL_PTR(digest, s_tag, "digest");
  RA_CHECK_NULL_PTR(out, s_tag, "out");

  uint8_t combined[sizeof(uint32_t) + k_ra_rot_digest_bytes] = {};
  for (uint8_t i = 0U; i < (uint8_t)sizeof(uint32_t); ++i) {
    combined[i] = (uint8_t)(img_version >> ((uint32_t)i * (uint32_t)k_rot_octet_bits));
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_rot_digest_bytes; ++i) {
    combined[(size_t)sizeof(uint32_t) + (size_t)i] = digest[i];
  }
  return internal_compute_digest(combined, (uint32_t)sizeof(combined), out);
}

/**
 * @brief Verify an ECDSA-P256 signature over a digest with the root key.
 *
 * @details
 * Imports ::s_rot_root_pubkey with verify-only usage, calls
 * ``ra_psa_verify_hash`` (algorithm ``k_ra_psa_alg_ecdsa_sha_256``), then
 * destroys the transient key. The PSA facade must already be initialized by
 * the caller (the gate ensures this once per call).
 *
 * @param[in] digest  ::k_ra_rot_digest_bytes message digest; non-NULL.
 * @param[in] sig     Signature bytes (raw r||s); non-NULL.
 * @param[in] sig_len Signature length in bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Signature is valid.
 * @retval k_ra_err_crc_mismatch Signature did not verify.
 * @retval k_ra_err_null_ptr     ``digest`` or ``sig`` was NULL.
 * @retval k_ra_err_*            Key import / verify backend error.
 *
 * @pre ``ra_psa_crypto_init`` returned success (or ``k_ra_err_exists``).
 * @pre ``digest`` addresses ::k_ra_rot_digest_bytes readable bytes.
 * @post The transient verify key is destroyed on every return path.
 * @post No image bytes are modified.
 *
 * @note Not thread-safe: mutates the shared PSA static key pool.
 * @since 0.1.0
 */
static ra_err_t internal_verify_sig(const uint8_t* digest, const uint8_t* sig, uint32_t sig_len)
{
  RA_CHECK_NULL_PTR(digest, s_tag, "digest");
  RA_CHECK_NULL_PTR(sig, s_tag, "sig");

  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_ecc_p256_pub,
    .alg   = k_ra_psa_alg_ecdsa_sha_256,
    .usage = k_ra_psa_usage_verify,
  };
  ra_psa_key_t   key = nullptr;
  const ra_err_t imp_err =
    ra_psa_key_import(&key, &attr, s_rot_root_pubkey, (size_t)sizeof(s_rot_root_pubkey));
  RA_RETURN_ON_ERROR(imp_err, s_tag, "rot: import root key failed");

  const ra_err_t ver_err = ra_psa_verify_hash(key,
                                              k_ra_psa_alg_ecdsa_sha_256,
                                              digest,
                                              (size_t)k_ra_rot_digest_bytes,
                                              sig,
                                              (size_t)sig_len);
  (void)ra_psa_key_destroy(key);
  return ver_err;
}

ra_err_t
ra_rot_verify_image(const uint8_t* body, uint32_t body_len, const ra_rot_trailer_t* trailer)
{
  RA_CHECK_NULL_PTR(body, s_tag, "body");
  RA_CHECK_NULL_PTR(trailer, s_tag, "trailer");

  /* Trailer header: a missing or malformed trailer is a default-deny. */
  if ((trailer->magic != (uint32_t)k_ra_rot_trailer_magic) ||
      (trailer->version != (uint32_t)k_ra_rot_version)) {
    ra_log_error(s_tag, "rot: trailer magic/version invalid");
    return k_ra_err_validation_failed;
  }

  /* Body-length sanity + cross-check against the trailer's recorded length. */
  if ((body_len == 0U) || (body_len > (uint32_t)k_ra_rot_body_max) ||
      (trailer->body_len != body_len)) {
    ra_log_error(s_tag, "rot: body_len out of range / mismatch");
    return k_ra_err_invalid_size;
  }

  /* Signature-length sanity (bounds the bytes handed to the verifier). */
  if ((trailer->sig_len == 0U) || (trailer->sig_len > (uint32_t)k_ra_rot_sig_bytes)) {
    ra_log_error(s_tag, "rot: sig_len invalid");
    return k_ra_err_invalid_size;
  }

  /* Ensure the PSA facade is ready (the sim hash and the ECDSA verify both
   * route through it). Already-initialized is fine. */
  const ra_err_t psa_err = ra_psa_crypto_init();
  if ((psa_err != k_ra_ok) && (psa_err != k_ra_err_exists)) {
    ra_log_error(s_tag, "rot: psa init failed");
    return psa_err;
  }

  /* Re-compute the body digest. The signature's authority is asserted over
   * THIS freshly-computed digest, never over the trailer's stored digest. */
  uint8_t        digest[k_ra_rot_digest_bytes] = {};
  const ra_err_t hash_err                      = internal_compute_digest(body, body_len, digest);
  RA_RETURN_ON_ERROR(hash_err, s_tag, "rot: hash compute failed");

  /* Tamper pre-check: the re-computed digest must equal the trailer digest. */
  if (!internal_ct_equal(digest, trailer->digest, (uint32_t)k_ra_rot_digest_bytes)) {
    ra_log_error(s_tag, "rot: body digest mismatch (tampered)");
    return k_ra_err_checksum_mismatch;
  }

  /* Bind the anti-rollback img_version into the signed material so a forged
   * version riding on a validly-signed body fails verification (T5-05): the
   * signature's authority is asserted over SHA-256(img_version_le || digest),
   * not the bare body digest. */
  uint8_t        signed_material[k_ra_rot_digest_bytes] = {};
  const ra_err_t bind_err = internal_bind_version(trailer->img_version, digest, signed_material);
  RA_RETURN_ON_ERROR(bind_err, s_tag, "rot: version-bind hash failed");

  /* Authority: ECDSA-P256 verify over the version-bound digest. */
  const ra_err_t sig_err = internal_verify_sig(signed_material, trailer->sig, trailer->sig_len);
  RA_RETURN_ON_ERROR(sig_err, s_tag, "rot: signature verify failed");

  return k_ra_ok; /* image is authentic -- launch permitted */
}

const ra_rot_trailer_t* ra_rot_trailer_after(const void* image_base, uint32_t body_len)
{
  if (image_base == nullptr) {
    return nullptr;
  }
  if ((body_len == 0U) || (body_len > (uint32_t)k_ra_rot_body_max)) {
    return nullptr;
  }
  const uint8_t* base = (const uint8_t*)image_base;
  /* The trailer sits immediately after the body; image bodies are placed at
   * 32-byte (page) multiples, so this address satisfies the struct alignment.
   * The void* hop documents the intent and silences -Wcast-align. */
  return (const ra_rot_trailer_t*)(const void*)(base + body_len);
}

ra_err_t ra_rot_root_public_key(const uint8_t** out_key, uint32_t* out_len)
{
  RA_CHECK_NULL_PTR(out_key, s_tag, "out_key");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len");
  *out_key = s_rot_root_pubkey;
  *out_len = (uint32_t)sizeof(s_rot_root_pubkey);
  return k_ra_ok;
}

#endif /* RA_ENABLE_ROOT_OF_TRUST */
