/**
 * @file ra_rsip_asym.c
 * @brief RSIP-E50D asymmetric + key-management + tamper path
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Asymmetric-cryptography and key-management slice of the RA8D2
 * RSIP-E50D HAL driver, split out of ``ra_rsip.c`` to keep every
 * translation unit under the file-size budget. Covers HUM Ch 51
 * (Security Features p 3263-3301) + Ch 52 (RSIP-E50D mailbox
 * p 3302-3307) for:
 *
 * - the generic hash family (SHA-2 / SHA-3 / SHAKE) + HMAC entry points;
 * - asymmetric RSA + ECDSA sign / verify and ECDH key agreement;
 * - the OEM boot-loader anti-rollback version counter;
 * - the wrapped-key vault (read / write / erase / count);
 * - the key wrap / unwrap engine (KEK-backed);
 * - key derivation (HKDF + HUK / UID bound);
 * - device lifecycle + debug-authorisation level transitions;
 * - the tamper subsystem (per-source enable / status / ack + SPA / DPA arm);
 * - DOTF key delivery routing.
 *
 * Cross-TU primitives shared with ``ra_rsip.c`` and ``ra_rsip_cipher.c``
 * are declared in ``ra_rsip_internal.h``. The engine itself is opaque
 * (HUM Ch 52, p 3302-3307); sequences here are derived from the FSP
 * RSIP primitive layer but no FSP code is included verbatim.
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

/* ===========================================================================
 * Round-3 entry points: hash + HMAC
 * ===========================================================================
 */

/* Map a hash algorithm selector to its natural digest length -- see surrounding code and HUM citations. */
static uint32_t internal_hash_size(ra_rsip_hash_alg_t alg)
{
  switch (alg) {
    case k_ra_rsip_hash_sha224:
    case k_ra_rsip_hash_sha512_224:
    case k_ra_rsip_hash_sha3_224:
      return (uint32_t)k_ra_rsip_sha224_digest_bytes;
    case k_ra_rsip_hash_sha256:
    case k_ra_rsip_hash_sha512_256:
    case k_ra_rsip_hash_sha3_256:
      return (uint32_t)k_ra_rsip_sha256_digest_bytes;
    case k_ra_rsip_hash_sha384:
    case k_ra_rsip_hash_sha3_384:
      return (uint32_t)k_ra_rsip_sha384_digest_bytes;
    case k_ra_rsip_hash_sha512:
    case k_ra_rsip_hash_sha3_512:
      return (uint32_t)k_ra_rsip_sha512_digest_bytes;
    case k_ra_rsip_hash_shake128:
    case k_ra_rsip_hash_shake256:
      /* Variable-length output: caller supplies. */
      return 1U;
    default:
      return 0U;
  }
}

/* Validate the hash + digest length arguments before any MMIO -- see surrounding code and HUM citations. */
static ra_err_t internal_hash_validate(ra_rsip_hash_alg_t alg,
                                       const uint8_t*     msg,
                                       uint32_t           msg_len,
                                       uint32_t           digest_len,
                                       uint32_t*          needed)
{
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  const uint32_t n = internal_hash_size(alg);
  if (n == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((alg != k_ra_rsip_hash_shake128) && (alg != k_ra_rsip_hash_shake256) && (digest_len < n)) {
    return k_ra_err_invalid_arg;
  }
  *needed = n;
  return k_ra_ok;
}

/* Read a variable-length digest from the HASH_DIGEST window -- see surrounding code and HUM citations. */
static void internal_hash_pull_digest(uint8_t* digest, uint32_t to_read)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  uint32_t i   = 0U;
  uint32_t off = (uint32_t)k_ra_rsip_off_hash_digest;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= to_read) {
    /* Computed digest-word offset is a valid HUM-defined register location, not
     * a literal enumerator -- the analyzer can't see that. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    internal_unpack_le(word, &digest[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    off += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < to_read) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    for (uint32_t b = 0U; (i + b) < to_read; ++b) {
      digest[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
  *ra_rsip_reg32(k_ra_rsip_off_hash_status) &= ~k_ra_rsip_mask_isr_done;
}

ra_err_t ra_rsip_hash(ra_rsip_hash_alg_t alg,
                      const uint8_t*     msg,
                      uint32_t           msg_len,
                      uint8_t*           digest,
                      uint32_t           digest_len)
{
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  uint32_t       needed = 0U;
  const ra_err_t v_err  = internal_hash_validate(alg, msg, msg_len, digest_len, &needed);
  RA_RETURN_ON_ERROR(v_err, s_tag, "rsip_hash: validate"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_hash_ctrl) = (uint32_t)alg;

  if (msg_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_hash_data_in, msg, msg_len);
  }

  /* Pre-arm + wait for DONE on host sim. */
  const ra_err_t wait_err = internal_hash_wait_done();
  RA_RETURN_ON_ERROR(wait_err, s_tag, "rsip_hash: hash done"); /* GCOVR_EXCL_BR_LINE */

  /* Read digest_len for SHAKE; algo-natural otherwise. */
  const uint32_t to_read =
    ((alg == k_ra_rsip_hash_shake128) || (alg == k_ra_rsip_hash_shake256)) ? digest_len : needed;
  internal_hash_pull_digest(digest, to_read);
  return k_ra_ok;
}

