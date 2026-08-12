/**
 * @file ra8_rsip_keys.h
 * @brief Renesas Secure IP (RSIP-E50D) HAL -- key install + cipher / hash API
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Key-touching surface of the RA8D2 RSIP-E50D HAL split out of the
 * ``ra8_rsip.h`` umbrella. This sub-header owns the opaque wrapped-key
 * handle type, the plaintext + OEM key-install flows, the AES block /
 * authenticated cipher modes, ChaCha20 + Poly1305, the generic
 * SHA-2 / SHA-3 / SHAKE hash family + HMAC, and the asymmetric
 * RSA / ECDSA / ECDH primitives.
 *
 * Every key-touching API takes opaque ``ra8_rsip_key_handle_t`` blobs --
 * raw key bytes never leave secure RAM. The mailbox-driven peripheral
 * is documented in HUM Ch 52 "Renesas Secure IP (RSIP-E50D)"
 * p 3302-3307; cross-references to the broader security feature set
 * live in HUM Ch 51 "Security Features" p 3263-3301.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Wrapped-key handle type (opaque to callers)
 * =============================================================================
 */

/**
 * @struct ra8_rsip_key_handle_t
 * @brief Opaque wrapped-key handle.
 *
 * @details
 * The HAL never exposes raw key bytes; every key the engine touches
 * lives behind a wrapped blob. The handle carries the algorithm
 * selector + the body word count so the engine can route the body
 * through the correct command FIFO (FSP ``rsip_wrapped_key_t``).
 *
 * The blob storage is sized to the largest wrapped key the engine
 * accepts (RSA-4096 private = 260 body words + 1 alg word).
 *
 * @invariant ``alg`` is one of ``ra8_rsip_oem_cmd_t``.
 * @invariant ``body_words <= k_ra8_rsip_handle_words_rsa4096_priv``.
 *
 * @note Handles are populated by ``ra8_rsip_aes_*_install_*``,
 * ``ra8_rsip_rsa_*_install_*`` etc. and consumed by every
 * cipher / signing / KDF API.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t alg;                                        /**< OEM-cmd algorithm selector.    */
  uint32_t body_words;                                 /**< Number of body words (1..261). */
  uint32_t body[k_ra8_rsip_handle_words_rsa4096_priv]; /**< Wrapped body.                  */
} ra8_rsip_key_handle_t;

/* =============================================================================
 * Key install -- plaintext (development) flow
 * =============================================================================
 */

/**
 * @brief Wrap a 16-byte AES-128 key for use by the engine.
 *
 * @details
 * Streams the plaintext key through the OEM key-install primitive
 * (FSP ``R_RSIP_AES128_InitialKeyWrap`` p ``r_rsip_key_injection.c``)
 * and returns the wrapped handle. The plaintext bytes are pushed
 * directly into the engine input FIFO and never copied into a
 * static buffer.
 *
 * @param[in] key Plaintext AES-128 key (16 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 16-byte readable.
 * @pre ``out`` is non-NULL.
 * @post On success, ``out->alg == k_ra8_rsip_oem_cmd_aes128``.
 * @post On success, ``out->body_words == k_ra8_rsip_handle_words_aes128``.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @see ra8_rsip_aes192_install_plain
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes128_install_plain(const uint8_t*         key,
                                                      ra8_rsip_key_handle_t* out);

/**
 * @brief Wrap a 24-byte AES-192 key (see ``ra8_rsip_aes128_install_plain``).
 *
 * @param[in] key Plaintext AES-192 key (24 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 24-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra8_rsip_oem_cmd_aes192``.
 * @post ``out->body_words == k_ra8_rsip_handle_words_aes192``.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes192_install_plain(const uint8_t*         key,
                                                      ra8_rsip_key_handle_t* out);

/**
 * @brief Wrap a 32-byte AES-256 key (see ``ra8_rsip_aes128_install_plain``).
 *
 * @param[in] key Plaintext AES-256 key (32 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 32-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra8_rsip_oem_cmd_aes256``.
 * @post ``out->body_words == k_ra8_rsip_handle_words_aes256``.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes256_install_plain(const uint8_t*         key,
                                                      ra8_rsip_key_handle_t* out);

/**
 * @brief Wrap a 32-byte ChaCha20 key.
 *
 * @param[in] key Plaintext ChaCha20 key (32 bytes).
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr ``key`` or ``out`` was nullptr.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and 32-byte readable.
 * @pre ``out`` is non-NULL.
 * @post ``out->alg == k_ra8_rsip_oem_cmd_chacha20``.
 * @post ``out->body_words == k_ra8_rsip_handle_words_chacha20``.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_chacha20_install_plain(const uint8_t*         key,
                                                        ra8_rsip_key_handle_t* out);

/**
 * @brief Wrap an HMAC key for use by the HMAC engine.
 *
 * @details
 * The wrapped-key body size depends on the underlying SHA flavour;
 * the selector is derived from ``alg``. HUM Ch 52 documents no HMAC
 * key-wrap register map (issue #215); production is fail-closed.
 *
 * @param[in] alg One of ``k_ra8_rsip_oem_cmd_hmac_sha*``.
 * @param[in] key Plaintext HMAC key.
 * @param[in] key_len ``key`` length in bytes.
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg ``alg`` not in the HMAC range, or
 * ``key_len`` is zero.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``alg`` is a HMAC opcode.
 * @pre ``key`` is non-NULL and ``key_len`` bytes readable.
 * @post On success ``out->alg == alg``.
 * @post On success ``out->body_words`` matches the algo's handle size.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_hmac_install_plain(ra8_rsip_oem_cmd_t     alg,
                                                    const uint8_t*         key,
                                                    uint32_t               key_len,
                                                    ra8_rsip_key_handle_t* out);

/* =============================================================================
 * Key install -- OEM (provisioning) flow
 * =============================================================================
 */

