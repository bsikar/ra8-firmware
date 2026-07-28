/**
 * @file ra8_rot.h
 * @brief Root-of-trust signed-image verifier (SHA-256 + ECDSA-P256, default-deny).
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * The shared authenticity gate placed at the firmware's two trust boundaries:
 *
 *  - **copy-to-run** (``libs/ra8_dfu/src/ra8_dfu_launch.c``): the bootloader
 *    must authenticate a slot image before copying it to SRAM and branching
 *    into it.
 *  - **BLXNS** (``libs/ra8_tz_secure_boot/src/ra8_tz_secure_boot.c``): the
 *    Secure world must authenticate the Non-Secure image before handing
 *    control to it.
 *
 * Historically both boundaries trusted a CRC32 only (integrity, not
 * authenticity): a CRC-correct image of any origin would launch. This module
 * adds the missing *authenticity* check -- a digital signature anchored to a
 * public key provisioned into the Secure firmware -- and enforces
 * **default-deny**: any failure (missing trailer, wrong format, hash mismatch,
 * or an invalid signature) returns an error and the caller must NOT launch.
 *
 * ## Signed-image format
 *
 * A signed image is ``[ body (body_len bytes) ] [ ra8_rot_trailer_t ]``. The
 * trailer carries the SHA-256 digest of the body and an ECDSA-P256 signature
 * (raw ``r || s``) over that digest. The verifier:
 *
 *  1. confirms the trailer magic + version (missing / malformed trailer
 *     -> deny);
 *  2. re-computes SHA-256 over the body (the RSIP hardware hash on target,
 *     the ``ra8_psa`` software hash in ``RA8_OFF_TARGET``);
 *  3. confirms the freshly-computed digest equals the trailer digest
 *     (tampered body -> deny); and
 *  4. verifies the ECDSA-P256 signature over the freshly-computed digest
 *     against the provisioned root public key (forged / mis-signed -> deny).
 *
 * The signature -- not the digest field and not any CRC -- is the authority:
 * step 4 verifies over the *re-computed* digest, so a matching trailer digest
 * is never sufficient on its own.
 *
 * ## Enabling (opt-in, default OFF)
 *
 * The whole root of trust is gated behind the ``RA8_ENABLE_ROOT_OF_TRUST``
 * build flag (default OFF), mirroring ``RA8_BOOT_ENABLE_CACHE_MPU``:
 *
 *  - **Flag OFF (default):** ``ra8_rot.c`` compiles to nothing and pulls in no
 *    crypto libraries; the launch boundaries do NOT verify. Existing apps --
 *    including unsigned images (``dfu_copy_to_run``, ``dfu_bootloader``, and
 *    the current Non-Secure images) -- are unchanged and launch as before.
 *  - **Flag ON:** the app opts in via ``target_compile_definitions(<app>
 *    PRIVATE RA8_ENABLE_ROOT_OF_TRUST)`` and MUST then also (a) link
 *    ``ra8_psa_crypto`` + the RSIP HAL, and (b) sign its DFU / Non-Secure
 *    images with an ::ra8_rot_trailer_t -- otherwise every launch
 *    **default-denies**.
 *
 * The declarations below are always visible (they reference no external
 * symbols, so a flag-off translation unit has zero extra link dependencies);
 * only the *implementation* in ``ra8_rot.c`` is flag-gated.
 *
 * @note On silicon, the ECDSA-P256 + SHA-256 known-answer tests pass on the M85
 *       (hil_needs_revalidation/psa_crypto_hil, rsip_sha256_kat), the real root public
 *       key is provisioned (see ra8_rot.c), and RoT enforcement is proven
 *       end-to-end by hw_validated/hil/secure_boot_hil. The host unit tests
 *       enable the flag and exercise the gate's decision logic against the
 *       ``RA8_OFF_TARGET`` crypto stand-ins; the remaining production step is
 *       to sign each shipped image and enable the flag on the boot path.
 *
 * @see @ref md_docs_2formats_2ROT1 -- the full ROT1 wire-format specification
 *      (rationale, algorithms, worked example, failure modes).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_rot_size_t
 * @brief Fixed byte-lengths of the cryptographic fields in a signed image.
 *
 * @details
 * SHA-256 produces a 32-byte digest (FIPS 180-4). An ECDSA-P256 signature is
 * the raw concatenation ``r || s`` of two 32-byte field elements = 64 bytes
 * (FIPS 186-4). An uncompressed NIST P-256 public key is the 65-byte string
 * ``0x04 || X(32) || Y(32)`` (SEC1 v2 Sec 2.3.3).
 */
