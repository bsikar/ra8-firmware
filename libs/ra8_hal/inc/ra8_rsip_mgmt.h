/**
 * @file ra8_rsip_mgmt.h
 * @brief Renesas Secure IP (RSIP-E50D) HAL -- vault / KDF / lifecycle / tamper
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Device-management surface of the RA8D2 RSIP-E50D HAL split out of
 * the ``ra8_rsip.h`` umbrella. This sub-header owns the OEM boot loader
 * version anti-rollback counter, the wrapped-key vault slots, the
 * KEK-backed key wrap / unwrap engine, the HKDF / HUK-UID key
 * derivation function, the device lifecycle + debug-authorisation
 * helpers, the tamper subsystem, and the DOTF key-delivery routing.
 *
 * Wrap / unwrap and KDF consume the opaque ``ra8_rsip_key_handle_t``
 * type defined in ``ra8_rsip_keys.h``, which this sub-header includes.
 *
 * @warning This entire surface FAILS CLOSED in a production build. The
 * RSIP-E50D has no documented command / security-state register map:
 * HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" (p 3302-3307) is a six-page
 * conceptual overview and HUM Ch 51 "Security Features" (p 3263-3301) is
 * a prose feature index, neither a register map. The key-management and
 * hash surface (``ra8_rsip_asym.c``, issues #214 / #215) and the device-
 * security surface -- lifecycle, debug authorisation, tamper, DPA arm
 * (``ra8_rsip_devsec.c``, issue #216) -- therefore return
 * ``k_ra8_err_not_supported`` outside the insecure off-target build
 * rather than fabricate a digest, key, or security-state answer. The
 * ``k_ra8_ok`` / ``@post`` contracts below describe the guarded fake
 * command path only; real key management runs on tf-psa-crypto (M85) and
 * real device-security state lives in the DLM / option-setting memory /
 * SAU, not an RSIP MMIO read.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip_keys.h"
#include "ra8_rsip_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * OEM boot loader version (anti-rollback)
 * =============================================================================
 */

/**
 * @brief Read the latched OEM boot loader version counter.
 *
 * @param[out] out Receives the 32-bit counter; never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``*out`` reflects the OEM_BL_VER cell.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_oem_bl_version_get(uint32_t* out);

/**
 * @brief Increment the OEM boot loader version (anti-rollback step).
 *
 * @details
 * Writes the inc-trigger word; the engine increments the counter
 * monotonically. The lock register MUST be set after a successful
 * boot to prevent further increments mid-flight.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Counter advanced.
 * @retval k_ra8_err_invalid_state OEM_BL_LOCK already latched.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre ``OEM_BL_LOCK`` is clear.
 * @post Counter has incremented by 1.
 * @post Counter value is observable via ``ra8_rsip_oem_bl_version_get``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_oem_bl_version_increment(void);

/**
 * @brief Latch the OEM_BL_LOCK so further increments are rejected.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Lock latched.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre Caller has finished any anti-rollback steps for this boot.
 * @post ``OEM_BL_LOCK`` reads as 1.
 * @post Subsequent ``ra8_rsip_oem_bl_version_increment`` returns
 * ``k_ra8_err_invalid_state``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_oem_bl_version_lock(void);

/* =============================================================================
 * Wrapped-key vault
 * =============================================================================
 */

/**
 * @brief Read a wrapped-key blob from a vault slot.
 *
 * @param[in] slot Slot index (0..``k_ra8_rsip_kv_slot_count``-1).
 * @param[out] out 64-byte buffer to receive the wrapped blob.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob returned.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 * @retval k_ra8_err_not_found Slot is empty.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra8_rsip_kv_slot_count``.
 * @pre ``out`` is non-NULL.
 * @post On success, ``out[0..63]`` holds the slot blob.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_kv_read(uint8_t slot, uint8_t* out);

/**
 * @brief Write a wrapped-key blob into a vault slot.
 *
 * @param[in] slot Slot index.
 * @param[in] in 64-byte wrapped blob.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot written.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_null_ptr ``in`` was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra8_rsip_kv_slot_count``.
 * @pre ``in`` is non-NULL.
 * @post Slot now reads back as ``in``.
 * @post Populated-slot count incremented if the slot was empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_kv_write(uint8_t slot, const uint8_t* in);

/**
 * @brief Zeroise a vault slot.
 *
 * @param[in] slot Slot index.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot erased.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``slot`` < ``k_ra8_rsip_kv_slot_count``.
 * @pre ``ra8_rsip_init`` has been called.
 * @post Slot reads back as zero.
 * @post Populated-slot count decremented if the slot was non-empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_kv_erase(uint8_t slot);

/**
 * @brief Snapshot the populated-slot count.
 *
 * @param[out] out Receives the count (0..16); never NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Count returned.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``*out`` is in [0..``k_ra8_rsip_kv_slot_count``].
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only, safe to call concurrently.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_kv_count(uint32_t* out);

/* =============================================================================
 * Key wrap / unwrap engine
 * =============================================================================
 */