/**
 * @brief Install an OEM-encrypted key blob into the wrapped vault.
 *
 * @details
 * Drives the OEM (PE5/PE6) install primitive used during factory
 * provisioning: the plaintext key has already been encrypted under
 * the OEM root key and is delivered as ``oem_blob``; the engine
 * unwraps it inside the secure boundary and returns a vault-wrapped
 * handle (FSP ``r_rsip_key_injection.c`` -- ``InitialKeyWrap`` family).
 *
 * @param[in] cmd OEM opcode (algorithm + key length selector).
 * @param[in] iv 16-byte install IV.
 * @param[in] oem_blob OEM-encrypted body.
 * @param[in] blob_len ``oem_blob`` length in bytes.
 * @param[out] out Wrapped handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Unknown ``cmd`` or ``blob_len`` zero.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Engine reported a verification fail.
 *
 * @pre ``cmd`` != ``k_ra8_rsip_oem_cmd_invalid``.
 * @pre ``iv``, ``oem_blob`` and ``out`` are non-NULL.
 * @post On success ``out->alg == cmd``.
 * @post On success ``out->body_words`` matches the algo's handle size.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_oem_install(ra8_rsip_oem_cmd_t     cmd,
                                             const uint8_t*         iv,
                                             const uint8_t*         oem_blob,
                                             uint32_t               blob_len,
                                             ra8_rsip_key_handle_t* out);

/* =============================================================================
 * AES symmetric cipher -- ECB / CBC / CTR / XTS / CMAC / GMAC
 * =============================================================================
 */

/**
 * @brief Encrypt or decrypt a buffer with AES in a non-AEAD mode.
 *
 * @details
 * Streams the input through ``DATA_IN0..3`` 16 bytes at a time and
 * pulls the result from ``DATA_OUT0..3``. The caller is responsible
 * for padding to a 16-byte boundary in modes that require it (ECB,
 * CBC, CMAC); CTR / XTS / GMAC accept partial trailing bytes.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] mode Block / authenticated mode selector.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv IV / counter / tweak (16 bytes); may be NULL
 * for ECB / CMAC.
 * @param[in] in Input buffer.
 * @param[out] out Output buffer (>= ``len`` bytes).
 * @param[in] len Number of input bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Buffer transformed.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any required pointer was NULL.
 * @retval k_ra8_err_invalid_arg ``mode`` is an AEAD mode, or
 * ``len`` is not a multiple of the
 * block size for ECB / CBC / CMAC.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key`` is non-NULL and refers to a wrapped AES handle.
 * @pre ``in`` and ``out`` are non-NULL.
 * @post On success, ``out[0..len-1]`` holds the transformed bytes.
 * @post Engine SYM_STATUS.DONE has been observed and acked.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @see ra8_rsip_aes_gcm
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes_cipher(const ra8_rsip_key_handle_t* key,
                                            ra8_rsip_aes_mode_t          mode,
                                            ra8_rsip_aes_dir_t           dir,
                                            const uint8_t*               iv,
                                            const uint8_t*               in,
                                            uint8_t*                     out,
                                            uint32_t                     len);

/* =============================================================================
 * AES authenticated modes -- GCM / CCM
 * =============================================================================
 */

