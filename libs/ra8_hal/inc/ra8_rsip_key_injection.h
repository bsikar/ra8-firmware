/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_key_injection.h
 * @brief RSIP key-injection HAL -- wrap raw key material into the
 * @ingroup grp_hal_crypto
 *        RSIP installed-key blob format
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The Renesas FSP ``r_rsip_key_injection`` driver bundles a raw key
 * (AES, RSA modulus + exponent, ECC private or public scalar) with a
 * key-injection envelope so the RSIP-E50D engine can accept key
 * material without ever exposing the raw bytes to host software
 * again. This HAL exposes the wrapping operation only -- the wrapped
 * blob is then consumed by ``ra8_rsip_protected_*`` calls.
 *
 * The implementation in ``ra8_rsip_key_injection.c`` is a host-friendly
 * software stub that lays out the wrapped buffer in the same shape
 * the FSP layer would (``[type | mgmt-info | key-bytes | footer-mac]``)
 * but uses a deterministic xorshift64* mixer to produce the embedded
 * MAC. This mirrors the pattern used by ``ra8_sce_key_injection`` so
 * the protected-op pipeline can round-trip in unit tests. A future
 * drop-in real backend (sanctioned FSP integration) only has to
 * honour the public API shape here.
 *
 * @warning Stub backend; NOT cryptographically secure. Do not ship
 *          key-bearing firmware against this header until a real
 *          backend is wired up.
 *
 * @see ra8_rsip.h  Underlying RSIP HAL surface.
 * @see ra8_rsip_protected.h  Crypto operations driven by wrapped keys.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"

/**
 * @enum ra8_rsip_wrapped_key_const_t
 * @brief Sizing constants for RSIP wrapped-key blobs.
 *
 * @details
 * Mirrors the FSP ``rsip_*_wrapped_key_t`` shape: a 4-byte type tag,
 * a fixed-size key-injection envelope, the raw key bytes, and a
 * 16-byte footer MAC. The sizes are picked to fit the largest
 * supported key (RSA-4096 modulus + exponent) plus padding so the
 * total blob fits in a single fixed-size buffer.
 *
 * @invariant ``k_ra8_rsip_wrapped_max_total`` ==
 *            ``k_ra8_rsip_wrapped_type_bytes`` +
 *            ``k_ra8_rsip_wrapped_mgmt_info_bytes`` +
 *            ``k_ra8_rsip_wrapped_max_payload`` +
 *            ``k_ra8_rsip_wrapped_mac_bytes``.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_wrapped_type_bytes      = 4U,   /**< Wrapped-blob type tag.          */
  k_ra8_rsip_wrapped_mgmt_info_bytes = 16U,  /**< Key-injection envelope.         */
  k_ra8_rsip_wrapped_mac_bytes       = 16U,  /**< Trailing integrity MAC.         */
  k_ra8_rsip_wrapped_max_payload     = 600U, /**< Largest key payload (RSA-4096). */
  k_ra8_rsip_wrapped_max_total       = 636U, /**< Largest wrapped blob in bytes.  */
} ra8_rsip_wrapped_key_const_t;

/**
 * @enum ra8_rsip_wrapped_key_type_t
 * @brief Type tag stored in the leading 4 bytes of a wrapped blob.
 *
 * @details
 * The tag lets the protected layer reject blobs created for the wrong
 * algorithm before the engine even sees them.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_wrapped_type_aes      = 0xD1D2D3D4UL, /**< AES key blob.     */
  k_ra8_rsip_wrapped_type_rsa_pub  = 0xE1E2E3E4UL, /**< RSA public blob.  */
  k_ra8_rsip_wrapped_type_rsa_priv = 0xE5E6E7E8UL, /**< RSA private blob. */
  k_ra8_rsip_wrapped_type_ecc_pub  = 0xF1F2F3F4UL, /**< ECC public blob.  */
  k_ra8_rsip_wrapped_type_ecc_priv = 0xF5F6F7F8UL, /**< ECC private blob. */
} ra8_rsip_wrapped_key_type_t;

/**
 * @enum ra8_rsip_aes_key_bits_t
 * @brief AES key-width selector accepted by ``ra8_rsip_key_inject_aes``.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_rsip_aes_key_bits_128 = 128U, /**< AES-128. */
  k_ra8_rsip_aes_key_bits_192 = 192U, /**< AES-192. */
  k_ra8_rsip_aes_key_bits_256 = 256U, /**< AES-256. */
} ra8_rsip_aes_key_bits_t;

