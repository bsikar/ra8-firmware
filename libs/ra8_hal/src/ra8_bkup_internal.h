/**
 * @file ra8_bkup_internal.h
 * @brief Cross-TU shared surface for the ra8_bkup driver split.
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The ra8_bkup driver is split across three translation units to stay
 * under the per-file line-count cap, one per responsibility:
 *
 * - ``ra8_bkup.c``          -- power-supply lifecycle, status registers,
 *                              the VBTBKRn backup store, the VBATT analog
 *                              voltage monitor, and the interrupt path.
 * - ``ra8_bkup_tamper.c``   -- tamper detection and RTCIC pad wiring.
 * - ``ra8_bkup_security.c`` -- the TrustZone attribution registers.
 *
 * This src/-local header carries the only two symbols those TUs share:
 * the init latch that gates the ISR, and the protected read-modify-write
 * helper. Everything else stayed private to the TU that owns it -- the
 * tamper channel-bit helper and per-channel validator are used nowhere
 * else, and ``ra8_bkup_security.c`` shares nothing at all. It is NOT
 * part of the public ABI; production code outside this driver must use
 * ``ra8_bkup.h``.
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

/**
 * @var s_bkup_initialized
 * @brief ``true`` once any of the lifecycle init helpers have run.
 *
 * @details
 * Defined once in ``ra8_bkup.c`` and referenced from
 * ``ra8_bkup_tamper.c`` through this declaration: ``ra8_bkup_tamper_init``
 * is an init entry point in its own right, so it sets the latch too.
 * Read only by ``ra8_bkup_isr_handle``, which returns
 * ``k_ra8_err_not_initialized`` while the latch is clear.
 *
 * @note Everything else in the driver is state-less, so the flag is set
 *       generously rather than tracking a full lifecycle.
 * @warning File-scope state, not thread-safe. Mutate only from
 *          single-threaded init or with IRQs masked.
 * @since 0.1.0
 */
extern bool s_bkup_initialized;

/**
 * @brief Set or clear a bit in an 8-bit PRCR-protected RMW register.
 *
 * @details
 * The read half needs no unlock (PRCR gates writes only, HUM Ch 13.1
 * p 520), so only the store runs inside the protection window. The
 * caller supplies the window because this driver spans three groups:
 * PRC1 for the VBT* file and PRC3 for VBATTMNSELR.
 *
 * Shared by both register-touching TUs of the driver: ``ra8_bkup.c``
 * uses it for the status W0C paths and the voltage-monitor select, and
 * ``ra8_bkup_tamper.c`` uses it for the per-channel input enable.
 *
 * @param[in,out] reg        Pointer to the live 8-bit register.
 * @param[in]     mask       Bit mask to manipulate.
 * @param[in]     enable     ``true`` to set, ``false`` to clear.
 * @param[in]     unlock_val ``k_ra8_prcr_unlock_*`` for @p reg's group.
 *
 * @pre reg != nullptr.
 * @pre mask != 0.
 * @pre unlock_val names the PRCR group that owns @p reg (HUM Table 13.1).
 * @post Bits in ``mask`` reflect ``enable``; other bits unchanged.
 * @post PRCR is relocked.
 *
 * @warning The store is only durable inside the unlock window: a write
 *          issued while @p reg's PRCR group is locked is discarded
 *          silently, with no bus fault and no status flag (issue #131).
 *          Do not separate the write from its unlock.
 *
 * @note Thread safety: not thread-safe; caller must serialise.
 * @since 0.1.0
 */
RA8_PRIV void
ra8_bkup_internal_rmw8(volatile uint8_t* reg, uint8_t mask, bool enable, uint16_t unlock_val);

#ifdef __cplusplus
}
#endif
