/**
 * @file ra8_rsip_protected.h
 * @brief RSIP protected-key HAL -- crypto operations driven by wrapped
 * @ingroup grp_hal_crypto
 *        key blobs (the RSIP engine never sees the raw key)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Mirrors the FSP ``r_rsip_protected`` shape: every entry point takes
 * a wrapped-key buffer (produced by ``ra8_rsip_key_inject_*``) instead
 * of a raw key. The wrapper carries the key-injection envelope and an
 * integrity MAC so the protected-op path can refuse forged or
 * corrupted blobs before they reach the engine.
 *
 * Internally the stub validates the blob via
 * ``ra8_rsip_key_validate``, unwraps the payload into a private
 * scratch buffer, drives the underlying ``ra8_rsip_*_install_plain``
 * call to obtain a wrapped ``ra8_rsip_key_handle_t``, dispatches the
 * crypto op, and scrubs the scratch buffer. A future drop-in real
 * backend (sanctioned FSP integration) replaces the unwrap step with
 * a hardware key-handle dispatch.
 *
 * @warning Stub backend; NOT cryptographically secure. The wrapper
 *          is symmetric (no real key isolation). Do not ship
 *          key-bearing firmware against this header until a real
 *          backend is wired up.
 *
 * @see ra8_rsip.h  Underlying RSIP HAL surface.
 * @see ra8_rsip_key_injection.h  Producer of wrapped-key blobs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip.h"
#include "ra8_rsip_key_injection.h"
#include "ra8_rsip_regs.h"

/**
 * @brief Initialise an AES context using a wrapped key blob.
 *
 * @details
 * Validates the wrapper, materialises the underlying
 * ``ra8_rsip_key_handle_t`` via the install-plain path, and latches it
 * along with the requested mode and IV. Subsequent calls to
 * ``ra8_rsip_protected_aes_encrypt`` / ``..._decrypt`` use the latched
 * state without re-presenting the raw key.
 *
 * @param[in] wrapped_key Wrapped-key buffer produced by
 *                        ``ra8_rsip_key_inject_aes``.
 * @param[in] key_bits    Width of the wrapped key.
 * @param[in] mode        Block-cipher mode (ECB / CBC / CTR).
 * @param[in] iv          16-byte IV (NULL only for ECB).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Context initialized.
 * @retval k_ra8_err_null_ptr ``wrapped_key`` was NULL.
 * @retval k_ra8_err_invalid_arg Wrapper type tag mismatched, mode or
 *                              key-bits unsupported.
 * @retval k_ra8_err_hw_error Wrapper integrity MAC failed.
 * @retval k_ra8_err_hw_timeout RSIP install primitive timed out.
 *
 * @pre Wrapper was produced by ``ra8_rsip_key_inject_aes``.
 * @pre ``ra8_rsip_init`` returned ``k_ra8_ok`` on the live engine
 *      (or ``ra8_fake_mmap`` is wired up under test).
 * @post On success, AES context is latched.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_aes_cipher
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_protected_aes_init(const uint8_t*          wrapped_key,
                                                    ra8_rsip_aes_key_bits_t key_bits,
                                                    ra8_rsip_aes_mode_t     mode,
                                                    const uint8_t*          iv);

/**
 * @brief Encrypt with the latched protected AES context.
 *
 * @param[in]  plaintext  Source bytes.
 * @param[out] ciphertext Destination bytes.
 * @param[in]  len        Number of bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Buffer transformed.
 * @retval k_ra8_err_invalid_state Protected AES context not latched.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``ra8_rsip_protected_aes_init`` returned ``k_ra8_ok``.
 * @post On success, ``ciphertext`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_protected_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len);

/**
 * @brief Decrypt with the latched protected AES context.
 *
 * @param[in]  ciphertext Source bytes.
 * @param[out] plaintext  Destination bytes.
 * @param[in]  len        Number of bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Buffer transformed.
 * @retval k_ra8_err_invalid_state Protected AES context not latched.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``ra8_rsip_protected_aes_init`` returned ``k_ra8_ok``.
 * @post On success, ``plaintext`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_protected_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len);

/**
 * @brief Finalise the protected AES context.
 *
 * @details
 * Scrubs the latched key handle and IV. Must be called after the last
 * encrypt / decrypt and before another ``ra8_rsip_protected_aes_init``
 * cycle so a stale handle cannot be reused.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Context cleared.
 * @retval k_ra8_err_invalid_state ``..._aes_init`` had not run.
 *
 * @pre ``ra8_rsip_protected_aes_init`` returned ``k_ra8_ok``.
 * @post Cached AES context is wiped.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_protected_aes_finish(void);

/**
 * @brief RSA-private decrypt using a wrapped private key.
 *
 * @details
 * The stub treats RSA-decrypt as RSA-sign with a swapped semantic:
 * the wrapper is presented to ``ra8_rsip_rsa_sign`` so the engine
 * exercises the same modular-exponentiation path that a real
 * "private op" would follow. The output is the engine's raw
 * exponentiation result -- callers wrap their own padding scheme
 * (PKCS#1 v1.5 / OAEP) on top.
 *
 * @param[in]  wrapped_priv      Wrapped-key buffer.
 * @param[in]  size              RSA modulus width.
 * @param[in]  ciphertext        Big-endian ciphertext (modulus / 8 bytes).
 * @param[in]  ciphertext_len    Length of ``ciphertext``.
 * @param[out] plaintext_out     Destination (modulus / 8 bytes).
 * @param[in]  plaintext_cap     Capacity of ``plaintext_out``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Plaintext written.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Wrapper malformed; lengths unsupported.
 * @retval k_ra8_err_hw_error Wrapper integrity MAC failed.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre Wrapper was produced by ``ra8_rsip_key_inject_rsa``.
 * @pre ``ra8_rsip_init`` returned ``k_ra8_ok`` on the live engine.
 * @post On success, ``plaintext_out`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_protected_rsa_decrypt(const uint8_t*      wrapped_priv,
                                                       ra8_rsip_rsa_size_t size,
                                                       const uint8_t*      ciphertext,
                                                       uint32_t            ciphertext_len,
                                                       uint8_t*            plaintext_out,
                                                       uint32_t            plaintext_cap);

/**
 * @brief ECDSA sign using a wrapped private key.
 *
 * @param[in]  wrapped_priv Wrapped-key buffer.
 * @param[in]  curve        Curve identifier.
 * @param[in]  hash         Message digest.
 * @param[in]  hash_len     Length of ``hash``.
 * @param[out] sig_out      Signature destination (``r || s``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature written.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Wrapper malformed; curve unsupported.
 * @retval k_ra8_err_hw_error Wrapper integrity MAC failed.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre Wrapper was produced by ``ra8_rsip_key_inject_ecc(..., true)``.
 * @pre ``ra8_rsip_init`` returned ``k_ra8_ok`` on the live engine.
 * @post On success, ``sig_out`` is populated.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_protected_ecdsa_sign(const uint8_t*   wrapped_priv,
                                                      ra8_rsip_curve_t curve,
                                                      const uint8_t*   hash,
                                                      uint32_t         hash_len,
                                                      uint8_t*         sig_out);

#ifdef __cplusplus
}
#endif