ra_err_t ra_rsip_hmac(const ra_rsip_key_handle_t* key,
                      const uint8_t*              msg,
                      uint32_t                    msg_len,
                      uint8_t*                    mac,
                      uint32_t                    mac_len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(mac, s_tag, "mac must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  /* Determine the underlying hash size from the install opcode. */
  uint32_t needed = 0U;
  switch (key->alg) {
    case k_ra_rsip_oem_cmd_hmac_sha224:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
      needed = (uint32_t)k_ra_rsip_sha224_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha256:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      needed = (uint32_t)k_ra_rsip_sha256_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha384:
      needed = (uint32_t)k_ra_rsip_sha384_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha512:
      needed = (uint32_t)k_ra_rsip_sha512_digest_bytes;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if (mac_len < needed) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stage HMAC key handle, then drive the hash unit in HMAC mode. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_hmac) = key->alg;
  internal_load_handle(key);
  return ra_rsip_hash(k_ra_rsip_hash_sha256, msg, msg_len, mac, needed);
}

/**
 * @brief Zero-fill the unused tail of a key-handle body buffer.
 *
 * @details
 * Several engine paths return a wrapped body shorter than the
 * maximum body capacity. To avoid leaking stale stack contents into
 * the structure, callers always pad ``body[words .. max-1]`` with
 * zeros. Centralised here.
 *
 * @param[in,out] handle Handle whose ``body[]`` tail is wiped.
 * @param[in]     words  Number of words already populated.
 *
 * @pre ``handle`` is non-NULL.
 * @pre ``words`` <= ``k_ra_rsip_handle_words_rsa4096_priv``.
 *
 * @post ``handle->body[w] == 0`` for all ``w`` in [``words``, max).
 * @post No other field is modified.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_zero_handle_tail(ra_rsip_key_handle_t* handle, uint32_t words)
{
  for (uint32_t w = words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    handle->body[w] = 0U;
  }
}

/* ===========================================================================
 * Round-3 entry points: asymmetric (RSA + ECDSA + ECDH)
 * ===========================================================================
 */

/* Push a buffer through an asymmetric input lane -- see surrounding code and HUM citations. */
static void internal_asym_push(ra_rsip_off_t off, const uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    *ra_rsip_reg32(off) = internal_pack_le(&buf[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)buf[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    *ra_rsip_reg32(off) = tail;
  }
}

/* Pull a buffer back through an asymmetric output lane -- see surrounding code and HUM citations. */
static void internal_asym_pull(ra_rsip_off_t off, uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    internal_unpack_le(*ra_rsip_reg32(off), &buf[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    const uint32_t word = *ra_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      buf[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
}

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

/**
 * @brief Map a curve to its scalar / coordinate byte length.
 *
 * @param[in] curve Curve selector.
 *
 * @return Byte length, or 0 for unknown.
 *
 * @pre ``curve`` is one of ``ra_rsip_curve_t``.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result == FIPS / RFC parameter byte length.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
/**
 * @enum ra_rsip_curve_bytes_t
 * @brief Per-curve scalar / coordinate byte lengths (FIPS 186-4 / RFC 7748).
 */
typedef enum : uint32_t {
  k_ra_rsip_curve_bytes_192 = 24U, /**< 192-bit curves: secp192r1.             */
  k_ra_rsip_curve_bytes_224 = 28U, /**< 224-bit curves: secp224r1.             */
  k_ra_rsip_curve_bytes_256 = 32U, /**< 256-bit curves: secp256*, ed25519.     */
  k_ra_rsip_curve_bytes_384 = 48U, /**< 384-bit curves: secp384r1, brain384r1. */
  k_ra_rsip_curve_bytes_512 = 64U, /**< 512-bit curves: brain512r1.            */
  k_ra_rsip_curve_bytes_521 = 66U, /**< 521-bit curves: secp521r1.             */
} ra_rsip_curve_bytes_t;

/* internal curve bytes -- see surrounding code and HUM citations. */
static uint32_t internal_curve_bytes(ra_rsip_curve_t curve)
{
  switch (curve) {
    case k_ra_rsip_curve_secp192r1:
      return k_ra_rsip_curve_bytes_192;
    case k_ra_rsip_curve_secp224r1:
      return k_ra_rsip_curve_bytes_224;
    case k_ra_rsip_curve_secp256r1:
    case k_ra_rsip_curve_brain256r1:
    case k_ra_rsip_curve_ed25519:
    case k_ra_rsip_curve_secp256k1:
      return k_ra_rsip_curve_bytes_256;
    case k_ra_rsip_curve_secp384r1:
    case k_ra_rsip_curve_brain384r1:
      return k_ra_rsip_curve_bytes_384;
    case k_ra_rsip_curve_brain512r1:
      return k_ra_rsip_curve_bytes_512;
    case k_ra_rsip_curve_secp521r1:
      return k_ra_rsip_curve_bytes_521;
    default:
      return 0U;
  }
}

ra_err_t ra_rsip_ecdsa_sign(const ra_rsip_key_handle_t* key,
                            ra_rsip_curve_t             curve,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if (curve == k_ra_rsip_curve_ed25519) {
    /* Ed25519 is PureEdDSA (RFC 8032), not ECDSA -- routing it through
     * the ECDSA opcode would not produce a valid signature. Callers
     * must use ra_rsip_eddsa_sign(). */
    return k_ra_err_invalid_arg;
  }
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdsa_sign;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* (r || s) */
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, curve_bytes * 2U);
  return k_ra_ok;
}

ra_err_t ra_rsip_ecdsa_verify(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              digest,
                              uint32_t                    digest_len,
                              const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if (curve == k_ra_rsip_curve_ed25519) {
    /* Ed25519 is PureEdDSA (RFC 8032), not ECDSA -- callers must use
     * ra_rsip_eddsa_verify(). */
    return k_ra_err_invalid_arg;
  }
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, curve_bytes * 2U);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdsa_verify;

  return internal_complete(k_ra_rsip_mask_isr_asym_done);
}

/**
 * @enum ra_rsip_ed25519_size_t
 * @brief Ed25519 PureEdDSA byte sizes (RFC 8032 Section 5.1).
 */
typedef enum : uint32_t {
  k_ra_rsip_ed25519_comp_bytes = 32U, /**< R or S component length.   */
  k_ra_rsip_ed25519_sig_bytes  = 64U, /**< Signature (R || S) length. */
} ra_rsip_ed25519_size_t;

ra_err_t ra_rsip_eddsa_sign(const ra_rsip_key_handle_t* key,
                            const uint8_t*              msg,
                            uint32_t                    msg_len,
                            uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (key->alg != (uint32_t)k_ra_rsip_oem_cmd_ecc_ed25519_priv) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)k_ra_rsip_curve_ed25519;
  /* PureEdDSA signs the message itself, not a pre-computed digest (RFC 8032). */
  if (msg_len > 0U) {
    internal_asym_push(k_ra_rsip_off_asym_msg_in, msg, msg_len);
  }
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_eddsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_eddsa_sign;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* (R || S) */
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, (uint32_t)k_ra_rsip_ed25519_sig_bytes);
  return k_ra_ok;
}

ra_err_t ra_rsip_eddsa_verify(const ra_rsip_key_handle_t* key,
                              const uint8_t*              msg,
                              uint32_t                    msg_len,
                              const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (key->alg != (uint32_t)k_ra_rsip_oem_cmd_ecc_ed25519_priv) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)k_ra_rsip_curve_ed25519;
  if (msg_len > 0U) {
    internal_asym_push(k_ra_rsip_off_asym_msg_in, msg, msg_len);
  }
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, (uint32_t)k_ra_rsip_ed25519_sig_bytes);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_eddsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_eddsa_verify;

  return internal_complete(k_ra_rsip_mask_isr_asym_done);
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
 * @pre ``internal_complete`` has just returned ``k_ra_ok``.
 *
 * @post ``out->alg`` and ``out->body_words`` reflect HMAC-SHA-256.
 * @post ``out->body[]`` has been fully populated and tail-zeroed.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_ecdh_pull_shared(ra_rsip_key_handle_t* out)
{
  /* The wrapped shared secret is delivered as an HMAC-SHA-256 handle. */
  out->alg        = k_ra_rsip_oem_cmd_hmac_sha256;
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_asym_shared);
  }
  internal_zero_handle_tail(out, out->body_words);
}

