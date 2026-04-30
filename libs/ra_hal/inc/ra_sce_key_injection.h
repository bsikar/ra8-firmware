/**
 * @file ra_sce_key_injection.h
 * @brief SCE key-injection HAL -- wrap raw key material into the
 *        SCE installed-key blob format
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The Renesas FSP ``r_sce_key_injection`` driver bundles a raw key
 * (AES, RSA modulus + exponent, ECC private or public scalar) along
 * with a ``key_management_info`` envelope so the SCE engine can
 * accept key material without ever exposing the raw bytes to host
 * software again. This HAL exposes the wrapping operation only --
 * the wrapped key is then consumed by ``ra_sce_protected_*`` calls.
 *
 * The implementation in ``ra_sce_key_injection.c`` is a host-friendly
 * software stub that lays out the wrapped buffer in the same shape
 * the FSP layer would (``[type | mgmt-info | key-bytes |
 * footer-mac]``) but uses the same deterministic xorshift mixer as
 * the rest of ``ra_sce`` to produce the embedded MAC. A future
 * drop-in real backend (sanctioned FSP integration) only has to
 * honour the public API shape here.
 *
 * @warning Stub backend; NOT cryptographically secure. Do not ship
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

/**
 * @enum ra_sce_wrapped_key_const_t
 * @brief Sizing constants for SCE wrapped-key blobs.
 *
 * @details
 * Mirrors the FSP ``sce_*_wrapped_key_t`` shape: a 4-byte type tag,
 * a fixed-size key-management-info pad, the raw key bytes, and a
 * 16-byte footer MAC. The sizes are picked to fit the largest
 * supported key (RSA-4096 modulus + exponent).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_wrapped_type_bytes      = 4U,   /**< Wrapped-blob type tag.            */
  k_ra_sce_wrapped_mgmt_info_bytes = 16U,  /**< Key-management envelope.          */
  k_ra_sce_wrapped_mac_bytes       = 16U,  /**< Trailing integrity MAC.           */
  k_ra_sce_wrapped_max_payload     = 600U, /**< Largest key payload (RSA-4096).   */
  k_ra_sce_wrapped_max_total       = 640U, /**< Largest wrapped blob in bytes.    */
} ra_sce_wrapped_key_const_t;

/**
 * @enum ra_sce_wrapped_key_type_t
 * @brief Type tag stored in the leading 4 bytes of a wrapped blob.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_wrapped_type_aes      = 0xA1A2A3A4UL, /**< AES key blob.    */
  k_ra_sce_wrapped_type_rsa_pub  = 0xB1B2B3B4UL, /**< RSA public blob. */
  k_ra_sce_wrapped_type_rsa_priv = 0xB5B6B7B8UL, /**< RSA private blob.*/
  k_ra_sce_wrapped_type_ecc_pub  = 0xC1C2C3C4UL, /**< ECC public blob. */
  k_ra_sce_wrapped_type_ecc_priv = 0xC5C6C7C8UL, /**< ECC private blob.*/
} ra_sce_wrapped_key_type_t;

/**
 * @brief Wrap a raw AES key into the SCE installed-key format.
 *
 * @details
 * Produces a self-describing blob the SCE engine accepts as a
 * "wrapped key": a leading type tag, the key-management envelope,
 * the encrypted key bytes, and a trailing MAC over the whole
 * structure. The blob is the only artefact a caller may pass to
 * ``ra_sce_protected_aes_init``.
 *
 * @warning Stub: the wrapping is deterministic XOR; not real key
 *          isolation. See file-level @warning.
 *
 * @param[out] installed_key_buf Destination buffer; must be
 *                               ``k_ra_sce_wrapped_max_total``.
 * @param[in]  raw_key           Raw AES key bytes.
 * @param[in]  key_bits          AES key width.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Unsupported ``key_bits``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @pre Buffer sizes match.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_key_inject_aes(uint8_t*              installed_key_buf,
                                             const uint8_t*        raw_key,
                                             ra_sce_aes_key_bits_t key_bits);

/**
 * @brief Wrap a raw RSA key (modulus + exponent) into the installed-key
 *        format.
 *
 * @param[out] installed_key_buf Destination (``k_ra_sce_wrapped_max_total``).
 * @param[in]  raw_modulus       Big-endian modulus bytes.
 * @param[in]  raw_exponent      Big-endian exponent bytes (private d
 *                               for private blobs, public e for
 *                               public blobs).
 * @param[in]  key_bits          RSA modulus width.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Unsupported ``key_bits``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_key_inject_rsa(uint8_t*              installed_key_buf,
                                             const uint8_t*        raw_modulus,
                                             const uint8_t*        raw_exponent,
                                             ra_sce_rsa_key_bits_t key_bits);

/**
 * @brief Wrap a raw ECC scalar (private or public) into the
 *        installed-key format.
 *
 * @param[out] installed_key_buf  Destination buffer.
 * @param[in]  curve              Curve identifier.
 * @param[in]  raw_priv_or_pub    Raw scalar bytes; the buffer length
 *                                is implied by ``curve``: priv-bytes
 *                                for a private blob, pub-bytes for a
 *                                public blob.
 * @param[in]  is_private         true -> wrap a private scalar; false
 *                                -> wrap a public X || Y point.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob written.
 * @retval k_ra_err_null_ptr Any pointer was NULL.
 * @retval k_ra_err_invalid_arg Unsupported ``curve``.
 * @retval k_ra_err_invalid_state ``ra_sce_open`` had not been called.
 *
 * @pre ``ra_sce_open`` returned ``k_ra_ok``.
 * @post On success, ``installed_key_buf`` holds a valid wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_key_inject_ecc(uint8_t*           installed_key_buf,
                                             ra_sce_ecc_curve_t curve,
                                             const uint8_t*     raw_priv_or_pub,
                                             bool               is_private);

/**
 * @brief Validate that a wrapped-key blob has the expected structure
 *        and a matching trailing MAC.
 *
 * @details
 * Parses the leading type tag, recomputes the MAC over
 * ``[type | mgmt-info | payload]`` using the same xorshift mixer
 * the inject path used, and returns ``k_ra_ok`` only if it matches.
 *
 * @param[in] installed_key_buf  Wrapped blob.
 * @param[in] expected_type      Expected type tag.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Blob is well-formed and MAC matches.
 * @retval k_ra_err_null_ptr ``installed_key_buf`` was NULL.
 * @retval k_ra_err_invalid_arg Type tag mismatched.
 * @retval k_ra_err_hw_error Trailing MAC mismatched.
 *
 * @pre Buffer is at least ``k_ra_sce_wrapped_max_total`` long.
 * @post No state changes.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_key_validate(const uint8_t*            installed_key_buf,
                                           ra_sce_wrapped_key_type_t expected_type);

#ifdef __cplusplus
}
#endif