typedef enum : uint8_t {
  k_ra8_rot_digest_bytes = 32U, /**< SHA-256 digest length (FIPS 180-4).         */
  k_ra8_rot_sig_bytes    = 64U, /**< ECDSA-P256 raw r||s signature length.       */
  k_ra8_rot_pubkey_bytes = 65U, /**< Uncompressed P-256 public key (0x04||X||Y). */
} ra8_rot_size_t;

/**
 * @enum ra8_rot_const_t
 * @brief Trailer magic / version and the body-length sanity cap.
 *
 * @details
 * The 1 MiB body cap comfortably exceeds both the largest DFU slot image
 * (``k_ra8_dfu_img_max`` ~= 448 KiB) and the Non-Secure MRAM partition
 * (512 KiB), while bounding the hash loop so a corrupt ``body_len`` cannot
 * drive an unbounded read (NASA Rule 2).
 */
typedef enum : uint32_t {
  k_ra8_rot_trailer_magic = 0x524F5431U, /**< ASCII "ROT1" -- signed-image marker.  */
  k_ra8_rot_version       = 0x00000001U, /**< Trailer format version.               */
  k_ra8_rot_body_max      = 0x00100000U, /**< Max signable body length (1 MiB cap). */
} ra8_rot_const_t;

/**
 * @struct ra8_rot_trailer_t
 * @brief Authenticity trailer appended after a signed image body.
 *
 * @details
 * Laid out immediately after the ``body_len`` image body. Every field is
 * 32-bit aligned. ``sig_len`` records the active signature length so the
 * same struct carries a 64-byte ECDSA-P256 signature on target and the
 * 32-byte fake stand-in signature in ``RA8_OFF_TARGET`` (the unused
 * tail of ``sig`` is ignored). ``img_version`` is the monotonic anti-rollback
 * version consumed by ``ra8_dfu_antirollback.h`` (downgrade protection).
 *
 * @invariant ``magic == k_ra8_rot_trailer_magic`` for a signed image.
 * @invariant ``version == k_ra8_rot_version`` for this verifier revision.
 * @invariant ``body_len`` equals the byte length the ``digest`` covers.
 * @invariant ``sig_len <= k_ra8_rot_sig_bytes``.
 *
 * @note ``img_version`` IS covered by ``sig``: the ECDSA signature
 *       authenticates ``SHA-256(img_version_le || body_digest)`` rather than
 *       the bare body digest (see ``ra8_rot_verify_image``), so an attacker
 *       holding an older validly-signed image cannot raise this field to
 *       defeat anti-rollback -- the forged version invalidates the signature.
 *       The signing tool ``tools/rot_sign.py`` binds the identical material.
 *       ``digest`` itself still covers only the body. See
 *       ``ra8_dfu_antirollback.h`` for the monotonic-counter check.
 *
 * @see ra8_rot_verify_image
 */
typedef struct {
  uint32_t magic;                          /**< ::k_ra8_rot_trailer_magic.            */
  uint32_t version;                        /**< ::k_ra8_rot_version (trailer format). */
  uint32_t img_version;                    /**< Monotonic anti-rollback image ver.    */
  uint32_t body_len;                       /**< Body length the digest covers.        */
  uint32_t sig_len;                        /**< Active signature length, bytes.       */
  uint8_t  digest[k_ra8_rot_digest_bytes]; /**< SHA-256 of the body (pre-check).      */
  uint8_t  sig[k_ra8_rot_sig_bytes];       /**< ECDSA-P256 raw r||s over the digest.  */
} ra8_rot_trailer_t;

/* Five 32-bit metadata words (magic, version, img_version, body_len, sig_len)
 * precede the digest + signature byte blocks; the assert pins the no-padding
 * layout. The trailing ``+ sizeof(uint32_t)`` accounts for img_version (4 is in
 * the magic-number ignore set; 5 is not, so it is summed via sizeof). */
static_assert(sizeof(ra8_rot_trailer_t) == (4U * sizeof(uint32_t)) + sizeof(uint32_t) +
                                             (uint32_t)k_ra8_rot_digest_bytes +
                                             (uint32_t)k_ra8_rot_sig_bytes,
              "ra8_rot_trailer_t must have no implicit padding");