ra_err_t ra_rsip_ecdh_compute(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              peer_x,
                              const uint8_t*              peer_y,
                              ra_rsip_key_handle_t*       out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(peer_x, s_tag, "peer_x must not be nullptr");
  RA_CHECK_NULL_PTR(peer_y, s_tag, "peer_y must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_pub_x, peer_x, curve_bytes);
  internal_asym_push(k_ra_rsip_off_asym_pub_y, peer_y, curve_bytes);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdh_compute;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdh_compute;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_ecdh_pull_shared(out);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: OEM boot loader version (anti-rollback)
 * ===========================================================================
 */

ra_err_t ra_rsip_oem_bl_version_get(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver);
  return k_ra_ok;
}

ra_err_t ra_rsip_oem_bl_version_increment(void)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  if (*ra_rsip_reg32(k_ra_rsip_off_oem_bl_lock) != 0U) {
    return k_ra_err_invalid_state;
  }
  /* W1 trigger; engine increments the latched counter. */
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_inc) = 1U;
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver) = *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver) + 1U;
  return k_ra_ok;
}

ra_err_t ra_rsip_oem_bl_version_lock(void)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_lock) = 1U;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: wrapped-key vault
 * ===========================================================================
 */

/* Issue a vault command and wait for completion -- see surrounding code and HUM citations. */
static ra_err_t internal_kv_op(ra_rsip_kv_op_t op, uint8_t slot)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kv_slot) = slot;
  *ra_rsip_reg32(k_ra_rsip_off_kv_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;
  return internal_complete(k_ra_rsip_mask_isr_kv_done);
}

