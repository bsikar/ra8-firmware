/**
 * @file ra_rsip_rsa.c
 * @brief RSIP-E50D RSA sign / verify / encrypt / decrypt path
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * RSA slice of the RA8D2 RSIP-E50D asymmetric HAL driver, split out of
 * ``ra_rsip_asym.c`` to keep every translation unit under the file-size
 * budget. Covers HUM Ch 52.2.4 "Asymmetric cipher" (p 3306) for:
 *
 * - RSASSA sign / verify over a pre-computed digest;
 * - RSAES encrypt / decrypt with OAEP / PKCS1 padding.
 *
 * The byte-lane streaming primitives ``internal_asym_push`` /
 * ``internal_asym_pull`` are defined in ``ra_rsip_asym.c`` and shared
 * with the ECDSA / ECDH / EdDSA code there; their declarations live in
 * ``ra_rsip_asym_internal.h``. The remaining cross-TU primitives
 * (``internal_load_handle``, ``internal_complete``) are declared in
 * ``ra_rsip_internal.h``. The engine itself is opaque (HUM Ch 52,
 * p 3302-3307); sequences here are derived from the FSP RSIP primitive
 * layer but no FSP code is included verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_rsip_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_rsip.h"
#include "ra_rsip_asym_internal.h"
#include "ra_rsip_internal.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix without
 * truncation. Each RSIP translation unit keeps its own private copy.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP";

ra_err_t ra_rsip_rsa_sign(const ra_rsip_key_handle_t* key,
                          ra_rsip_rsa_size_t          size,
                          const uint8_t*              digest,
                          uint32_t                    digest_len,
                          uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_rsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_rsa_sign;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t sig_len = (uint32_t)size / k_ra_rsip_byte_bits;
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, sig_len);
  return k_ra_ok;
}

ra_err_t ra_rsip_rsa_verify(const ra_rsip_key_handle_t* key,
                            ra_rsip_rsa_size_t          size,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  const uint32_t sig_len = (uint32_t)size / k_ra_rsip_byte_bits;
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, sig_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_rsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_rsa_verify;

  return internal_complete(k_ra_rsip_mask_isr_asym_done);
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
 * @pre ``size`` is a ``ra_rsip_rsa_size_t`` value.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result == modulus bit length / 8 for supported sizes.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static uint32_t internal_rsa_modulus_bytes(ra_rsip_rsa_size_t size)
{
  switch (size) {
    case k_ra_rsip_rsa_1024:
    case k_ra_rsip_rsa_2048:
    case k_ra_rsip_rsa_3072:
    case k_ra_rsip_rsa_4096:
      return (uint32_t)size / (uint32_t)k_ra_rsip_byte_bits;
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
 * @pre ``pad`` is a ``ra_rsip_rsa_pad_t`` value.
 * @pre Caller rejects the operation on ``false``.
 *
 * @post No state modified.
 * @post Result reflects membership in ``ra_rsip_rsa_pad_t``.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static bool internal_rsa_pad_ok(ra_rsip_rsa_pad_t pad)
{
  switch (pad) {
    case k_ra_rsip_rsa_pad_pkcs1:
    case k_ra_rsip_rsa_pad_oaep:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Validate the RSA modulus + padding selectors for encrypt / decrypt.
 *
 * @details
 * Shared front-half of ``ra_rsip_rsa_encrypt`` / ``ra_rsip_rsa_decrypt``.
 * Both single-condition checks (``modulus == 0`` and ``!pad_ok``) live
 * here so the public entry points stay under the NASA Rule-4 statement
 * cap; neither check is a compound boolean decision.
 *
 * @param[in]  size              RSA modulus selector.
 * @param[in]  pad               RSAES padding selector.
 * @param[out] modulus_bytes_out Receives the modulus byte width on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              ``size`` and ``pad`` are supported.
 * @retval k_ra_err_invalid_arg ``size`` or ``pad`` is unsupported.
 *
 * @pre ``modulus_bytes_out`` is non-NULL.
 * @pre Caller has already null-checked its public-API pointers.
 *
 * @post On ``k_ra_ok``, ``*modulus_bytes_out`` is the modulus width.
 * @post On error, ``*modulus_bytes_out`` is left unmodified.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static ra_err_t internal_rsa_size_pad_check(ra_rsip_rsa_size_t size,
                                            ra_rsip_rsa_pad_t  pad,
                                            uint32_t*          modulus_bytes_out)
{
  const uint32_t modulus_bytes = internal_rsa_modulus_bytes(size);
  if (modulus_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_rsa_pad_ok(pad)) {
    return k_ra_err_invalid_arg;
  }
  *modulus_bytes_out = modulus_bytes;
  return k_ra_ok;
}

/**
 * @brief Stage the RSA descriptor + input lane, issue the opcode, and wait.
 *
 * @details
 * Shared back-half of ``ra_rsip_rsa_encrypt`` / ``ra_rsip_rsa_decrypt``:
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
 * @param[in] op     ``k_ra_rsip_asym_op_rsa_encrypt`` or ``..._rsa_decrypt``.
 *
 * @return ``ra_err_t`` error code from the mailbox completion.
 * @retval k_ra_ok             Engine completed without error.
 * @retval k_ra_err_hw_timeout Completion bit never observed.
 * @retval k_ra_err_hw_error   Engine reported a fault.
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
static ra_err_t internal_rsa_dispatch(const ra_rsip_key_handle_t* key,
                                      ra_rsip_rsa_size_t          size,
                                      ra_rsip_rsa_pad_t           pad,
                                      const uint8_t*              in,
                                      uint32_t                    in_len,
                                      ra_rsip_asym_op_t           op)
{
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  *ra_rsip_reg32(k_ra_rsip_off_asym_arg)      = (uint32_t)pad;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, in, in_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)op;
  return internal_complete(k_ra_rsip_mask_isr_asym_done);
}

ra_err_t ra_rsip_rsa_encrypt(const ra_rsip_key_handle_t* key,
                             ra_rsip_rsa_size_t          size,
                             ra_rsip_rsa_pad_t           pad,
                             const uint8_t*              plaintext,
                             uint32_t                    plaintext_len,
                             uint8_t*                    ciphertext)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "plaintext must not be nullptr");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "ciphertext must not be nullptr");
  uint32_t       modulus_bytes = 0U;
  const ra_err_t v_err         = internal_rsa_size_pad_check(size, pad, &modulus_bytes);
  if (v_err != k_ra_ok) {
    return v_err;
  }
  if (plaintext_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (plaintext_len > modulus_bytes) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t err =
    internal_rsa_dispatch(key, size, pad, plaintext, plaintext_len, k_ra_rsip_asym_op_rsa_encrypt);
  if (err != k_ra_ok) {
    return err;
  }
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, ciphertext, modulus_bytes);
  return k_ra_ok;
}

ra_err_t ra_rsip_rsa_decrypt(const ra_rsip_key_handle_t* key,
                             ra_rsip_rsa_size_t          size,
                             ra_rsip_rsa_pad_t           pad,
                             const uint8_t*              ciphertext,
                             uint8_t*                    plaintext,
                             uint32_t                    plaintext_cap,
                             uint32_t*                   recovered_len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "ciphertext must not be nullptr");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "plaintext must not be nullptr");
  RA_CHECK_NULL_PTR(recovered_len, s_tag, "recovered_len must not be nullptr");
  uint32_t       modulus_bytes = 0U;
  const ra_err_t v_err         = internal_rsa_size_pad_check(size, pad, &modulus_bytes);
  if (v_err != k_ra_ok) {
    return v_err;
  }
  const ra_err_t err =
    internal_rsa_dispatch(key, size, pad, ciphertext, modulus_bytes, k_ra_rsip_asym_op_rsa_decrypt);
  if (err != k_ra_ok) {
    return err;
  }
  /* The engine writes the unpadded message length back into ASYM_ARG. */
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  const uint32_t recovered = *ra_rsip_reg32(k_ra_rsip_off_asym_arg);
  if (recovered > plaintext_cap) {
    return k_ra_err_invalid_arg;
  }
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, plaintext, recovered);
  *recovered_len = recovered;
  return k_ra_ok;
}
