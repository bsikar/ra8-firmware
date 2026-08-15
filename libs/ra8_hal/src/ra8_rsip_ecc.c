/**
 * @file ra8_rsip_ecc.c
 * @brief RSIP-E50D elliptic-curve asymmetric path (ECDSA / ECDH / Ed25519)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Elliptic-curve slice of the RA8D2 RSIP-E50D asymmetric HAL driver, split out
 * of ``ra8_rsip_asym.c`` to keep every translation unit under the file-size
 * budget. Exposes:
 *
 * - ECDSA sign / verify over a pre-computed digest;
 * - ECDH key agreement;
 * - Ed25519 PureEdDSA sign / verify (RFC 8032).
 *
 * Every entry point is FAIL-CLOSED in production: HUM Ch 52 documents no
 * asymmetric command-register map for the RSIP-E50D, so the off-target-only command
 * path is gated behind the stub-crypto guard and a production build returns
 * ``k_ra8_err_not_supported``. The real ECDSA-P256 / ECDH / Ed25519 backend is
 * tf-psa-crypto on the M85, silicon-proven in psa_crypto_hil (issues #214 +
 * #181).
 *
 * The byte-lane streaming primitives ``internal_asym_push`` /
 * ``internal_asym_pull`` and the handle-tail zero helper
 * ``internal_zero_handle_tail`` are defined in ``ra8_rsip_asym.c`` and shared
 * via ``ra8_rsip_asym_internal.h``; the remaining cross-TU primitives
 * (``priv_load_handle``, ``priv_complete``) are declared in
 * ``ra8_rsip_internal.h``. The RSIP engine exposes no documented asymmetric
 * register interface (HUM Ch 52 is a feature overview, p 3302-3307), so the fake
 * command path here is a modelled fiction, not a real hardware sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rsip.h"
#include "ra8_rsip_asym_internal.h"
#include "ra8_rsip_internal.h"
#include "ra8_rsip_regs.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra8_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix without
 * truncation. Each RSIP translation unit keeps its own private copy.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP";

/*
 * The RSIP-E50D asymmetric signature / key-agreement family (ECDSA sign /
 * verify, ECDH, Ed25519 PureEdDSA) is NOT backed by a documented register
 * interface on this silicon. HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" is a
 * six-page feature overview (p 3302-3307) with no command-register map; the
 * vendor engine is driven through an encrypted firmware mailbox, not the MMIO
 * opcodes modelled below. The command-path bodies here only round-trip the
 * host register fake; they do NOT compute a real ECDSA / ECDH / RFC 8032
 * result. They compile only under the insecure-stub / off-target guard so a
 * production image gets the fail-closed #else and can never mistake these
 * bytes for a valid signature or shared secret. The real ECDSA-P256 / ECDH /
 * Ed25519 backend is tf-psa-crypto on the M85, silicon-proven in
 * psa_crypto_hil (issues #214 + #181). The register pokes below therefore
 * carry NO HUM citation: there is no real register map to cite.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)

/**
 * @enum ra8_rsip_curve_bytes_t
 * @brief Per-curve scalar / coordinate byte lengths (FIPS 186-4 / RFC 7748).
 */
typedef enum : uint32_t {
  k_ra8_rsip_curve_bytes_192 = 24U, /**< 192-bit curves: secp192r1.             */
  k_ra8_rsip_curve_bytes_224 = 28U, /**< 224-bit curves: secp224r1.             */
  k_ra8_rsip_curve_bytes_256 = 32U, /**< 256-bit curves: secp256*, ed25519.     */
  k_ra8_rsip_curve_bytes_384 = 48U, /**< 384-bit curves: secp384r1, brain384r1. */
  k_ra8_rsip_curve_bytes_512 = 64U, /**< 512-bit curves: brain512r1.            */
  k_ra8_rsip_curve_bytes_521 = 66U, /**< 521-bit curves: secp521r1.             */
} ra8_rsip_curve_bytes_t;

