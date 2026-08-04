/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ota_commit.h
 * @brief Secure-side OTA bank commit + flash-bank-config write
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * The RA8D2 supports a swappable A/B bank layout in MRAM where the
 * boot ROM picks the active bank from a flash-config option byte.
 * The two operations exposed here are privileged:
 *
 * - ``ra8_ota_commit_swap_bank(slot)`` arms the boot ROM to start
 *   from the *other* bank on next reset. NS code can request this
 *   via the NSC veneer ``ra8_nsc_ota_commit``; the secure side is
 *   the only world allowed to touch the option byte.
 *
 * - ``ra8_ota_commit_set_bank_config(value)`` programs the raw
 *   ``OFS3``-style configuration register that selects the active
 *   bank, debug ports, and integrity check mode. The veneer
 *   ``ra8_nsc_flash_bank_config`` lets NS supply just the bank-select
 *   bits; everything else is masked off in this module.
 *
 * Both writes touch the option region (an ``OFS3`` / ``BTFLG`` option-byte
 * program behind the PRCR unlock), which is brick-risky and not yet wired --
 * it is bench-gated. On silicon both functions are therefore **FAIL-CLOSED**:
 * they validate their arguments and then return ``k_ra8_err_not_supported``
 * instead of a fake ``k_ra8_ok`` (T5-10). Under ``RA8_OFF_TARGET`` they run
 * against an in-memory shadow so the unit tests can verify the masking and
 * re-entry guards without touching real flash. The real ``ra8_flash_*`` call
 * sites are marked ``TODO`` at each fail-closed branch in the implementation.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_ota_bank_t
 * @brief Bank selector used by ``ra8_ota_commit_swap_bank``.
 */
typedef enum : uint8_t {
  k_ra8_ota_bank_a = 0U, /**< Bank A is the next-boot target. */
  k_ra8_ota_bank_b = 1U, /**< Bank B is the next-boot target. */
} ra8_ota_bank_t;

/**
 * @enum ra8_ota_bank_config_mask_t
 * @brief Allowed-bit masks for the bank-config register.
 *
 * @details
 * The bank-config register has a handful of fields the secure side
 * exposes to NS callers; everything else (debug-disable, integrity
 * check mode, lifetime fuses) is filtered out before the write
 * lands.
 */
typedef enum : uint32_t {
  k_ra8_ota_bank_config_allowed = 0x00000003U, /**< Only the 2-bit BANK_SEL field. */
} ra8_ota_bank_config_mask_t;

/**
 * @brief Reset the secure OTA shadow (drops any pending commit).
 *
 * @return ``ra8_err_t`` error code (currently always ``k_ra8_ok``).
 *
 * @pre Called from the secure boot path before any veneer can fire.
 *
 * @post No swap is pending.
 * @post Bank-config shadow reads zero.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_reset(void);

/**
 * @brief Arm the boot ROM to start from ``target`` on next reset.
 *
 * @details
 * Arming a boot-bank swap requires programming the boot option region (an
 * ``OFS3`` / ``BTFLG`` option-byte write behind the PRCR unlock). That write is
 * **brick-risky** and is not yet wired -- it is bench-gated -- so on silicon
 * this function is **FAIL-CLOSED**: after the argument + idempotency checks it
 * returns ``k_ra8_err_not_supported`` and arms nothing, rather than reporting a
 * fake ``k_ra8_ok`` for a commit that never touched flash (T5-10). Under
 * ``RA8_OFF_TARGET`` it instead records the request in a host shadow so the
 * unit tests can exercise the argument-validation + single-shot idempotency
 * policy without real flash. A caller therefore never mistakes an unwritten
 * option byte for an armed swap.
 *
 * @param[in] target Bank selector (``k_ra8_ota_bank_a`` / ``k_ra8_ota_bank_b``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                (``RA8_OFF_TARGET`` only) swap armed in the
 *                                host shadow.
 * @retval k_ra8_err_invalid_arg   ``target`` not in ``ra8_ota_bank_t``.
 * @retval k_ra8_err_invalid_state Another commit is already pending.
 * @retval k_ra8_err_not_supported (silicon build) the real option-byte bank-swap
 *                                write is bench-gated -- fail-closed, nothing
 *                                armed.
 *
 * @pre Caller has already validated firmware integrity on ``target``.
 * @pre IRQs masked (or single-threaded boot context).
 *
 * @post On ``k_ra8_ok`` (fake), the shadow records ``target`` as pending.
 * @post On any error, no shadow state changes and no option byte is written.
 *
 * @note Thread safety: not thread-safe; serialise via secure-side
 *       mutex.
 * @warning On silicon this is a fail-closed stub: it never arms a swap. Do not
 *          treat a non-error return as a committed bank swap until the
 *          bench-gated option-byte write is wired.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_swap_bank(ra8_ota_bank_t target);

/**
 * @brief Read back the pending swap target (for tests + diagnostics).
 *
 * @param[out] out_target Pending bank, valid only when the function
 *                        returns ``k_ra8_ok``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                A swap is pending; target written.
 * @retval k_ra8_err_no_data       No swap pending.
 * @retval k_ra8_err_null_ptr      ``out_target`` was NULL.
 *
 * @pre ``out_target`` non-NULL.
 *
 * @post On success, ``*out_target`` is one of ``k_ra8_ota_bank_*``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_pending(ra8_ota_bank_t* out_target);

/**
 * @brief Write the bank-config register, masked to the allowed bits.
 *
 * @details
 * Any bit outside ``k_ra8_ota_bank_config_allowed`` is masked off first so an NS
 * caller can only touch the bank-select field. Persisting that masked value is
 * an option-region write (same PRCR-unlocked, brick-risky path as
 * ``ra8_ota_commit_swap_bank``); it is not yet wired, so on silicon this function
 * is **FAIL-CLOSED**: it returns ``k_ra8_err_not_supported`` and writes nothing,
 * rather than a fake ``k_ra8_ok`` (T5-10). Under ``RA8_OFF_TARGET`` it records
 * the masked value in the host shadow so the masking policy stays unit-testable.
 * This is the entry point behind the ``ra8_nsc_flash_bank_config`` veneer.
 *
 * @param[in] raw_value Raw register value supplied by NS code.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                (``RA8_OFF_TARGET`` only) masked value
 *                                recorded in the host shadow.
 * @retval k_ra8_err_not_supported (silicon build) the real option-region write is
 *                                bench-gated -- fail-closed, nothing written.
 *
 * @pre IRQs masked (or single-threaded boot context).
 * @pre ``raw_value`` may take any uint32_t value (reserved bits are masked off).
 *
 * @post On ``k_ra8_ok`` (fake), the shadow holds ``raw_value`` masked to the
 *       allowed bits.
 * @post On any error, no shadow state changes and no option region is written.
 *
 * @note Thread safety: not thread-safe.
 * @warning On silicon this is a fail-closed stub: it never persists a value.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_set_bank_config(uint32_t raw_value);

/**
 * @brief Read back the bank-config shadow (for tests).
 *
 * @param[out] out_value Filtered value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Value written.
 * @retval k_ra8_err_null_ptr  ``out_value`` was NULL.
 *
 * @pre ``out_value`` non-NULL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_get_bank_config(uint32_t* out_value);

#ifdef __cplusplus
}
#endif
