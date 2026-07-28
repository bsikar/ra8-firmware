/**
 * @file ra8_rsip_rsa.c
 * @brief RSIP-E50D RSA sign / verify / encrypt / decrypt path
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * RSA slice of the RA8D2 RSIP-E50D asymmetric HAL driver, split out of
 * ``ra8_rsip_asym.c`` to keep every translation unit under the file-size
 * budget. Exposes:
 *
 * - RSASSA sign / verify over a pre-computed digest;
 * - RSAES encrypt / decrypt with OAEP / PKCS1 padding.
 *
 * All four entry points are FAIL-CLOSED in production: HUM Ch 52 documents no
 * asymmetric command-register map for the RSIP-E50D, so the off-target-only command
 * path is gated behind the stub-crypto guard and a production build returns
 * ``k_ra8_err_not_supported`` (issues #214 + #187).
 *
 * The byte-lane streaming primitives ``internal_asym_push`` /
 * ``internal_asym_pull`` are defined in ``ra8_rsip_asym.c`` and shared
 * with the ECDSA / ECDH / EdDSA code there; their declarations live in
 * ``ra8_rsip_asym_internal.h``. The remaining cross-TU primitives
 * (``internal_load_handle``, ``internal_complete``) are declared in
 * ``ra8_rsip_internal.h``. The RSIP engine exposes no documented asymmetric
 * register interface (HUM Ch 52 is a feature overview, p 3302-3307), so the
 * fake command path here is a modelled fiction, not a real hardware sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
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
 * RSIP-E50D RSA (RSASSA sign / verify, RSAES-OAEP / PKCS1 encrypt / decrypt) is
 * NOT backed by a documented register interface on this silicon. HUM Ch 52
 * "Renesas Secure IP (RSIP-E50D)" is a six-page feature overview (p 3302-3307)
 * with no asymmetric command-register map; the vendor engine is driven through
 * an encrypted firmware mailbox, not the MMIO opcodes modelled below. The
 * command-path bodies here only round-trip the host register fake; they do
 * NOT compute a real RSASP1 / RFC 8017 result. They compile only under the
 * insecure-stub / off-target guard so a production image gets the fail-closed
 * #else and can never mistake these bytes for a valid RSA signature or
 * ciphertext. No plain-key RSA backend ships on this part; RSA (if ever needed)
 * is provided by tf-psa-crypto on the M85 (issues #214 + #187). The register
 * pokes below therefore carry NO HUM citation: there is no real register map to
 * cite.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)

ra8_err_t ra8_rsip_rsa_sign(const ra8_rsip_key_handle_t* key,
                            ra8_rsip_rsa_size_t          size,
                            const uint8_t*               digest,
                            uint32_t                     digest_len,
                            uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra8_rsip_rsa_1024) && (size != k_ra8_rsip_rsa_2048) &&
      (size != k_ra8_rsip_rsa_3072) && (size != k_ra8_rsip_rsa_4096)) {
    return k_ra8_err_invalid_arg;
  }
  internal_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra8_rsip_off_asym_msg_in, digest, digest_len);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_rsa_sign;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_rsa_sign;

  const ra8_err_t err = internal_complete(k_ra8_rsip_mask_isr_asym_done);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t sig_len = (uint32_t)size / k_ra8_rsip_byte_bits;
  internal_asym_pull(k_ra8_rsip_off_asym_sig_out, signature, sig_len);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_rsa_verify(const ra8_rsip_key_handle_t* key,
                              ra8_rsip_rsa_size_t          size,
                              const uint8_t*               digest,
                              uint32_t                     digest_len,
                              const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra8_rsip_rsa_1024) && (size != k_ra8_rsip_rsa_2048) &&
      (size != k_ra8_rsip_rsa_3072) && (size != k_ra8_rsip_rsa_4096)) {
    return k_ra8_err_invalid_arg;
  }
  internal_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra8_rsip_off_asym_msg_in, digest, digest_len);
  const uint32_t sig_len = (uint32_t)size / k_ra8_rsip_byte_bits;
  internal_asym_push(k_ra8_rsip_off_asym_sig_in, signature, sig_len);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = k_ra8_rsip_asym_op_rsa_verify;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = k_ra8_rsip_asym_op_rsa_verify;

  return internal_complete(k_ra8_rsip_mask_isr_asym_done);
}

/**
 * @brief Map an RSA modulus selector to its byte width.
 *
 * @details
 * Switch-based lookup (no compound boolean decision) so the size
 * validation reduces to a single ``== 0`` test at the call site. The
 * modulus byte width is also the ciphertext length for RSAES.
 *
 * @param[in] size RSA modulus selector.
 *
 * @return Modulus width in bytes, or 0 for an unsupported selector.
 * @retval 0 ``size`` is not a supported RSA modulus.
 *
 * @pre ``size`` is a ``ra8_rsip_rsa_size_t`` value.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result == modulus bit length / 8 for supported sizes.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_rsa_modulus_bytes(ra8_rsip_rsa_size_t size)
{
  switch (size) {
    case k_ra8_rsip_rsa_1024:
    case k_ra8_rsip_rsa_2048:
    case k_ra8_rsip_rsa_3072:
    case k_ra8_rsip_rsa_4096:
      return (uint32_t)size / (uint32_t)k_ra8_rsip_byte_bits;
    default:
      return 0U;
  }
}

/**
 * @brief Validate an RSAES padding selector.
 *
 * @details
 * Switch-based check (no compound boolean decision) so the encrypt /
 * decrypt entry points stay free of multi-condition validation.
 *
 * @param[in] pad Padding-scheme selector.
 *
 * @return ``true`` if ``pad`` is a supported RSAES scheme.
 * @retval true  ``pad`` is OAEP or PKCS1.
 * @retval false ``pad`` is unsupported.
 *
 * @pre ``pad`` is a ``ra8_rsip_rsa_pad_t`` value.
 * @pre Caller rejects the operation on ``false``.
 *
 * @post No state modified.
 * @post Result reflects membership in ``ra8_rsip_rsa_pad_t``.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_rsa_pad_ok(ra8_rsip_rsa_pad_t pad)
{
  switch (pad) {
    case k_ra8_rsip_rsa_pad_pkcs1:
    case k_ra8_rsip_rsa_pad_oaep:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Validate the RSA modulus + padding selectors for encrypt / decrypt.
 *
 * @details
 * Shared front-half of ``ra8_rsip_rsa_encrypt`` / ``ra8_rsip_rsa_decrypt``.
 * Both single-condition checks (``modulus == 0`` and ``!pad_ok``) live
 * here so the public entry points stay under the NASA Rule-4 statement
 * cap; neither check is a compound boolean decision.
 *
 * @param[in]  size              RSA modulus selector.
 * @param[in]  pad               RSAES padding selector.
 * @param[out] modulus_bytes_out Receives the modulus byte width on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ``size`` and ``pad`` are supported.
 * @retval k_ra8_err_invalid_arg ``size`` or ``pad`` is unsupported.
 *
 * @pre ``modulus_bytes_out`` is non-NULL.
 * @pre Caller has already null-checked its public-API pointers.
 *
 * @post On ``k_ra8_ok``, ``*modulus_bytes_out`` is the modulus width.
 * @post On error, ``*modulus_bytes_out`` is left unmodified.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rsa_size_pad_check(ra8_rsip_rsa_size_t size,
                                             ra8_rsip_rsa_pad_t  pad,
                                             uint32_t*           modulus_bytes_out)
{
  const uint32_t modulus_bytes = internal_rsa_modulus_bytes(size);
  if (modulus_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_rsa_pad_ok(pad)) {
    return k_ra8_err_invalid_arg;
  }
  *modulus_bytes_out = modulus_bytes;
  return k_ra8_ok;
}

/**
 * @brief Stage the RSA descriptor + input lane, issue the opcode, and wait.
 *
 * @details
 * Shared back-half of ``ra8_rsip_rsa_encrypt`` / ``ra8_rsip_rsa_decrypt``:
 * loads the wrapped key, publishes the modulus-size + padding-scheme
 * descriptor registers, streams the input block through ``ASYM_MSG_IN``,
 * writes ``op`` to both ``ASYM_CTRL`` and ``MBOX_OP``, then drives the
 * mailbox completion. The exact opcode dispatch the host tests assert on
 * is preserved verbatim -- ``op`` is written unmodified to both lanes.
 *
 * @param[in] key    Wrapped RSA key handle.
 * @param[in] size   RSA modulus selector (already validated).
 * @param[in] pad    RSAES padding selector (already validated).
 * @param[in] in     Input block (plaintext on encrypt, ciphertext on decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in] op     ``k_ra8_rsip_asym_op_rsa_encrypt`` or ``..._rsa_decrypt``.
 *
 * @return ``ra8_err_t`` error code from the mailbox completion.
 * @retval k_ra8_ok             Engine completed without error.
 * @retval k_ra8_err_hw_timeout Completion bit never observed.
 * @retval k_ra8_err_hw_error   Engine reported a fault.
 *
 * @pre ``key`` and ``in`` are non-NULL.
 * @pre ``size`` / ``pad`` passed ``internal_rsa_size_pad_check``.
 *
 * @post ``ASYM_CTRL`` and ``MBOX_OP`` carry ``op``.
 * @post ``ASYM_RSA_SIZE`` carries ``size`` and ``ASYM_ARG`` carries ``pad``.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rsa_dispatch(const ra8_rsip_key_handle_t* key,
                                       ra8_rsip_rsa_size_t          size,
                                       ra8_rsip_rsa_pad_t           pad,
                                       const uint8_t*               in,
                                       uint32_t                     in_len,
                                       ra8_rsip_asym_op_t           op)
{
  internal_load_handle(key);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_rsa_size) = (uint32_t)size;
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_arg)      = (uint32_t)pad;
  internal_asym_push(k_ra8_rsip_off_asym_msg_in, in, in_len);
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = (uint32_t)op;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op)   = (uint32_t)op;
  return internal_complete(k_ra8_rsip_mask_isr_asym_done);
}

ra8_err_t ra8_rsip_rsa_encrypt(const ra8_rsip_key_handle_t* key,
                               ra8_rsip_rsa_size_t          size,
                               ra8_rsip_rsa_pad_t           pad,
                               const uint8_t*               plaintext,
                               uint32_t                     plaintext_len,
                               uint8_t*                     ciphertext)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(plaintext, s_tag, "plaintext must not be nullptr");
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "ciphertext must not be nullptr");
  uint32_t        modulus_bytes = 0U;
  const ra8_err_t v_err         = internal_rsa_size_pad_check(size, pad, &modulus_bytes);
  if (v_err != k_ra8_ok) {
    return v_err;
  }
  if (plaintext_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (plaintext_len > modulus_bytes) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err =
    internal_rsa_dispatch(key, size, pad, plaintext, plaintext_len, k_ra8_rsip_asym_op_rsa_encrypt);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_asym_pull(k_ra8_rsip_off_asym_sig_out, ciphertext, modulus_bytes);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_rsa_decrypt(const ra8_rsip_key_handle_t* key,
                               ra8_rsip_rsa_size_t          size,
                               ra8_rsip_rsa_pad_t           pad,
                               const uint8_t*               ciphertext,
                               uint8_t*                     plaintext,
                               uint32_t                     plaintext_cap,
                               uint32_t*                    recovered_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "ciphertext must not be nullptr");
  RA8_CHECK_NULL_PTR(plaintext, s_tag, "plaintext must not be nullptr");
  RA8_CHECK_NULL_PTR(recovered_len, s_tag, "recovered_len must not be nullptr");
  uint32_t        modulus_bytes = 0U;
  const ra8_err_t v_err         = internal_rsa_size_pad_check(size, pad, &modulus_bytes);
  if (v_err != k_ra8_ok) {
    return v_err;
  }
  const ra8_err_t err = internal_rsa_dispatch(key,
                                              size,
                                              pad,
                                              ciphertext,
                                              modulus_bytes,
                                              k_ra8_rsip_asym_op_rsa_decrypt);
  if (err != k_ra8_ok) {
    return err;
  }
  /* The engine writes the unpadded message length back into ASYM_ARG. */
  const uint32_t recovered = *ra8_rsip_reg32(k_ra8_rsip_off_asym_arg);
  if (recovered > plaintext_cap) {
    return k_ra8_err_invalid_arg;
  }
  internal_asym_pull(k_ra8_rsip_off_asym_sig_out, plaintext, recovered);
  *recovered_len = recovered;
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_OFF_TARGET */