#ifdef RA_SIMULATOR_MODE
/* The host-test simulator backs MMIO with plain memory, so successive
 * writes to the kv_data FIFO would just overwrite the same word. Keep
 * a per-slot shadow so read-after-write tests can verify the round
 * trip without modelling the FIFO inside ra_sim_mmap. */
static uint8_t s_sim_kv_slots[(uint32_t)k_ra_rsip_kv_slot_count]
                             [k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes];
#endif

ra_err_t ra_rsip_kv_read(uint8_t slot, uint8_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t err = internal_kv_op(k_ra_rsip_kv_op_read, slot);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kv_data);
    internal_unpack_le(word, &out[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
#ifdef RA_SIMULATOR_MODE
  /* Replay from the per-slot shadow so the test sees the bytes that
   * were actually written rather than whatever the FIFO MMIO settled
   * on after 16 overlapping writes. */
  for (uint32_t i = 0U; i < k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes; ++i) {
    out[i] = s_sim_kv_slots[slot][i];
  }
#endif
  return k_ra_ok;
}

ra_err_t ra_rsip_kv_write(uint8_t slot, const uint8_t* in)
{
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kv_data) =
      internal_pack_le(&in[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
#ifdef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes; ++i) {
    s_sim_kv_slots[slot][i] = in[i];
  }
#endif
  return internal_kv_op(k_ra_rsip_kv_op_write, slot);
}

ra_err_t ra_rsip_kv_erase(uint8_t slot)
{
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  return internal_kv_op(k_ra_rsip_kv_op_erase, slot);
}

ra_err_t ra_rsip_kv_count(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_kv_count);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: key wrap / unwrap engine
 * ===========================================================================
 */

/**
 * @brief Stage the wrap engine's KEK selector, body, and IV.
 *
 * @details
 * Both wrap and unwrap start by publishing the KEK algorithm to
 * ``KW_KEK``, streaming the KEK body into the staging port, and
 * loading the 16-byte IV into ``KW_IV0..3``. Centralised here.
 *
 * @param[in] kek KEK handle.
 * @param[in] iv  16-byte IV.
 *
 * @pre ``kek`` and ``iv`` are non-NULL.
 *
 * @post ``KW_KEK`` carries ``kek->alg``.
 * @post ``KEY_STAGE`` has observed ``kek->body_words`` writes.
 * @post ``KW_IV0..3`` reflect ``iv``.
 *
 * @note Internal helper.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 */
static void internal_kw_stage_kek(const ra_rsip_key_handle_t* kek, const uint8_t* iv)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kw_kek) = kek->alg;
  internal_push_handle_body(kek);
  internal_push_iv_lanes(k_ra_rsip_off_kw_iv0, iv);
}