/**
 * @brief Authenticate a signed image: SHA-256 + ECDSA-P256, default-deny.
 *
 * @details
 * The shared root-of-trust gate for both the copy-to-run and BLXNS
 * boundaries. Re-computes SHA-256 over ``[body, body + body_len)``
 * (``ra8_rsip_sha256`` on target, ``ra8_psa_hash_compute`` under
 * ``RA8_OFF_TARGET``), cross-checks it against ``trailer->digest``, then
 * verifies ``trailer->sig`` over the *freshly-computed* digest against the
 * provisioned root public key via ``ra8_psa_verify_hash`` (algorithm
 * ``k_ra8_psa_alg_ecdsa_sha_256``). The signature is the authority; the
 * trailer digest is only a fast pre-check.
 *
 * Any failure returns a non-``k_ra8_ok`` error -- the caller MUST treat that
 * as DENY and not launch / not branch.
 *
 * @param[in] body     Pointer to the image body's first byte; non-NULL.
 * @param[in] body_len Body length in bytes; ``(0, k_ra8_rot_body_max]`` and
 *                     equal to ``trailer->body_len``.
 * @param[in] trailer  Authenticity trailer that follows the body; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Image is authentic -- launch is permitted.
 * @retval k_ra8_err_null_ptr        ``body`` or ``trailer`` is NULL (missing
 *                                  trailer -> deny).
 * @retval k_ra8_err_validation_failed Trailer magic / version wrong (malformed
 *                                  trailer -> deny).
 * @retval k_ra8_err_invalid_size    ``body_len`` out of range, mismatched
 *                                  against the trailer, or ``sig_len`` invalid.
 * @retval k_ra8_err_checksum_mismatch Re-computed digest differs from the
 *                                  trailer digest (tampered body -> deny).
 * @retval k_ra8_err_crc_mismatch    Signature did not verify (forged /
 *                                  mis-signed -> deny).
 * @retval k_ra8_err_hw_error        Underlying hash / verify engine fault.
 *
 * @pre ``body`` addresses at least ``body_len`` readable bytes.
 * @pre ``trailer`` addresses a readable ::ra8_rot_trailer_t.
 * @post On any non-``k_ra8_ok`` return the caller must NOT launch the image.
 * @post No image bytes are modified; the verifier is read-only over its input.
 *
 * @note Not thread-safe: imports + destroys a transient PSA verify key in the
 *       shared static key pool. Call from the single-threaded boot path.
 * @note On-silicon ECDSA-P256 KAT (hil_needs_revalidation/psa_crypto_hil) and root-key
 *       provisioning are done; enforcement is proven by
 *       hw_validated/hil/secure_boot_hil.
 *
 * @see ra8_rot_trailer_after
 * @see ra8_psa_verify_hash
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rot_verify_image(const uint8_t* body, uint32_t body_len, const ra8_rot_trailer_t* trailer);

/**
 * @brief Locate the trailer that immediately follows a signed image body.
 *
 * @details
 * For the ``[ body ] [ trailer ]`` layout the trailer begins at
 * ``image_base + body_len``. A thin, side-effect-free helper so the
 * copy-to-run path expresses the layout contract explicitly rather than with
 * an inline pointer cast.
 *
 * @param[in] image_base Base of the signed image (its body); non-NULL.
 * @param[in] body_len   Body length in bytes; ``(0, k_ra8_rot_body_max]``.
 *
 * @return Pointer to the trailer that follows the body.
 * @retval non-NULL ``image_base + body_len`` reinterpreted as a trailer.
 * @retval nullptr  ``image_base`` is NULL or ``body_len`` is out of range.
 *
 * @pre ``image_base`` addresses at least ``body_len + sizeof(ra8_rot_trailer_t)``
 *      readable bytes.
 * @pre ``body_len`` is the length recorded in the image's DFU header.
 * @post No state is mutated; the result depends only on the inputs.
 * @post The returned pointer, when non-NULL, lies at ``image_base + body_len``.
 *
 * @note Thread-safe (pure; no statics).
 * @see ra8_rot_verify_image
 * @since 0.1.0
 */
const ra8_rot_trailer_t* ra8_rot_trailer_after(const void* image_base, uint32_t body_len);

/**
 * @brief Expose the provisioned root public key (host / test only).
 *
 * @details
 * Host-only accessor: lets the unit tests forge a correctly-signed trailer
 * using the *same* key bytes the verifier trusts, so the gate's decision
 * logic can be driven end-to-end against the ``RA8_OFF_TARGET`` crypto
 * stand-ins. The key is a public value, so exposing it leaks nothing.
 *
 * @param[out] out_key Receives a pointer to the embedded key bytes; non-NULL.
 * @param[out] out_len Receives the key length in bytes; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``*out_key`` / ``*out_len`` populated.
 * @retval k_ra8_err_null_ptr ``out_key`` or ``out_len`` is NULL.
 *
 * @pre ``out_key`` and ``out_len`` are non-NULL.
 * @pre The caller is a host unit test (the symbol is a test helper).
 * @post ``*out_len == k_ra8_rot_pubkey_bytes`` on success.
 * @post No state is mutated.
 *
 * @note Test-only helper; production code never reads the key through this
 *       accessor.
 * @since 0.1.0
 */
RA8_TEST_HELPER ra8_err_t ra8_rot_root_public_key(const uint8_t** out_key, uint32_t* out_len);

#ifdef __cplusplus
}
#endif
