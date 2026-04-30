/**
 * @file ra_sce_protected.h
 * @brief SCE protected-key HAL -- crypto operations driven by wrapped
 *        key blobs (the SCE engine never sees the raw key)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Mirrors the FSP ``r_sce_protected`` shape: every entry point takes
 * a wrapped-key buffer (produced by ``ra_sce_key_inject_*``) instead
 * of a raw key. The wrapper carries the key-management envelope and
 * an integrity MAC so the protected-op path can refuse forged or
 * corrupted blobs before they reach the engine.
 *
 * Internally the stub validates the blob via
 * ``ra_sce_key_validate``, unwraps the payload into a private
 * scratch buffer and forwards to the unprotected ``ra_sce_*`` entry
 * point, then scrubs the scratch buffer. A future drop-in real
 * backend (sanctioned FSP integration) replaces the unwrap step
 * with a hardware key-handle dispatch.
 *
 * @warning Stub backend; NOT cryptographically secure. The wrapper
 *          is symmetric (no real key isolation). Do not ship
 *          key-bearing firmware against this header until a real
 *          backend is wired up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_sce.h"
#include "ra_sce_key_injection.h"

/**
 * @brief Initialise an AES context using a wrapped key blob.
 *
 * @param[in] wrapped_key Wrapped-key buffer produced by
 *                        ``ra_sce_key_inject_aes``.
 * @param[in] key_bits    Width of the wrapped key.
 * @param[in] mode        Block-cipher mode.
 * @param[in] iv          16-byte IV (NULL only for ECB).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context initialised.
 * @retval k_ra_err_null_ptr ``wrapped_key`` was NULL.
 * @retval k_ra_err_invalid_arg Wrapper type tag mismatched, mode or
 *                              key-bits unsupported.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_error Wrapper integrity MAC failed.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Wrapper was produced by ``ra_sce_key_inject_aes``.
 * @post On success, AES context is latched.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_sce_aes_init
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_protected_aes_init(const uint8_t*        wrapped_key,
                                                 ra_sce_aes_key_bits_t key_bits,
                                                 ra_sce_aes_mode_t     mode,
                                                 const uint8_t*        iv);

/**
 * @brief Encrypt with the protected AES context.
 *
 * @param[in]  plaintext  Source bytes.
 * @param[out] ciphertext Destination bytes.
 * @param[in]  len        Number of bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer transformed.
 * @retval k_ra_err_invalid_state Protected AES context not latched.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 *
 * @pre ``ra_sce_protected_aes_init`` returned ``k_ra_ok``.
 * @post On success, ``ciphertext`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_protected_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len);

/**
 * @brief Decrypt with the protected AES context.
 *
 * @param[in]  ciphertext Source bytes.
 * @param[out] plaintext  Destination bytes.
 * @param[in]  len        Number of bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Buffer transformed.
 * @retval k_ra_err_invalid_state Protected AES context not latched.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 *
 * @pre ``ra_sce_protected_aes_init`` returned ``k_ra_ok``.
 * @post On success, ``plaintext`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sce_protected_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len);

/**
 * @brief Finalise the protected AES context and (GCM only) emit the tag.
 *
 * @param[out] tag_out 16-byte tag buffer (GCM only); may be NULL.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Context cleared.
 * @retval k_ra_err_invalid_state ``_aes_init`` had not run.
 *
 * @pre ``ra_sce_protected_aes_init`` returned ``k_ra_ok``.
 * @post Cached AES context is wiped.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_protected_aes_finish(uint8_t* tag_out);

/**
 * @brief RSA-private decrypt using a wrapped private key.
 *
 * @param[in]      wrapped_priv      Wrapped-key buffer.
 * @param[in]      key_bits          RSA modulus width.
 * @param[in]      ciphertext        Big-endian ciphertext.
 * @param[in]      ciphertext_len    Length of ``ciphertext``.
 * @param[out]     plaintext_out     Destination.
 * @param[in,out]  plaintext_len_out Capacity in / used out.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Plaintext written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Wrapper malformed; lengths unsupported.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_error Wrapper integrity MAC failed.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Wrapper was produced by ``ra_sce_key_inject_rsa``.
 * @post On success, ``plaintext_out`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_protected_rsa_decrypt(const uint8_t*        wrapped_priv,
                                                    ra_sce_rsa_key_bits_t key_bits,
                                                    const uint8_t*        ciphertext,
                                                    uint32_t              ciphertext_len,
                                                    uint8_t*              plaintext_out,
                                                    uint32_t*             plaintext_len_out);

/**
 * @brief ECDSA sign using a wrapped private key.
 *
 * @param[in]  wrapped_priv Wrapped-key buffer.
 * @param[in]  curve        Curve identifier.
 * @param[in]  hash         Message digest.
 * @param[in]  hash_len     Length of ``hash``.
 * @param[out] sig_out      Signature destination.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Signature written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Wrapper malformed; curve unsupported.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 * @retval k_ra_err_hw_error Wrapper integrity MAC failed.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Wrapper was produced by ``ra_sce_key_inject_ecc(..., true)``.
 * @post On success, ``sig_out`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_protected_ecdsa_sign(const uint8_t*     wrapped_priv,
                                                   ra_sce_ecc_curve_t curve,
                                                   const uint8_t*     hash,
                                                   uint32_t           hash_len,
                                                   uint8_t*           sig_out);

#ifdef __cplusplus
}
#endif
