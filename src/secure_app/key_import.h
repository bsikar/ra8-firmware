/**
 * @file key_import.h
 * @brief Secure-side sealed key import + opaque handle vending
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Layered on top of ``key_vault.h``. Where ``ra_key_vault_store`` is
 * a privileged secure-only API for one-time provisioning of a 32-byte
 * symmetric key into an in-memory slot, the ``ra_key_import_*`` API
 * is the bridge that lets Non-Secure code request a sealed import
 * and then refer to the key only by an *opaque handle* whose value
 * has no relation to the slot index or the key bytes.
 *
 * The model:
 *
 * - NS code holds a "sealed" blob (key bytes plus a 32-bit MAC).
 * - It hands the blob to the secure side via the
 *   ``ra_nsc_key_import`` veneer.
 * - The secure side validates the MAC, picks the next free slot,
 *   stores the key, and returns a 32-bit *handle* derived from the
 *   slot index and a per-boot salt.
 * - Subsequent NS calls (challenge/response) reference the handle;
 *   the secure side translates handle->slot internally.
 *
 * The sealing MAC in is intentionally tiny -- a length-tagged
 * XOR-fold rather than full CMAC -- because the real authentication
 * stage gets wired in once the SCE driver supports CMAC-on-import
 *. The handle layer, however, lands now so the rest of
 * the stack can be built against it.
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

/**
 * @enum ra_key_import_limits_t
 * @brief Sizing constants exposed to NS callers via the veneer.
 */
typedef enum : uint16_t {
  k_ra_key_import_blob_bytes  = 36U, /**< 32-byte key + 4-byte MAC.         */
  k_ra_key_import_mac_bytes   = 4U,  /**< Trailing MAC length.              */
  k_ra_key_import_key_bytes   = 32U, /**< Key portion length.               */
  k_ra_key_import_handle_zero = 0U,  /**< Reserved invalid-handle sentinel. */
} ra_key_import_limits_t;

/**
 * @brief Reset the import table (drops every handle) and re-seed the salt.
 *
 * @return ``ra_err_t`` error code (currently always ``k_ra_ok``).
 *
 * @pre Called from the secure boot path before any veneer can fire.
 *
 * @post Every previously vended handle now resolves to no slot.
 * @post The per-boot salt has been refreshed.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_key_import_reset(void);

/**
 * @brief Validate a sealed blob and import the key into a free slot.
 *
 * @details
 * Walks the sealed blob ``[key (32B) | mac (4B)]``, verifies the
 * MAC against the secret salt, and copies the key bytes into the
 * next free slot via ``ra_key_vault_store``. The returned handle
 * is an obfuscated form of the slot index that NS callers can
 * use as the ``slot`` argument to subsequent veneer calls.
 *
 * @param[in]  blob       Sealed blob (caller-owned secure copy).
 * @param[in]  blob_len   Must equal ``k_ra_key_import_blob_bytes``.
 * @param[out] out_handle Opaque handle (non-zero on success).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Key imported, handle written.
 * @retval k_ra_err_null_ptr      ``blob`` or ``out_handle`` was NULL.
 * @retval k_ra_err_invalid_size  ``blob_len`` mismatched.
 * @retval k_ra_err_invalid_arg   MAC verification failed.
 * @retval k_ra_err_no_mem        Every slot already occupied.
 *
 * @pre Called from secure world (or via the NSC veneer).
 * @pre ``blob`` points to ``blob_len`` bytes of secure scratch
 *      (the veneer copies from NS before calling this function).
 *
 * @post On success ``*out_handle != k_ra_key_import_handle_zero``.
 * @post On failure the import table is unchanged.
 *
 * @note Thread safety: not thread-safe; the slot bitmap is a single
 *       static.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_key_import_seal(const uint8_t* blob, uint32_t blob_len, uint32_t* out_handle);

/**
 * @brief Resolve a handle to the underlying slot index.
 *
 * @param[in]  handle    Handle previously returned from ``ra_key_import_seal``.
 * @param[out] out_slot  Slot index 0..k_ra_key_vault_slots-1 on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Slot returned.
 * @retval k_ra_err_null_ptr       ``out_slot`` was NULL.
 * @retval k_ra_err_not_found      Handle does not refer to a live import.
 *
 * @pre ``out_slot`` non-NULL.
 *
 * @post On success ``*out_slot < k_ra_key_vault_slots``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_key_import_resolve(uint32_t handle, uint16_t* out_slot);

/**
 * @brief Build a sealed blob from a raw key (provisioning + test helper).
 *
 * @details
 * Computes the MAC over ``key`` using the current per-boot salt and
 * writes ``[key | mac]`` to ``out_blob``. Secure-side provisioning
 * code uses this to package a key just before passing it through
 * the import API. Tests use it to drive the happy path.
 *
 * @param[in]  key      32-byte key.
 * @param[out] out_blob Destination of ``k_ra_key_import_blob_bytes``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Blob built.
 * @retval k_ra_err_null_ptr  ``key`` or ``out_blob`` was NULL.
 *
 * @pre Both pointers non-NULL.
 *
 * @post ``out_blob`` carries a blob that ``ra_key_import_seal`` will
 *       accept while the salt is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_key_import_build_blob(const uint8_t* key, uint8_t* out_blob);

#ifdef __cplusplus
}
#endif