/**
 * @brief Map a curve to its scalar / coordinate byte length.
 *
 * @details
 * Pure lookup over ``ra8_rsip_curve_t``. The value is the FIPS 186-4 /
 * RFC 5639 parameter length in bytes, which is what every caller uses to
 * size the scalar, the affine coordinates and the r/s signature halves.
 * secp521r1 rounds 521 bits up to 66 bytes.
 *
 * @param[in] curve Curve selector.
 *
 * @return Byte length, or 0 for unknown.
 * @retval 0 ``curve`` is not a supported ``ra8_rsip_curve_t`` value.
 *
 * @pre ``curve`` is one of ``ra8_rsip_curve_t``.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result == FIPS / RFC parameter byte length.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_curve_bytes(ra8_rsip_curve_t curve)
{
  switch (curve) {
    case k_ra8_rsip_curve_secp192r1:
      return k_ra8_rsip_curve_bytes_192;
    case k_ra8_rsip_curve_secp224r1:
      return k_ra8_rsip_curve_bytes_224;
    case k_ra8_rsip_curve_secp256r1:
    case k_ra8_rsip_curve_brain256r1:
    case k_ra8_rsip_curve_ed25519:
    case k_ra8_rsip_curve_secp256k1:
      return k_ra8_rsip_curve_bytes_256;
    case k_ra8_rsip_curve_secp384r1:
    case k_ra8_rsip_curve_brain384r1:
      return k_ra8_rsip_curve_bytes_384;
    case k_ra8_rsip_curve_brain512r1:
      return k_ra8_rsip_curve_bytes_512;
    case k_ra8_rsip_curve_secp521r1:
      return k_ra8_rsip_curve_bytes_521;
    default:
      return 0U;
  }
}

ra8_err_t ra8_rsip_ecdsa_sign(const ra8_rsip_key_handle_t* key,
                              ra8_rsip_curve_t             curve,
                              const uint8_t*               digest,
                              uint32_t                     digest_len,
                              uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if (curve == k_ra8_rsip_curve_ed25519) {
    /* Ed25519 is PureEdDSA (RFC 8032), not ECDSA -- routing it through
     * the ECDSA opcode would not produce a valid signature. Callers
     * must use ra8_rsip_eddsa_sign(). */
    return k_ra8_err_invalid_arg;
  }
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  priv_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra8_rsip_off_asym_msg_in, digest, digest_len);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_ecdsa_sign;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_ecdsa_sign;

  const ra8_err_t err = priv_complete(k_ra8_rsip_mask_isr_asym_done);
  if (err != k_ra8_ok) {
    return err;
  }
  /* (r || s) */
  internal_asym_pull(k_ra8_rsip_off_asym_sig_out, signature, curve_bytes * 2U);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_ecdsa_verify(const ra8_rsip_key_handle_t* key,
                                ra8_rsip_curve_t             curve,
                                const uint8_t*               digest,
                                uint32_t                     digest_len,
                                const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if (curve == k_ra8_rsip_curve_ed25519) {
    /* Ed25519 is PureEdDSA (RFC 8032), not ECDSA -- callers must use
     * ra8_rsip_eddsa_verify(). */
    return k_ra8_err_invalid_arg;
  }
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  priv_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra8_rsip_off_asym_msg_in, digest, digest_len);
  internal_asym_push(k_ra8_rsip_off_asym_sig_in, signature, curve_bytes * 2U);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_ecdsa_verify;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_ecdsa_verify;

  return priv_complete(k_ra8_rsip_mask_isr_asym_done);
}

/**
 * @enum ra8_rsip_ed25519_size_t
 * @brief Ed25519 PureEdDSA byte sizes (RFC 8032 Section 5.1).
 */
typedef enum : uint32_t {
  k_ra8_rsip_ed25519_comp_bytes = 32U, /**< R or S component length.   */
  k_ra8_rsip_ed25519_sig_bytes  = 64U, /**< Signature (R || S) length. */
} ra8_rsip_ed25519_size_t;

ra8_err_t ra8_rsip_eddsa_sign(const ra8_rsip_key_handle_t* key,
                              const uint8_t*               msg,
                              uint32_t                     msg_len,
                              uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (key->alg != (uint32_t)k_ra8_rsip_oem_cmd_ecc_ed25519_priv) {
    return k_ra8_err_invalid_arg;
  }
  priv_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve) = (uint32_t)k_ra8_rsip_curve_ed25519;
  /* PureEdDSA signs the message itself, not a pre-computed digest (RFC 8032). */
  if (msg_len > 0U) {
    internal_asym_push(k_ra8_rsip_off_asym_msg_in, msg, msg_len);
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_eddsa_sign;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_eddsa_sign;

  const ra8_err_t err = priv_complete(k_ra8_rsip_mask_isr_asym_done);
  if (err != k_ra8_ok) {
    return err;
  }
  /* (R || S) */
  internal_asym_pull(k_ra8_rsip_off_asym_sig_out,
                     signature,
                     (uint32_t)k_ra8_rsip_ed25519_sig_bytes);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_eddsa_verify(const ra8_rsip_key_handle_t* key,
                                const uint8_t*               msg,
                                uint32_t                     msg_len,
                                const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (key->alg != (uint32_t)k_ra8_rsip_oem_cmd_ecc_ed25519_priv) {
    return k_ra8_err_invalid_arg;
  }
  priv_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve) = (uint32_t)k_ra8_rsip_curve_ed25519;
  if (msg_len > 0U) {
    internal_asym_push(k_ra8_rsip_off_asym_msg_in, msg, msg_len);
  }
  internal_asym_push(k_ra8_rsip_off_asym_sig_in, signature, (uint32_t)k_ra8_rsip_ed25519_sig_bytes);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_eddsa_verify;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_eddsa_verify;

  return priv_complete(k_ra8_rsip_mask_isr_asym_done);
}