/*
 * Fail-closed production variant. With no real RSIP RSA backend on this
 * silicon, every RSASSA / RSAES entry point returns a hard error (never
 * k_ra8_ok) so a production image cannot mistake the fake command-path for
 * a valid RSA signature or ciphertext. Callers use tf-psa-crypto on the M85.
 */

ra8_err_t ra8_rsip_rsa_sign(const ra8_rsip_key_handle_t* key,
                            ra8_rsip_rsa_size_t          size,
                            const uint8_t*               digest,
                            uint32_t                     digest_len,
                            uint8_t*                     signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "rsa_sign: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "rsa_sign: signature must not be nullptr");
  (void)size;
  (void)digest;
  (void)digest_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_rsa_verify(const ra8_rsip_key_handle_t* key,
                              ra8_rsip_rsa_size_t          size,
                              const uint8_t*               digest,
                              uint32_t                     digest_len,
                              const uint8_t*               signature)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "rsa_verify: key must not be nullptr");
  RA8_CHECK_NULL_PTR(signature, s_tag, "rsa_verify: signature must not be nullptr");
  (void)size;
  (void)digest;
  (void)digest_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_rsa_encrypt(const ra8_rsip_key_handle_t* key,
                               ra8_rsip_rsa_size_t          size,
                               ra8_rsip_rsa_pad_t           pad,
                               const uint8_t*               plaintext,
                               uint32_t                     plaintext_len,
                               uint8_t*                     ciphertext)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "rsa_encrypt: key must not be nullptr");
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "rsa_encrypt: ciphertext must not be nullptr");
  (void)size;
  (void)pad;
  (void)plaintext;
  (void)plaintext_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_rsa_decrypt(const ra8_rsip_key_handle_t* key,
                               ra8_rsip_rsa_size_t          size,
                               ra8_rsip_rsa_pad_t           pad,
                               const uint8_t*               ciphertext,
                               uint8_t*                     plaintext,
                               uint32_t                     plaintext_cap,
                               uint32_t*                    recovered_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "rsa_decrypt: key must not be nullptr");
  RA8_CHECK_NULL_PTR(plaintext, s_tag, "rsa_decrypt: plaintext must not be nullptr");
  RA8_CHECK_NULL_PTR(recovered_len, s_tag, "rsa_decrypt: recovered_len must not be nullptr");
  (void)size;
  (void)pad;
  (void)ciphertext;
  (void)plaintext_cap;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_OFF_TARGET */