/* Stream the wrap-engine output blob (16 words) into a byte buffer -- see surrounding code and HUM citations. */
static void internal_kw_pull_blob(uint8_t* blob)
{
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kw_blob_out);
    internal_unpack_le(word, &blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Push the source-handle body into the wrap-engine input FIFO -- see surrounding code and HUM citations. */
static void internal_kw_push_src(const ra_rsip_key_handle_t* src)
{
  *ra_rsip_reg32(k_ra_rsip_off_kw_handle) = src->alg;
  for (uint32_t w = 0U; w < src->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) = src->body[w];
  }
}

ra_err_t ra_rsip_key_wrap(const ra_rsip_key_handle_t* kek,
                          const uint8_t*              iv,
                          const ra_rsip_key_handle_t* src,
                          uint8_t*                    blob)
{
  RA_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  internal_kw_push_src(src);
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = k_ra_rsip_kw_op_wrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_kw_op_wrap;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_kw_pull_blob(blob);
  return k_ra_ok;
}

/* Pull the unwrapped algorithm + body into a destination handle -- see surrounding code and HUM citations. */
static ra_err_t internal_kw_pull_handle(ra_rsip_key_handle_t* dest)
{
  /* Pull the unwrapped algorithm + body out. */
  dest->alg            = *ra_rsip_reg32(k_ra_rsip_off_kw_handle);
  const uint32_t words = internal_handle_words_for((ra_rsip_oem_cmd_t)dest->alg);
  if (words == 0U) {
    return k_ra_err_hw_error;
  }
  dest->body_words = words;
  for (uint32_t w = 0U; w < words; ++w) {
    dest->body[w] = *ra_rsip_reg32(k_ra_rsip_off_kw_blob_out);
  }
  internal_zero_handle_tail(dest, words);
  return k_ra_ok;
}

ra_err_t ra_rsip_key_unwrap(const ra_rsip_key_handle_t* kek,
                            const uint8_t*              iv,
                            const uint8_t*              blob,
                            ra_rsip_key_handle_t*       dest)
{
  RA_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  RA_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) =
      internal_pack_le(&blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = k_ra_rsip_kw_op_unwrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_kw_op_unwrap;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_kw_pull_handle(dest);
}

/* ===========================================================================
 * Round-3 entry points: key derivation
 * ===========================================================================
 */

