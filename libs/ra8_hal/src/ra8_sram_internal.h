/**
 * @file ra8_sram_internal.h
 * @brief src/-local shared surface for the SRAM HAL driver split.
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Declares the module-private state shared between the two SRAM driver
 * translation units:
 *
 *  - ``ra8_sram.c``           -- lifecycle, ECC mode, status/clear,
 *                               zero-init, self-test, introspection.
 *  - ``ra8_sram_security.c``  -- TrustZone security attribution + the ECC
 *                               error callback fan-out.
 *
 * The per-(global, bank) ECC error callback table lives in
 * ``ra8_sram_security.c`` (the callback owner) and is referenced from
 * ``ra8_sram.c`` only so ``ra8_sram_deinit`` can clear it on teardown.
 * These ``extern`` declarations give that one cross-TU reference a
 * single, documented home instead of a stray forward declaration.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_sram.h"
#include "ra8_sram_regs.h"

/**
 * @var s_sram_on_error
 * @brief Registered global ECC error callback (NULL until attach).
 *
 * @details
 * Defined in ``ra8_sram_security.c``. Referenced by ``ra8_sram_deinit``
 * in ``ra8_sram.c`` to drop the registration on teardown.
 *
 * @note Not thread-safe; mutate under the same single-threaded context
 *       as the rest of the driver.
 * @warning Do not assign directly outside the SRAM driver TUs.
 * @since 0.1.0
 */
extern ra8_sram_error_fn_t s_sram_on_error;

/**
 * @var s_sram_on_error_ctx
 * @brief Caller context forwarded to ``s_sram_on_error``.
 *
 * @details Defined in ``ra8_sram_security.c``.
 *
 * @note Not thread-safe.
 * @warning Do not assign directly outside the SRAM driver TUs.
 * @since 0.1.0
 */
extern void* s_sram_on_error_ctx;

/**
 * @var s_sram_on_error_bank
 * @brief Per-bank ECC error callback table (NULL until attach).
 *
 * @details Defined in ``ra8_sram_security.c``.
 *
 * @note Not thread-safe.
 * @warning Do not assign directly outside the SRAM driver TUs.
 * @since 0.1.0
 */
extern ra8_sram_error_fn_t s_sram_on_error_bank[k_ra8_sram_bank_count];

/**
 * @var s_sram_on_error_bank_ctx
 * @brief Per-bank context forwarded to ``s_sram_on_error_bank``.
 *
 * @details Defined in ``ra8_sram_security.c``.
 *
 * @note Not thread-safe.
 * @warning Do not assign directly outside the SRAM driver TUs.
 * @since 0.1.0
 */
extern void* s_sram_on_error_bank_ctx[k_ra8_sram_bank_count];

#ifdef __cplusplus
}
#endif