/**
 * @brief Pull the ECDH shared-secret handle out of the engine.
 *
 * @details
 * The RSIP delivers the wrapped shared secret as an HMAC-SHA-256
 * handle: 1 algorithm word + N body words read from
 * ``ASYM_SHARED``. The unused tail of ``out->body[]`` is zero-padded
 * to avoid leaking stack contents.
 *
 * @param[out] out Destination handle.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``priv_complete`` has just returned ``k_ra8_ok``.
 *
 * @post ``out->alg`` and ``out->body_words`` reflect HMAC-SHA-256.
 * @post ``out->body[]`` has been fully populated and tail-zeroed.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ecdh_pull_shared(ra8_rsip_key_handle_t* out)
{
  /* The wrapped shared secret is delivered as an HMAC-SHA-256 handle. */
  out->alg        = k_ra8_rsip_oem_cmd_hmac_sha256;
  out->body_words = (uint32_t)k_ra8_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra8_rsip_reg32(k_ra8_rsip_off_asym_shared);
  }
  internal_zero_handle_tail(out, out->body_words);
}

ra8_err_t ra8_rsip_ecdh_compute(const ra8_rsip_key_handle_t* key,
                                ra8_rsip_curve_t             curve,
                                const uint8_t*               peer_x,
                                const uint8_t*               peer_y,
                                ra8_rsip_key_handle_t*       out)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(peer_x, s_tag, "peer_x must not be nullptr");
  RA8_CHECK_NULL_PTR(peer_y, s_tag, "peer_y must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  priv_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra8_rsip_off_asym_pub_x, peer_x, curve_bytes);
  internal_asym_push(k_ra8_rsip_off_asym_pub_y, peer_y, curve_bytes);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_ecdh_compute;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_ecdh_compute;

  const ra8_err_t err = priv_complete(k_ra8_rsip_mask_isr_asym_done);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_ecdh_pull_shared(out);
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_OFF_TARGET */

/*
 * Fail-closed production variant. With no real RSIP asymmetric backend on this
 * silicon, every ECDSA / ECDH / Ed25519 entry point returns a hard error
 * (never k_ra8_ok) so a production image cannot mistake the fake
 * command-path for a valid signature or shared secret. Callers use
 * tf-psa-crypto (ECDSA-P256 / ECDH / PSA_ALG_PURE_EDDSA) on the M85 instead.
 */

ra8_err_t ra8_rsip_ecdsa_sign(const ra8_rsip_key_handle_t* key,
                              ra8_rsip_curve_t             curve,
                              const uint8_t*               digest,
                              uint32_t                     digest_len,
                              uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "ecdsa_sign: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "ecdsa_sign: signature must not be nullptr");
  (void)curve;
  (void)digest;
  (void)digest_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_ecdsa_verify(const ra8_rsip_key_handle_t* key,
                                ra8_rsip_curve_t             curve,
                                const uint8_t*               digest,
                                uint32_t                     digest_len,
                                const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "ecdsa_verify: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "ecdsa_verify: signature must not be nullptr");
  (void)curve;
  (void)digest;
  (void)digest_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_eddsa_sign(const ra8_rsip_key_handle_t* key,
                              const uint8_t*               msg,
                              uint32_t                     msg_len,
                              uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "eddsa_sign: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "eddsa_sign: signature must not be nullptr");
  (void)msg;
  (void)msg_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_eddsa_verify(const ra8_rsip_key_handle_t* key,
                                const uint8_t*               msg,
                                uint32_t                     msg_len,
                                const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "eddsa_verify: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "eddsa_verify: signature must not be nullptr");
  (void)msg;
  (void)msg_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_ecdh_compute(const ra8_rsip_key_handle_t* key,
                                ra8_rsip_curve_t             curve,
                                const uint8_t*               peer_x,
                                const uint8_t*               peer_y,
                                ra8_rsip_key_handle_t*       out)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "ecdh_compute: key must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "ecdh_compute: out must not be nullptr");
  (void)curve;
  (void)peer_x;
  (void)peer_y;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_OFF_TARGET */