/* Validate the KDF arguments before any MMIO is touched -- see surrounding code and HUM citations. */
static ra_err_t internal_kdf_validate(ra_rsip_kdf_op_t            op,
                                      const ra_rsip_key_handle_t* ikm,
                                      const uint8_t*              label,
                                      uint32_t                    label_len,
                                      const uint8_t*              salt,
                                      uint32_t                    salt_len,
                                      uint32_t                    out_len)
{
  if ((label == nullptr) && (label_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((salt == nullptr) && (salt_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (out_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HKDF modes need an IKM handle; HUK / UID modes do not. */
  if (((op == k_ra_rsip_kdf_op_hkdf_sha256) || (op == k_ra_rsip_kdf_op_hkdf_sha384) ||
       (op == k_ra_rsip_kdf_op_hkdf_sha512)) &&
      (ikm == nullptr)) {
    return k_ra_err_null_ptr;
  }
  return k_ra_ok;
}

/* Stage the KDF inputs (op + length + optional IKM + label + salt) -- see surrounding code and HUM citations. */
static void internal_kdf_stage(ra_rsip_kdf_op_t            op,
                               const ra_rsip_key_handle_t* ikm,
                               const uint8_t*              label,
                               uint32_t                    label_len,
                               const uint8_t*              salt,
                               uint32_t                    salt_len,
                               uint32_t                    out_len)
{
  /* HUM Ch 52.1 "KDF" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kdf_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_kdf_len)  = out_len;
  if (ikm != nullptr) {
    *ra_rsip_reg32(k_ra_rsip_off_kdf_ikm) = ikm->alg;
    internal_push_handle_body(ikm);
  }
  if (label_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_kdf_label, label, label_len);
  }
  if (salt_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_kdf_salt, salt, salt_len);
  }
}

/* Pull the wrapped derived-key handle out of the KDF engine -- see surrounding code and HUM citations. */
static void internal_kdf_pull_handle(ra_rsip_key_handle_t* out)
{
  /* Wrapped derived key delivered through KDF_OUT. */
  out->alg        = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  }
  internal_zero_handle_tail(out, out->body_words);
}

ra_err_t ra_rsip_kdf(ra_rsip_kdf_op_t            op,
                     const ra_rsip_key_handle_t* ikm,
                     const uint8_t*              label,
                     uint32_t                    label_len,
                     const uint8_t*              salt,
                     uint32_t                    salt_len,
                     uint32_t                    out_len,
                     ra_rsip_key_handle_t*       out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra_err_t v_err = internal_kdf_validate(op, ikm, label, label_len, salt, salt_len, out_len);
  RA_RETURN_ON_ERROR(v_err, s_tag, "rsip_kdf: validate"); /* GCOVR_EXCL_BR_LINE */

  internal_kdf_stage(op, ikm, label, label_len, salt, salt_len, out_len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_kdf_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_kdf_pull_handle(out);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: device lifecycle + debug authorisation
 * ===========================================================================
 */

ra_err_t ra_rsip_life_get(ra_rsip_life_state_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Device lifecycle" p 3263 */
  *out = (ra_rsip_life_state_t)*ra_rsip_reg32(k_ra_rsip_off_life_state);
  return k_ra_ok;
}

ra_err_t ra_rsip_life_advance(ra_rsip_life_state_t state)
{
  if ((uint32_t)state > k_ra_rsip_life_rma) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.1 "Device lifecycle" p 3263 */
  const uint32_t cur = *ra_rsip_reg32(k_ra_rsip_off_life_state);
  if ((uint32_t)state < cur) {
    return k_ra_err_invalid_state;
  }
  *ra_rsip_reg32(k_ra_rsip_off_life_state) = (uint32_t)state;
  return k_ra_ok;
}

ra_err_t ra_rsip_debug_level_get(ra_rsip_debug_level_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Three debug levels" p 3263 */
  *out = (ra_rsip_debug_level_t)*ra_rsip_reg32(k_ra_rsip_off_debug_level);
  return k_ra_ok;
}

ra_err_t ra_rsip_debug_level_set(ra_rsip_debug_level_t level)
{
  if ((uint32_t)level > (uint32_t)k_ra_rsip_debug_al2) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.1 "Three debug levels" p 3263 */
  *ra_rsip_reg32(k_ra_rsip_off_debug_level) = (uint32_t)level;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: tamper subsystem
 * ===========================================================================
 */

ra_err_t ra_rsip_tamper_enable(uint32_t sources)
{
  if ((sources & ~k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_ctrl) = sources;
  return k_ra_ok;
}

ra_err_t ra_rsip_tamper_status(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_tamper_status);
  return k_ra_ok;
}

ra_err_t ra_rsip_tamper_ack(uint32_t mask)
{
  if (mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((mask & ~k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_status) = mask;
  return k_ra_ok;
}

ra_err_t ra_rsip_dpa_arm(bool enable)
{
  /* HUM Ch 51.5 "Side-channel countermeasures" p 3290 */
  volatile uint32_t* ctrl = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  if (enable) {
    *ctrl |= k_ra_rsip_mask_ctrl_dpa_arm;
  } else {
    *ctrl &= ~k_ra_rsip_mask_ctrl_dpa_arm;
  }
  *ra_rsip_reg32(k_ra_rsip_off_dpa_ctrl) = (uint32_t)enable;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: DOTF key delivery routing
 * ===========================================================================
 */

ra_err_t ra_rsip_dotf_route(uint8_t which, uint8_t slot, bool on)
{
  if (which > 1U) {
    return k_ra_err_invalid_arg;
  }
  if (on && (slot >= (uint8_t)k_ra_rsip_kv_slot_count)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  const ra_rsip_off_t off = (which == 0U) ? k_ra_rsip_off_dotf0_ctrl : k_ra_rsip_off_dotf1_ctrl;
  /* DOTFn_CTRL = (slot << 16) | route_enable */
  uint32_t word = k_ra_rsip_dotf_off;
  if (on) {
    word = ((uint32_t)slot << k_ra_rsip_byte_shift_2) | k_ra_rsip_dotf_on;
  }
  *ra_rsip_reg32(off) = word;
  return k_ra_ok;
}