/**
 * @brief Wrap a key handle into a transportable blob using a KEK.
 *
 * @param[in] kek Key-encryption-key handle.
 * @param[in] iv 16-byte wrap IV.
 * @param[in] src Source key handle to wrap.
 * @param[out] blob Wrapped blob output (64 bytes).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blob produced.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``kek->alg`` is an AES install opcode.
 * @pre ``iv`` is non-NULL.
 * @post On success, ``blob[0..63]`` is the wrapped blob.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_wrap(const ra8_rsip_key_handle_t* kek,
                                          const uint8_t*               iv,
                                          const ra8_rsip_key_handle_t* src,
                                          uint8_t*                     blob);

/**
 * @brief Unwrap a transportable blob into a key handle.
 *
 * @param[in] kek Key-encryption-key handle.
 * @param[in] iv 16-byte wrap IV used at wrap time.
 * @param[in] blob Wrapped blob (64 bytes).
 * @param[out] dest Destination key handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_null_ptr Any pointer was NULL.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 * @retval k_ra8_err_hw_error Blob authenticity check failed.
 *
 * @pre ``kek->alg`` is an AES install opcode.
 * @pre ``iv``, ``blob``, ``dest`` are non-NULL.
 * @post On success, ``dest`` carries the unwrapped algorithm + body.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_key_unwrap(const ra8_rsip_key_handle_t* kek,
                                            const uint8_t*               iv,
                                            const uint8_t*               blob,
                                            ra8_rsip_key_handle_t*       dest);

/* =============================================================================
 * Key Derivation Function
 * =============================================================================
 */

/**
 * @brief Derive a key from input keying material + label/salt/info.
 *
 * @details
 * Implements:
 * - HKDF-SHA-256 / 384 / 512 -- ``op`` selects the hash flavour;
 * ``ikm`` is consumed as the IKM.
 * - HUK / UID bound -- ``op`` selects the source root, ``ikm`` is
 * NULL, and the engine binds the device-unique value.
 *
 * @param[in] op KDF mode selector.
 * @param[in] ikm Input keying material handle (may be NULL for
 * HUK / UID modes).
 * @param[in] label Label / context bytes.
 * @param[in] label_len Label length in bytes.
 * @param[in] salt Salt bytes (may be NULL).
 * @param[in] salt_len Salt length in bytes.
 * @param[in] out_len Bytes of derived material requested.
 * @param[out] out Wrapped derived-key handle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handle filled.
 * @retval k_ra8_err_null_ptr ``out`` was NULL or ``label``/``ikm``
 * missing where required.
 * @retval k_ra8_err_invalid_arg ``out_len`` zero or larger than the
 * hash's max XOF length.
 * @retval k_ra8_err_hw_timeout Engine never signalled KDF_DONE.
 *
 * @pre ``op`` is one of ``k_ra8_rsip_kdf_op_*``.
 * @pre ``out`` is non-NULL.
 * @post On success ``out`` carries a wrapped HMAC-SHA-256 handle.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_kdf(ra8_rsip_kdf_op_t            op,
                                     const ra8_rsip_key_handle_t* ikm,
                                     const uint8_t*               label,
                                     uint32_t                     label_len,
                                     const uint8_t*               salt,
                                     uint32_t                     salt_len,
                                     uint32_t                     out_len,
                                     ra8_rsip_key_handle_t*       out);

/* =============================================================================
 * Device lifecycle + debug authorisation
 * =============================================================================
 */

