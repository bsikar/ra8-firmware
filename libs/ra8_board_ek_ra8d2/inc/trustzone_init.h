/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file libs/ra8_board_ek_ra8d2/inc/trustzone_init.h
 * @brief Cortex-M85 TrustZone-M SAU bring-up entry point
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name J-Link bench-probe verdict globals
 *
 * @details
 * The TrustZone bring-up TU that defines ``ra8_trustzone_init()`` latches
 * its per-step verdicts into these ``volatile`` globals so a debugger can
 * read them across a halt without any console. They are a bench-probe
 * interface, NOT an API: firmware code other than the defining TU must
 * never read or write them. Each defining TU provides the subset it
 * latches; the others stay undefined and unreferenced in that image.
 *
 * @note Written once during boot by the defining TU; read externally by
 * J-Link only.
 * @since 0.1.0
 * @{
 */
/** @brief Confirmed PSARB read-back after the USBFS NS-attribution write. */
extern volatile uint32_t g_tz_usb_psarb_readback;
/** @brief First non-OK ``ra8_err_t`` from the USB pin + PLL setup (0 = OK). */
extern volatile uint32_t g_tz_usb_pins_err;
/** @brief Last ``ra8_err_t`` from the U15 I/O-expander host-mode write (0 = OK). */
extern volatile uint32_t g_tz_usb_expander_err;
/** @brief Denial verdict from ``ra8_tz_secure_boot_jump_ns`` (0 = never denied). */
extern volatile uint32_t g_tz_jump_ns_err;
/** @} */

/**
 * @brief Programme + enable the SAU per the partition.
 *
 * @details
 * Called from ``SystemInit`` after the cache + MPU are up but
 * before any application code runs. When the firmware is built
 * without ``RA8_TRUSTZONE_ENABLE`` defined this function is a
 * no-op so the single-world build is unaffected.
 *
 * @pre Called once from ``SystemInit``.
 * @pre Called from secure world only (the SAU registers live at
 * ``0xE000EDD0`` which is not reachable from NS).
 *
 * @post On success, SAU_CTRL.ENABLE is set and the four canonical
 * regions cover NS MRAM / SRAM / SDRAM + the NSC veneer alias.
 * @post On an unusable SAU (< 4 regions) SAU_CTRL.ENABLE stays clear and
 * the caller falls back to the single-world model.
 *
 * @par TrustZone Safety:
 * - **Validates:** SAU_TYPE.SREGION >= 4 before programming.
 * - **Trusts:** boot ROM left SAU disabled and IDAU at reset state.
 * - **Denies:** any access to the SAU registers from NS world.
 *
 * @note Thread safety: not thread-safe; runs once at boot.
 * @since 0.1.0
 */
void ra8_trustzone_init(void);

#ifdef __cplusplus
}
#endif