/**
 * @brief AES-GCM encrypt or decrypt with associated data.
 *
 * @details
 * NIST SP 800-38D (HUM Ch 52.1 Table 52.1 "GCM" p 3302). On encrypt,
 * ``tag`` is filled with the 16-byte authenticator; on decrypt, the
 * tag passed in is compared and the routine returns
 * ``k_ra8_err_hw_error`` if the comparison fails.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte tag buffer (output on encrypt,
 * input on decrypt).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr ``key``, ``iv``, ``in``, ``out`` or
 * ``tag`` was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Decrypt-side tag comparison failed.
 *
 * @pre ``key->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the authenticator.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes_gcm(const ra8_rsip_key_handle_t* key,
                                         ra8_rsip_aes_dir_t           dir,
                                         const uint8_t*               iv,
                                         const uint8_t*               aad,
                                         uint32_t                     aad_len,
                                         const uint8_t*               in,
                                         uint8_t*                     out,
                                         uint32_t                     in_len,
                                         uint8_t*                     tag);

/**
 * @brief AES-CCM encrypt or decrypt with associated data.
 *
 * @details
 * NIST SP 800-38C. Same surface as ``ra8_rsip_aes_gcm`` -- the
 * engine handles the L / nonce concatenation internally; the caller
 * supplies a 12-byte nonce for compatibility.
 *
 * @param[in] key Wrapped AES key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] iv 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte tag buffer.
 *
 * @return ``ra8_err_t`` error code (same set as ``ra8_rsip_aes_gcm``).
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 *
 * @pre ``key->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the authenticator.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_aes_ccm(const ra8_rsip_key_handle_t* key,
                                         ra8_rsip_aes_dir_t           dir,
                                         const uint8_t*               iv,
                                         const uint8_t*               aad,
                                         uint32_t                     aad_len,
                                         const uint8_t*               in,
                                         uint8_t*                     out,
                                         uint32_t                     in_len,
                                         uint8_t*                     tag);

/* =============================================================================
 * ChaCha20 + Poly1305
 * =============================================================================
 */

/**
 * @brief ChaCha20 stream encrypt or decrypt (RFC 7539, no AEAD).
 *
 * @param[in] key Wrapped ChaCha20 key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] nonce 12-byte nonce.
 * @param[in] counter Initial 32-bit block counter.
 * @param[in] in Input buffer.
 * @param[out] out Output buffer (>= ``len`` bytes).
 * @param[in] len Input length in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg == k_ra8_rsip_oem_cmd_chacha20``.
 * @pre ``nonce`` is non-NULL.
 * @post On success, ``out[0..len-1]`` holds the transformed bytes.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_chacha20(const ra8_rsip_key_handle_t* key,
                                          ra8_rsip_aes_dir_t           dir,
                                          const uint8_t*               nonce,
                                          uint32_t                     counter,
                                          const uint8_t*               in,
                                          uint8_t*                     out,
                                          uint32_t                     len);

/**
 * @brief ChaCha20-Poly1305 AEAD encrypt or decrypt (RFC 7539).
 *
 * @param[in] key Wrapped ChaCha20 key handle.
 * @param[in] dir Encrypt / decrypt selector.
 * @param[in] nonce 12-byte nonce.
 * @param[in] aad Additional authenticated data; may be NULL.
 * @param[in] aad_len ``aad`` length in bytes.
 * @param[in] in Plaintext (encrypt) or ciphertext (decrypt).
 * @param[out] out Ciphertext (encrypt) or plaintext (decrypt).
 * @param[in] in_len Input length in bytes.
 * @param[in,out] tag 16-byte Poly1305 tag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any required pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Decrypt-side tag check failed.
 *
 * @pre ``key->alg == k_ra8_rsip_oem_cmd_chacha20``.
 * @pre ``nonce``, ``in``, ``out``, ``tag`` are non-NULL.
 * @post On encrypt success, ``tag[0..15]`` is the Poly1305 MAC.
 * @post On decrypt success, ``out[0..in_len-1]`` is plaintext.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_chacha20_poly1305(const ra8_rsip_key_handle_t* key,
                                                   ra8_rsip_aes_dir_t           dir,
                                                   const uint8_t*               nonce,
                                                   const uint8_t*               aad,
                                                   uint32_t                     aad_len,
                                                   const uint8_t*               in,
                                                   uint8_t*                     out,
                                                   uint32_t                     in_len,
                                                   uint8_t*                     tag);

/**
 * @brief Poly1305 MAC over a buffer using a 32-byte one-time key.
 *
 * @param[in] one_time_key 32-byte Poly1305 key (derived per message).
 * @param[in] msg Buffer to authenticate.
 * @param[in] msg_len Length of ``msg`` in bytes.
 * @param[out] tag 16-byte tag output.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Tag computed.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``one_time_key`` is non-NULL and 32-byte readable.
 * @pre ``tag`` is non-NULL.
 * @post On success, ``tag[0..15]`` is the Poly1305 MAC.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rsip_poly1305(const uint8_t* one_time_key, const uint8_t* msg, uint32_t msg_len, uint8_t* tag);

/* =============================================================================
 * Hash family -- generic SHA-2 / SHA-3 / SHAKE
 * =============================================================================
 */

