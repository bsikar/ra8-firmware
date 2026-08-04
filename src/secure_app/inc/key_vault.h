/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file key_vault.h
 * @brief Secure-only symmetric key store
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * deliverable. Stores up to ``k_ra8_key_vault_slots`` 256-bit
 * symmetric keys in a Secure-side static array that is unreachable
 * from the Non-Secure world after the SAU partition is enabled.
 *
 * ships the in-memory vault. The hardened storage path
 * (MRAM-backed Secure region or SCE7 hardware key wrap) is a
 * future deliverable that depends on the SCE driver gaining
 * key-injection support.
 *
 * The vault is intentionally minimal:
 *
 * - ``key_vault_init`` zeroes every slot.
 * - ``key_vault_store(slot, key)`` copies a 256-bit key into a
 * slot. Only callable from the secure world (no NSC veneer).
 * - ``key_vault_sha256_xor_challenge(slot, challenge, out)``
 * computes ``SHA-256(key XOR challenge)`` and returns the
 * 32-byte digest. This is the only operation the NS world can
 * reach via the NSC veneer
 * ``ra8_nsc_key_vault_challenge``; the raw key never leaves the
 * secure world.
 *
 * @par TrustZone Safety:
 * - **Validates:** slot index is in range; output pointer length
 * matches expected digest size.
 * - **Trusts:** the SAU partition keeps the static key array
 * inaccessible from NS. The veneer is the only NS->S path.
 * - **Denies:** raw key reads from any code path. Only the
 * SHA-256-of-XOR digest crosses the boundary.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_key_vault_limits_t
 * @brief Sizing constants for the vault.
 */
typedef enum : uint16_t {
  k_ra8_key_vault_slots         = 8U,  /**< Number of stored keys.      */
  k_ra8_key_vault_key_bytes     = 32U, /**< 256-bit symmetric key.      */
  k_ra8_key_vault_chal_bytes    = 32U, /**< Challenge length.           */
  k_ra8_key_vault_digest_bytes  = 32U, /**< SHA-256 output size.        */
  k_ra8_key_vault_mac_key_bytes = 32U, /**< Max key-authentication key. */
} ra8_key_vault_limits_t;

/**
 * @brief Initialise the vault (zero every slot).
 *
 * @return ``ra8_err_t`` error code (currently always ``k_ra8_ok``).
 *
 * @pre Called once from secure-world boot.
 *
 * @post Every slot reads as all-zeros.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_key_vault_init(void);

/**
 * @brief Programme a 256-bit symmetric key into a vault slot.
 *
 * @param[in] slot Slot index 0..k_ra8_key_vault_slots-1.
 * @param[in] key 32-byte symmetric key.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Key stored.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_null_ptr ``key`` was NULL.
 *
 * @pre Called from secure world only.
 * @pre ``key`` points to ``k_ra8_key_vault_key_bytes`` of secure RAM.
 *
 * @post The slot contains the key.
 *
 * @note Thread safety: secure-world only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_key_vault_store(uint16_t slot, const uint8_t* key);

/**
 * @brief Compute SHA-256(key XOR challenge) for slot.
 *
 * @details
 * This is the only operation the Non-Secure world can reach via
 * the ``ra8_nsc_key_vault_challenge`` veneer. The raw key never
 * leaves the secure world; what crosses the boundary is the
 * 32-byte SHA-256 digest of (key XOR challenge), which depends
 * on both the key and the challenge but reveals neither.
 *
 * @param[in] slot Slot index 0..k_ra8_key_vault_slots-1.
 * @param[in] challenge 32-byte challenge from the NS caller.
 * @param[out] out 32-byte digest destination.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Digest computed.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_null_ptr ``challenge`` or ``out`` was NULL.
 *
 * @pre Called from secure world (or via NSC veneer).
 * @pre ``challenge`` and ``out`` point to ``k_ra8_key_vault_*_bytes``.
 *
 * @post ``out[0..31]`` holds the digest.
 *
 * @note Thread safety: not thread-safe; the SHA-256 sponge is
 * single-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_key_vault_sha256_xor_challenge(uint16_t slot, const uint8_t* challenge, uint8_t* out);

/**
 * @brief Provision the key-authentication key (KAK) used to MAC key imports.
 *
 * @details
 * The KAK is the secret that keys the AES-CMAC over a wrapped-key blob in
 * ``ra8_key_import_seal``. It is stored in dedicated secure-side storage that
 * is *separate from the NS-importable slot array*, so it can never be
 * overwritten or read through the ``ra8_key_import_*`` / NSC path a
 * Non-Secure caller reaches. Secure boot (or the host test harness)
 * provisions it once before any import can occur. This is the single answer
 * to "where does the CMAC key come from": the secure key vault, never NS.
 *
 * @param[in] key     Raw AES-CMAC key material.
 * @param[in] key_len Key length: 16 (AES-128) or 32 (AES-256) bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               KAK stored.
 * @retval k_ra8_err_null_ptr     ``key`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``key_len`` was neither 16 nor 32.
 *
 * @pre Called from secure world only, during provisioning.
 * @pre ``key`` points to ``key_len`` bytes of secure RAM.
 * @post The KAK store holds the key and records ``key_len``.
 *
 * @note Thread safety: secure-world only, single-threaded provisioning.
 * @see ra8_key_vault_load_mac_key()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_key_vault_set_mac_key(const uint8_t* key, uint16_t key_len);

/**
 * @brief Copy the provisioned key-authentication key for a secure caller.
 *
 * @details
 * Secure-world-only accessor used by ``key_import.c`` to key the AES-CMAC. It
 * copies the KAK into a caller-supplied secure buffer; the material never
 * crosses to Non-Secure code (no NSC veneer exposes it). The caller wipes the
 * copy after use.
 *
 * @param[out] out     Destination for the KAK (secure scratch).
 * @param[in]  out_cap Capacity of ``out`` in bytes (``>= key_len``).
 * @param[out] out_len Receives the KAK length actually copied.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               KAK copied and ``*out_len`` set.
 * @retval k_ra8_err_null_ptr     ``out`` or ``out_len`` was NULL.
 * @retval k_ra8_err_not_found    No KAK has been provisioned yet.
 * @retval k_ra8_err_invalid_size ``out_cap`` was smaller than the KAK.
 *
 * @pre ``out`` and ``out_len`` are non-NULL.
 * @pre A KAK was provisioned via ::ra8_key_vault_set_mac_key.
 * @post On success, ``out[0..*out_len-1]`` holds the KAK.
 * @post On error, no KAK bytes are copied.
 *
 * @note Thread safety: not thread-safe; secure-world only.
 * @warning The caller must ::ra8_secure_memzero the copy after use.
 * @see ra8_key_vault_set_mac_key()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_key_vault_load_mac_key(uint8_t* out, uint16_t out_cap, uint16_t* out_len);

#ifdef __cplusplus
}
#endif
