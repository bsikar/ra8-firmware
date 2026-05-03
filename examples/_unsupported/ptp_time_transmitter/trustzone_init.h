/**
 * @file examples/_unsupported/ptp_time_transmitter/trustzone_init.h
 * @brief Cortex-M85 TrustZone-M SAU bring-up entry point
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Programme + enable the SAU per the partition.
 *
 * @details
 * Called from ``SystemInit`` after the cache + MPU are up but
 * before any application code runs. When the firmware is built
 * without ``RA_TRUSTZONE_ENABLE`` defined this function is a
 * no-op so the single-world build is unaffected.
 *
 * @pre Called once from ``SystemInit``.
 * @pre Called from secure world only (the SAU registers live at
 * ``0xE000EDD0`` which is not reachable from NS).
 *
 * @post On success, SAU_CTRL.ENABLE is set and the four canonical
 * regions cover NS MRAM / SRAM / SDRAM + the NSC veneer alias.
 *
 * @par TrustZone Safety:
 * - **Validates:** SAU_TYPE.SREGION >= 4 before programming.
 * - **Trusts:** boot ROM left SAU disabled and IDAU at reset state.
 * - **Denies:** any access to the SAU registers from NS world.
 *
 * @note Thread safety: not thread-safe; runs once at boot.
 * @since 0.1.0
 */
void ra_trustzone_init(void);

#ifdef __cplusplus
}
#endif