/**
 * @brief Compute a hash of an in-memory buffer using the selected algorithm.
 *
 * @details
 * Generalisation of ``ra8_rsip_sha256`` to every algorithm in
 * ``ra8_rsip_hash_alg_t``. ``out_len`` must be at least the digest
 * size for the selected algorithm; for SHAKE-128/256 ``out_len`` is
 * the requested XOF length and may be any positive value.
 *
 * @param[in] alg Algorithm selector.
 * @param[in] msg Message to hash; may be NULL only if ``msg_len``
 * is zero.
 * @param[in] msg_len Message length in bytes.
 * @param[out] digest Output buffer.
 * @param[in] digest_len Output buffer length.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Digest written.
 * @retval k_ra8_err_null_ptr ``digest`` was NULL, or ``msg`` was
 * NULL with non-zero ``msg_len``.
 * @retval k_ra8_err_invalid_arg ``digest_len`` too small for ``alg``.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``alg`` is one of ``k_ra8_rsip_hash_*``.
 * @pre If ``msg_len`` > 0, ``msg`` is non-NULL.
 * @post On success, ``digest[0..N-1]`` is the digest where N is
 * the algorithm's natural output size (or ``digest_len`` for
 * SHAKE).
 * @post HASH_STATUS.DONE has been acked.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_rsip_sha256
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_hash(ra8_rsip_hash_alg_t alg,
                                      const uint8_t*      msg,
                                      uint32_t            msg_len,
                                      uint8_t*            digest,
                                      uint32_t            digest_len);

/**
 * @brief HMAC-SHA-2 / HMAC-SHA-3 over a buffer using a wrapped key.
 *
 * @param[in] key Wrapped HMAC key handle.
 * @param[in] msg Buffer to authenticate.
 * @param[in] msg_len Length of ``msg`` in bytes.
 * @param[out] mac Output MAC buffer (>= digest size of HMAC's
 * underlying hash).
 * @param[in] mac_len ``mac`` buffer length.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok MAC written.
 * @retval k_ra8_err_null_ptr Any required pointer was NULL.
 * @retval k_ra8_err_invalid_arg ``mac_len`` too small for the algo.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is a HMAC opcode.
 * @pre ``mac`` is non-NULL.
 * @post On success, ``mac[0..N-1]`` is the HMAC.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_hmac(const ra8_rsip_key_handle_t* key,
                                      const uint8_t*               msg,
                                      uint32_t                     msg_len,
                                      uint8_t*                     mac,
                                      uint32_t                     mac_len);

/* =============================================================================
 * Asymmetric crypto -- RSA + ECDSA + ECDH
 * =============================================================================
 */

