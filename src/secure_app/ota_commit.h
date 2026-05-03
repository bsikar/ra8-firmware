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
 * - ``ra_ota_commit_swap_bank(slot)`` arms the boot ROM to start
 *   from the *other* bank on next reset. NS code can request this
 *   via the NSC veneer ``ra_nsc_ota_commit``; the secure side is
 *   the only world allowed to touch the option byte.
 *
 * - ``ra_ota_commit_set_bank_config(value)`` programs the raw
 *   ``OFS3``-style configuration register that selects the active
 *   bank, debug ports, and integrity check mode. The veneer
 *   ``ra_nsc_flash_bank_config`` lets NS supply just the bank-select
 *   bits; everything else is masked off in this module.
 *
 * Both functions are stubbed against an in-memory shadow on host so
 * the unit tests can verify the masking and re-entry guards without
 * touching real flash. The real ``ra_flash_*`` driver call sites
 * are documented in the implementation; they get wired in once the
 * register-protect (PRCR) sequence is centralised in .
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
 * @enum ra_ota_bank_t
 * @brief Bank selector used by ``ra_ota_commit_swap_bank``.
 */
typedef enum : uint8_t {
  k_ra_ota_bank_a = 0U, /**< Bank A is the next-boot target. */
  k_ra_ota_bank_b = 1U, /**< Bank B is the next-boot target. */
} ra_ota_bank_t;

/**
 * @enum ra_ota_bank_config_mask_t
 * @brief Allowed-bit masks for the bank-config register.
 *
 * @details
 * The bank-config register has a handful of fields the secure side
 * exposes to NS callers; everything else (debug-disable, integrity
 * check mode, lifetime fuses) is filtered out before the write
 * lands.
 */
typedef enum : uint32_t {
  k_ra_ota_bank_config_allowed = 0x00000003U, /**< Only the 2-bit BANK_SEL field. */
} ra_ota_bank_config_mask_t;

/**
 * @brief Reset the secure OTA shadow (drops any pending commit).
 *
 * @return ``ra_err_t`` error code (currently always ``k_ra_ok``).
 *
 * @pre Called from the secure boot path before any veneer can fire.
 *
 * @post No swap is pending.
 * @post Bank-config shadow reads zero.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ota_commit_reset(void);

/**
 * @brief Arm the boot ROM to start from ``target`` on next reset.
 *
 * @details
 * On the real chip this would call ``ra_flash_write`` against the
 * option region (see ``OFS3 / OSIS`` in HUM section 14.2). On host
 * it just records the request in a shadow that the unit tests can
 * inspect.
 *
 * @param[in] target Bank selector.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Swap armed.
 * @retval k_ra_err_invalid_arg   ``target`` not in ``ra_ota_bank_t``.
 * @retval k_ra_err_invalid_state Another commit is already pending.
 *
 * @pre Caller has already validated firmware integrity on ``target``.
 * @pre IRQs masked (or single-threaded boot context).
 *
 * @post On success, the shadow records ``target`` as pending.
 * @post On failure, no shadow state changes.
 *
 * @note Thread safety: not thread-safe; serialise via secure-side
 *       mutex.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ota_commit_swap_bank(ra_ota_bank_t target);

/**
 * @brief Read back the pending swap target (for tests + diagnostics).
 *
 * @param[out] out_target Pending bank, valid only when the function
 *                        returns ``k_ra_ok``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                A swap is pending; target written.
 * @retval k_ra_err_no_data       No swap pending.
 * @retval k_ra_err_null_ptr      ``out_target`` was NULL.
 *
 * @pre ``out_target`` non-NULL.
 *
 * @post On success, ``*out_target`` is one of ``k_ra_ota_bank_*``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ota_commit_pending(ra_ota_bank_t* out_target);

/**
 * @brief Write the bank-config register, masked to the allowed bits.
 *
 * @details
 * Any bit outside ``k_ra_ota_bank_config_allowed`` is silently
 * dropped. This is the entry point behind the
 * ``ra_nsc_flash_bank_config`` veneer.
 *
 * @param[in] raw_value Raw register value supplied by NS code.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok    Write accepted.
 *
 * @pre IRQs masked (or single-threaded boot context).
 *
 * @post Shadow reflects the masked value.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ota_commit_set_bank_config(uint32_t raw_value);

/**
 * @brief Read back the bank-config shadow (for tests).
 *
 * @param[out] out_value Filtered value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Value written.
 * @retval k_ra_err_null_ptr  ``out_value`` was NULL.
 *
 * @pre ``out_value`` non-NULL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ota_commit_get_bank_config(uint32_t* out_value);

#ifdef __cplusplus
}
#endif
