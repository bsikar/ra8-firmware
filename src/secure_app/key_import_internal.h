/**
 * @file key_import_internal.h
 * @brief Secure-side sealed key import + opaque handle vending
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Layered on top of ``key_vault.h``. Where ``ra8_key_vault_store`` is
 * a privileged secure-only API for one-time provisioning of a 32-byte
 * symmetric key into an in-memory slot, the ``ra8_key_import_*`` API
 * is the bridge that lets Non-Secure code request a sealed import
 * and then refer to the key only by an *opaque handle* whose value
 * has no relation to the slot index or the key bytes.
 *
 * The model:
 *
 * - NS code holds a "sealed" blob (key bytes plus a 128-bit CMAC).
 * - It hands the blob to the secure side via the
 *   ``ra8_nsc_key_import`` veneer.
 * - The secure side validates the CMAC, picks the next free slot,
 *   stores the key, and returns a 32-bit *handle* derived from the
 *   slot index and a per-boot salt.
 * - Subsequent NS calls (challenge/response) reference the handle;
 *   the secure side translates handle->slot internally.
 *
 * The sealing MAC is AES-CMAC (NIST SP 800-38B) over the 32 key
 * bytes, keyed by the key-authentication key (KAK) the vault holds
 * (``ra8_key_vault_load_mac_key``) -- a secret the Non-Secure world
 * can never reach. The CMAC itself is computed through the
 * ``ra8_sec_cmac_*`` seam, which routes to the silicon-proven
 * TF-PSA-Crypto ``psa_mac_*`` backend on the firmware image and to a
 * KAT-pinned in-tree AES-CMAC reference in host tests. A forged blob
 * therefore requires recovering the KAK, not merely replaying a
 * trivial fold.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_key_import_limits_t
 * @brief Sizing constants exposed to NS callers via the veneer.
 */
typedef enum : uint16_t {
  k_ra8_key_import_blob_bytes  = 48U, /**< 32-byte key + 16-byte CMAC.       */
  k_ra8_key_import_mac_bytes   = 16U, /**< Trailing AES-CMAC tag length.     */
  k_ra8_key_import_key_bytes   = 32U, /**< Key portion length.               */
  k_ra8_key_import_handle_zero = 0U,  /**< Reserved invalid-handle sentinel. */
} ra8_key_import_limits_t;

/**
 * @brief Reset the import table (drops every handle) and re-seed the salt.
 *
 * @return ``ra8_err_t`` error code (currently always ``k_ra8_ok``).
 *
 * @pre Called from the secure boot path before any veneer can fire.
 *
 * @post Every previously vended handle now resolves to no slot.
 * @post The per-boot salt has been refreshed.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_key_import_reset(void);

/**
 * @brief Validate a sealed blob and import the key into a free slot.
 *
 * @details
 * Walks the sealed blob ``[key (32B) | cmac (16B)]``, verifies the
 * AES-CMAC keyed by the vault KAK, and copies the key bytes into the
 * next free slot via ``ra8_key_vault_store``. The returned handle
 * is an obfuscated form of the slot index that NS callers can
 * use as the ``slot`` argument to subsequent veneer calls.
 *
 * @param[in]  blob       Sealed blob (caller-owned secure copy).
 * @param[in]  blob_len   Must equal ``k_ra8_key_import_blob_bytes``.
 * @param[out] out_handle Opaque handle (non-zero on success).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Key imported, handle written.
 * @retval k_ra8_err_null_ptr      ``blob`` or ``out_handle`` was NULL.
 * @retval k_ra8_err_invalid_size  ``blob_len`` mismatched.
 * @retval k_ra8_err_invalid_arg   CMAC verification failed.
 * @retval k_ra8_err_not_found     No KAK provisioned in the vault.
 * @retval k_ra8_err_no_mem        Every slot already occupied.
 *
 * @pre Called from secure world (or via the NSC veneer).
 * @pre A KAK was provisioned via ``ra8_key_vault_set_mac_key``.
 * @pre ``blob`` points to ``blob_len`` bytes of secure scratch
 *      (the veneer copies from NS before calling this function).
 *
 * @post On success ``*out_handle != k_ra8_key_import_handle_zero``.
 * @post On failure the import table is unchanged.
 *
 * @note Thread safety: not thread-safe; the slot bitmap is a single
 *       static.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
ra8_key_import_seal(const uint8_t* blob, uint32_t blob_len, uint32_t* out_handle);

/**
 * @brief Resolve a handle to the underlying slot index.
 *
 * @param[in]  handle    Handle previously returned from ``ra8_key_import_seal``.
 * @param[out] out_slot  Slot index 0..k_ra8_key_vault_slots-1 on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Slot returned.
 * @retval k_ra8_err_null_ptr       ``out_slot`` was NULL.
 * @retval k_ra8_err_not_found      Handle does not refer to a live import.
 *
 * @pre ``out_slot`` non-NULL.
 *
 * @post On success ``*out_slot < k_ra8_key_vault_slots``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_key_import_resolve(uint32_t handle, uint16_t* out_slot);

/**
 * @brief Build a sealed blob from a raw key (provisioning + test helper).
 *
 * @details
 * Computes the AES-CMAC over ``material`` using the vault KAK and
 * writes ``[key | cmac]`` to ``out_blob``. Secure-side provisioning
 * code uses this to package a key just before passing it through the
 * import API. Tests use it to drive the happy path.
 *
 * @param[in]  material 32-byte key material to seal.
 * @param[out] out_blob Destination of ``k_ra8_key_import_blob_bytes``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Blob built.
 * @retval k_ra8_err_null_ptr  ``material`` or ``out_blob`` was NULL.
 * @retval k_ra8_err_not_found No KAK provisioned in the vault.
 *
 * @pre Both pointers non-NULL.
 * @pre A KAK was provisioned via ``ra8_key_vault_set_mac_key``.
 *
 * @post ``out_blob`` carries a blob that ``ra8_key_import_seal`` will
 *       accept while the KAK is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_key_import_build_blob(const uint8_t* material,
                                                           uint8_t*       out_blob);

#ifdef __cplusplus
}
#endif