/**
 * @brief RSA sign a digest with a wrapped private key.
 *
 * @param[in] key Wrapped RSA private-key handle.
 * @param[in] size RSA modulus selector (1024 / 2048 / 3072 / 4096).
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length in bytes.
 * @param[out] signature Output signature (modulus / 8 bytes).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature produced.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``size``.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is an RSA install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, ``signature[0..modulus_bytes-1]`` is the RSA sig.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_rsa_sign(const ra8_rsip_key_handle_t* key,
                                          ra8_rsip_rsa_size_t          size,
                                          const uint8_t*               digest,
                                          uint32_t                     digest_len,
                                          uint8_t*                     signature);

/**
 * @brief RSA verify a signature against a digest using a wrapped pubkey.
 *
 * @param[in] key Wrapped RSA public-key handle.
 * @param[in] size RSA modulus selector.
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length.
 * @param[in] signature Signature (modulus / 8 bytes).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature valid.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``size``.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Signature did not verify.
 *
 * @pre ``key->alg`` is an RSA install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, the signature has been validated by the engine.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_rsa_verify(const ra8_rsip_key_handle_t* key,
                                            ra8_rsip_rsa_size_t          size,
                                            const uint8_t*               digest,
                                            uint32_t                     digest_len,
                                            const uint8_t*               signature);

/**
 * @brief RSA public-key encrypt a short message (RSAES-OAEP / PKCS1).
 *
 * @details
 * Drives the RSIP asymmetric engine's public-encrypt opcode
 * (``k_ra8_rsip_asym_op_rsa_encrypt``) so RSA-OAEP key transport works:
 * the plaintext (typically a wrapped content-encryption key) is padded
 * per ``pad`` and raised to the public exponent, yielding a
 * modulus-width ciphertext. The engine performs the padding inside the
 * secure boundary; the caller supplies only the raw message bytes.
 *
 * @param[in] key Wrapped RSA public-key handle.
 * @param[in] size RSA modulus selector (1024 / 2048 / 3072 / 4096).
 * @param[in] pad Padding scheme (OAEP or PKCS1).
 * @param[in] plaintext Message to encrypt; never NULL.
 * @param[in] plaintext_len Message length in bytes; 1 .. modulus / 8.
 * @param[out] ciphertext Output ciphertext (modulus / 8 bytes); never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Ciphertext produced.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``size`` / ``pad``, or
 *                              ``plaintext_len`` out of range.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Message too long for the padding scheme.
 *
 * @pre ``key->alg`` is an RSA install opcode.
 * @pre ``plaintext`` and ``ciphertext`` are non-NULL.
 * @post On success, ``ciphertext[0..modulus_bytes-1]`` is the RSAES blob.
 * @post No engine key state persists beyond the call.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @see ra8_rsip_rsa_decrypt
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_rsa_encrypt(const ra8_rsip_key_handle_t* key,
                                             ra8_rsip_rsa_size_t          size,
                                             ra8_rsip_rsa_pad_t           pad,
                                             const uint8_t*               plaintext,
                                             uint32_t                     plaintext_len,
                                             uint8_t*                     ciphertext);

/**
 * @brief RSA private-key decrypt a ciphertext (RSAES-OAEP / PKCS1).
 *
 * @details
 * Drives the RSIP asymmetric engine's private-decrypt opcode
 * (``k_ra8_rsip_asym_op_rsa_decrypt``): the modulus-width ciphertext is
 * raised to the private exponent and the ``pad`` padding is removed
 * inside the secure boundary, leaving the recovered message. The
 * recovered length (shorter than the modulus for OAEP / PKCS1) is
 * reported through ``recovered_len``.
 *
 * @param[in] key Wrapped RSA private-key handle.
 * @param[in] size RSA modulus selector (1024 / 2048 / 3072 / 4096).
 * @param[in] pad Padding scheme (OAEP or PKCS1).
 * @param[in] ciphertext Ciphertext to decrypt (modulus / 8 bytes); never NULL.
 * @param[out] plaintext Recovered-message buffer; never NULL.
 * @param[in] plaintext_cap Capacity of ``plaintext`` in bytes.
 * @param[out] recovered_len Receives the recovered-message length; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Message recovered.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``size`` / ``pad``, or recovered
 *                              message exceeds ``plaintext_cap``.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Padding check failed (corrupt ciphertext).
 *
 * @pre ``key->alg`` is an RSA private install opcode.
 * @pre ``ciphertext``, ``plaintext`` and ``recovered_len`` are non-NULL.
 * @post On success, ``plaintext[0..*recovered_len-1]`` is the message.
 * @post On success, ``*recovered_len <= plaintext_cap``.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @see ra8_rsip_rsa_encrypt
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_rsa_decrypt(const ra8_rsip_key_handle_t* key,
                                             ra8_rsip_rsa_size_t          size,
                                             ra8_rsip_rsa_pad_t           pad,
                                             const uint8_t*               ciphertext,
                                             uint8_t*                     plaintext,
                                             uint32_t                     plaintext_cap,
                                             uint32_t*                    recovered_len);

/**
 * @brief ECDSA sign a digest with a wrapped private key.
 *
 * @param[in] key Wrapped ECC private-key handle.
 * @param[in] curve Curve selector.
 * @param[in] digest Pre-computed message digest.
 * @param[in] digest_len Digest length.
 * @param[out] signature Output (r || s); 64 bytes for P-256, 96 for
 * P-384, 132 for P-521.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature produced.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``curve``, or ``curve`` is
 *                              ``k_ra8_rsip_curve_ed25519`` (use
 *                              ``ra8_rsip_eddsa_sign`` -- Ed25519 is
 *                              PureEdDSA, not ECDSA).
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg`` is an ECC (Weierstrass-curve) install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, ``signature`` holds (r || s).
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @note Ed25519 is rejected here; route it through ``ra8_rsip_eddsa_sign``.
 * @see ra8_rsip_eddsa_sign
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_ecdsa_sign(const ra8_rsip_key_handle_t* key,
                                            ra8_rsip_curve_t             curve,
                                            const uint8_t*               digest,
                                            uint32_t                     digest_len,
                                            uint8_t*                     signature);

/**
 * @brief ECDSA verify a signature with a peer public key.
 *
 * @param[in] key Wrapped ECC public-key handle (or the peer's
 * raw uncompressed point staged through
 * ``ASYM_PUB_X``/``ASYM_PUB_Y``).
 * @param[in] curve Curve selector.
 * @param[in] digest Pre-computed digest.
 * @param[in] digest_len Digest length.
 * @param[in] signature Signature (r || s).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature valid.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``curve``, or ``curve`` is
 *                              ``k_ra8_rsip_curve_ed25519`` (use
 *                              ``ra8_rsip_eddsa_verify``).
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Signature did not verify.
 *
 * @pre ``key->alg`` is an ECC (Weierstrass-curve) install opcode.
 * @pre ``digest`` and ``signature`` are non-NULL.
 * @post On success, the engine has validated the signature.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @note Ed25519 is rejected here; route it through ``ra8_rsip_eddsa_verify``.
 * @see ra8_rsip_eddsa_verify
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_ecdsa_verify(const ra8_rsip_key_handle_t* key,
                                              ra8_rsip_curve_t             curve,
                                              const uint8_t*               digest,
                                              uint32_t                     digest_len,
                                              const uint8_t*               signature);

/**
 * @brief Ed25519 PureEdDSA sign a message (RFC 8032).
 *
 * @details
 * Ed25519 PureEdDSA (RFC 8032) is NOT backed by a documented RSIP
 * register interface on this silicon -- HUM Ch 52 "Renesas Secure IP
 * (RSIP-E50D)" is a feature overview, not a command-register map, and
 * the vendor engine is driven through an encrypted firmware mailbox. A
 * production build (neither ``RA8_INSECURE_STUB_CRYPTO`` nor
 * ``RA8_OFF_TARGET``) is therefore FAIL-CLOSED: this entry point
 * returns ``k_ra8_err_not_supported`` rather than hand back bytes that no
 * RFC 8032 verifier would accept. The real Ed25519 signer is
 * tf-psa-crypto (``PSA_ALG_PURE_EDDSA``) on the M85. Only the
 * insecure-stub / off-target build drives a placeholder EdDSA command
 * path (host command-path testing only); PureEdDSA signs the raw
 * message, NOT a pre-computed digest, and the 64-byte output is the
 * ``R || S`` encoding.
 *
 * @param[in] key Wrapped Ed25519 private-key handle
 *                (``k_ra8_rsip_oem_cmd_ecc_ed25519_priv``).
 * @param[in] msg Message to sign; may be NULL only if ``msg_len`` is 0.
 * @param[in] msg_len Message length in bytes.
 * @param[out] signature 64-byte output (R || S); never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature produced (stub / off-target build only).
 * @retval k_ra8_err_not_supported Production build -- Ed25519 has no RSIP
 *                                backend; use tf-psa-crypto instead.
 * @retval k_ra8_err_null_ptr ``key`` / ``signature`` was NULL, or
 *                           ``msg`` was NULL with non-zero ``msg_len``.
 * @retval k_ra8_err_invalid_arg ``key->alg`` is not the Ed25519 opcode.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``key->alg == k_ra8_rsip_oem_cmd_ecc_ed25519_priv``.
 * @pre ``signature`` is at least 64 bytes wide.
 * @post On success, ``signature[0..63]`` holds (R || S).
 * @post No engine key state persists beyond the call.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production; Ed25519 is provided by tf-psa-crypto.
 * @see ra8_rsip_eddsa_verify
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_eddsa_sign(const ra8_rsip_key_handle_t* key,
                                            const uint8_t*               msg,
                                            uint32_t                     msg_len,
                                            uint8_t*                     signature);

/**
 * @brief Ed25519 PureEdDSA verify a signature (RFC 8032).
 *
 * @details
 * The verify counterpart to ``ra8_rsip_eddsa_sign`` and subject to the
 * same constraint: the RSIP-E50D exposes no documented Ed25519 register
 * interface on this silicon, so a production build (neither
 * ``RA8_INSECURE_STUB_CRYPTO`` nor ``RA8_OFF_TARGET``) is FAIL-CLOSED
 * and returns ``k_ra8_err_not_supported``. Ed25519 verification is
 * provided by tf-psa-crypto (``PSA_ALG_PURE_EDDSA``) on the M85. Only
 * the insecure-stub / off-target build drives a placeholder EdDSA
 * command path: the raw message is presented and the 64-byte
 * ``signature`` is the ``R || S`` encoding.
 *
 * @param[in] key Wrapped Ed25519 public-key handle (tagged with the
 *                Ed25519 opcode).
 * @param[in] msg Message that was signed; may be NULL only if
 *                ``msg_len`` is 0.
 * @param[in] msg_len Message length in bytes.
 * @param[in] signature 64-byte signature (R || S); never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Signature valid (stub / off-target build only).
 * @retval k_ra8_err_not_supported Production build -- Ed25519 has no RSIP
 *                                backend; use tf-psa-crypto instead.
 * @retval k_ra8_err_null_ptr ``key`` / ``signature`` was NULL, or
 *                           ``msg`` was NULL with non-zero ``msg_len``.
 * @retval k_ra8_err_invalid_arg ``key->alg`` is not the Ed25519 opcode.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Signature did not verify.
 *
 * @pre ``key->alg == k_ra8_rsip_oem_cmd_ecc_ed25519_priv``.
 * @pre ``signature`` is at least 64 bytes wide.
 * @post On success, the engine has validated the signature.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production; Ed25519 is provided by tf-psa-crypto.
 * @see ra8_rsip_eddsa_sign
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_eddsa_verify(const ra8_rsip_key_handle_t* key,
                                              const uint8_t*               msg,
                                              uint32_t                     msg_len,
                                              const uint8_t*               signature);

/**
 * @brief ECDH shared-secret derivation.
 *
 * @details
 * The peer public key is supplied as the uncompressed (X || Y)
 * coordinate pair. The shared secret stays inside the wrapped vault
 * and ``out`` receives a wrapped handle suitable for ``ra8_rsip_kdf``.
 *
 * @param[in] key Wrapped ECC private-key handle (own).
 * @param[in] curve Curve selector.
 * @param[in] peer_x Peer X coordinate (curve byte length).
 * @param[in] peer_y Peer Y coordinate (curve byte length).
 * @param[out] out Wrapped shared-secret handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Shared secret derived.
 * @retval k_ra8_err_not_supported Production build fail-closed; no RSIP backend, use tf-psa-crypto.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Bad ``curve``.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Peer point off-curve.
 *
 * @pre ``key->alg`` is an ECC private install opcode.
 * @pre ``peer_x`` and ``peer_y`` are non-NULL.
 * @post On success ``out->alg`` matches the curve's HMAC opcode.
 *
 * @note Thread safety: not thread-safe.
 * @note Fail-closed in production (HUM Ch 52 documents no RSIP backend); the fake/stub command path never ships. Real crypto: tf-psa-crypto (issues #214 / #187 / #181).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_ecdh_compute(const ra8_rsip_key_handle_t* key,
                                              ra8_rsip_curve_t             curve,
                                              const uint8_t*               peer_x,
                                              const uint8_t*               peer_y,
                                              ra8_rsip_key_handle_t*       out);

#ifdef __cplusplus
}
#endif