/**
 * @brief Read the device-lifecycle state.
 *
 * @param[out] out Receives the lifecycle word.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``*out`` reflects ``LIFE_STATE``.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_life_get(ra8_rsip_life_state_t* out);

/**
 * @brief Advance the device lifecycle to a new state.
 *
 * @details
 * Lifecycle transitions are one-way (HUM Ch 51.1 p 3263). The
 * engine rejects backward moves with ``k_ra8_err_invalid_state``.
 *
 * @param[in] state Target lifecycle state.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transition complete.
 * @retval k_ra8_err_invalid_arg Unknown ``state``.
 * @retval k_ra8_err_invalid_state Transition would move backward.
 * @retval k_ra8_err_hw_timeout Engine never signalled DONE.
 *
 * @pre ``state`` is one of ``ra8_rsip_life_state_t``.
 * @pre ``state`` is forward of the current state.
 * @post Subsequent ``ra8_rsip_life_get`` returns ``state``.
 *
 * @note Thread safety: not thread-safe.
 * @warning Lifecycle transitions are irreversible.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_life_advance(ra8_rsip_life_state_t state);

/**
 * @brief Read the current debug authorisation level.
 *
 * @param[out] out Receives the level (AL0/AL1/AL2).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``*out`` is one of AL0/AL1/AL2.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_debug_level_get(ra8_rsip_debug_level_t* out);

/**
 * @brief Set the debug authorisation level.
 *
 * @param[in] level Target level.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Level applied.
 * @retval k_ra8_err_invalid_arg ``level`` out of range.
 *
 * @pre ``level`` is one of ``ra8_rsip_debug_level_t``.
 * @pre Lifecycle state allows the requested level.
 * @post Subsequent ``ra8_rsip_debug_level_get`` returns ``level``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_debug_level_set(ra8_rsip_debug_level_t level);

/* =============================================================================
 * Tamper subsystem
 * =============================================================================
 */

/**
 * @brief Enable or disable a set of tamper sources.
 *
 * @param[in] sources OR of ``k_ra8_rsip_tamper_src_*`` bits to enable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Mask applied.
 * @retval k_ra8_err_invalid_arg ``sources`` has bits outside the
 * documented mask.
 *
 * @pre ``sources`` AND-clean against ``k_ra8_rsip_tamper_src_all``.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``TAMPER_CTRL`` reads as ``sources``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_tamper_enable(uint32_t sources);

/**
 * @brief Read latched tamper-source flags.
 *
 * @param[out] out Receives the flag word.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``ra8_rsip_init`` has been called.
 * @post ``*out`` reflects ``TAMPER_STATUS``.
 * @post No engine state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_tamper_status(uint32_t* out);

/**
 * @brief Acknowledge tamper-source flags (write-1-to-clear).
 *
 * @param[in] mask Bits to clear.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bits cleared.
 * @retval k_ra8_err_invalid_arg ``mask`` had bits outside the field.
 *
 * @pre ``mask`` AND-clean against ``k_ra8_rsip_tamper_src_all``.
 * @pre ``mask`` is non-zero.
 * @post Requested bits read as zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_tamper_ack(uint32_t mask);

/**
 * @brief Arm or disarm SPA / DPA countermeasures.
 *
 * @param[in] enable ``true`` to arm, ``false`` to disarm.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Done.
 *
 * @pre ``ra8_rsip_init`` has been called.
 * @pre Engine is idle.
 * @post ``CTRL.DPA_ARM`` matches ``enable``.
 * @post Subsequent crypto ops run with countermeasures as requested.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_dpa_arm(bool enable);

/* =============================================================================
 * DOTF key delivery routing
 * =============================================================================
 */

/**
 * @brief Route a wrapped-key vault slot to one of the DOTF instances.
 *
 * @param[in] which DOTF instance (0 or 1).
 * @param[in] slot Vault slot to feed (must contain an AES key).
 * @param[in] on ``true`` to enable the route, ``false`` to disable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Route applied.
 * @retval k_ra8_err_invalid_arg ``which`` not 0/1 or ``slot`` out
 * of range.
 *
 * @pre ``which`` < 2.
 * @pre If ``on`` is ``true``, ``slot`` < ``k_ra8_rsip_kv_slot_count``.
 * @post ``DOTFn_CTRL`` reflects the requested route.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rsip_dotf_route(uint8_t which, uint8_t slot, bool on);

#ifdef __cplusplus
}
#endif