/**
 * @brief Wrap a raw AES key into the RSIP installed-key format.
 *
 * @details
 * Produces a self-describing blob the RSIP engine accepts as a
 * "wrapped key": a leading type tag, the key-injection envelope, the
 * AES key bytes, and a trailing MAC over the whole structure. The
 * blob is the only artefact a caller may pass to
 * ``ra8_rsip_protected_aes_init``.
 *
 * @warning Stub: the wrapping is deterministic; not real key
 *          isolation. See file-level @warning.
 *
 * @param[out] installed_key_buf Destination buffer; must be
 *                               ``k_ra8_rsip_wrapped_max_total`` bytes.
 * @param[in]  raw_key           Raw AES key bytes (``key_bits / 8``).
 * @param[in]  key_bits          AES key width.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob written.
 * @retval k_ra8_err_null_ptr ``installed_key_buf`` or ``raw_key`` was NULL.
 * @retval k_ra8_err_invalid_arg Unsupported ``key_bits``.
 *
 * @pre ``installed_key_buf`` is ``k_ra8_rsip_wrapped_max_total`` bytes.
 * @pre ``raw_key`` is at least ``key_bits / 8`` bytes.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 * @post First 4 bytes of ``installed_key_buf`` equal
 *       ``k_ra8_rsip_wrapped_type_aes`` (little-endian).
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_protected_aes_init
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_inject_aes(uint8_t*                installed_key_buf,
                                                const uint8_t*          raw_key,
                                                ra8_rsip_aes_key_bits_t key_bits);

/**
 * @brief Wrap a raw RSA key (modulus + exponent) into the
 *        installed-key format.
 *
 * @details
 * The payload layout is ``[modulus | 4-byte exponent]``. Both private
 * and public RSA blobs share this representation and are
 * differentiated only by the leading type tag (private blobs use
 * ``k_ra8_rsip_wrapped_type_rsa_priv``).
 *
 * @param[out] installed_key_buf Destination
 *                               (``k_ra8_rsip_wrapped_max_total``).
 * @param[in]  raw_modulus       Big-endian modulus bytes
 *                               (``size / 8`` long).
 * @param[in]  raw_exponent      Big-endian exponent bytes (4 bytes for
 *                               public e; truncated to 4 for the stub).
 * @param[in]  size              RSA modulus width.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob written.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Unsupported ``size``.
 *
 * @pre ``installed_key_buf`` is ``k_ra8_rsip_wrapped_max_total`` bytes.
 * @pre ``raw_modulus`` is ``size / 8`` bytes.
 * @pre ``raw_exponent`` is at least 4 bytes.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_protected_rsa_decrypt
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_inject_rsa(uint8_t*            installed_key_buf,
                                                const uint8_t*      raw_modulus,
                                                const uint8_t*      raw_exponent,
                                                ra8_rsip_rsa_size_t size);

/**
 * @brief Wrap a raw ECC scalar (private or public) into the
 *        installed-key format.
 *
 * @details
 * For private keys the payload is the raw scalar bytes; for public
 * keys the payload is ``X || Y`` (the uncompressed point coordinates).
 * The byte counts come from ``curve``.
 *
 * @param[out] installed_key_buf Destination buffer.
 * @param[in]  curve             Curve identifier.
 * @param[in]  raw_priv_or_pub   Raw scalar bytes; the buffer length
 *                               is implied by ``curve``.
 * @param[in]  is_private        ``true`` -> wrap a private scalar;
 *                               ``false`` -> wrap a public point.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob written.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_invalid_arg Unsupported ``curve``.
 *
 * @pre ``installed_key_buf`` is ``k_ra8_rsip_wrapped_max_total`` bytes.
 * @pre ``raw_priv_or_pub`` is large enough for ``curve``.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_rsip_protected_ecdsa_sign
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_inject_ecc(uint8_t*         installed_key_buf,
                                                ra8_rsip_curve_t curve,
                                                const uint8_t*   raw_priv_or_pub,
                                                bool             is_private);

/**
 * @brief Validate that a wrapped-key blob has the expected structure
 *        and a matching trailing MAC.
 *
 * @details
 * Parses the leading type tag, recomputes the MAC over
 * ``[type | mgmt-info | payload]`` using the same xorshift mixer
 * the inject path used, and returns ``k_ra8_ok`` only if it matches.
 * Any single-byte tamper in the blob causes the recomputed MAC to
 * diverge.
 *
 * @param[in] installed_key_buf Wrapped blob.
 * @param[in] expected_type     Expected type tag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob is well-formed and MAC matches.
 * @retval k_ra8_err_null_ptr ``installed_key_buf`` was NULL.
 * @retval k_ra8_err_invalid_arg Type tag mismatched.
 * @retval k_ra8_err_hw_error Trailing MAC mismatched.
 *
 * @pre ``installed_key_buf`` is at least
 *      ``k_ra8_rsip_wrapped_max_total`` long.
 * @post No state changes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_validate(const uint8_t*              installed_key_buf,
                                              ra8_rsip_wrapped_key_type_t expected_type);

#ifdef __cplusplus
}
#endif
