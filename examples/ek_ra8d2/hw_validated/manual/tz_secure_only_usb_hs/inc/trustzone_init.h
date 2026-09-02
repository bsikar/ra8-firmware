/**
 * @file examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/inc/trustzone_init.h
 * @brief No-op TrustZone bring-up entry point for the secure-only USB demo
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * This experiment runs the entire firmware in the Secure world without
 * an SAU partition. The function exists so the rest of the boot
 * scaffolding keeps its prototype but does nothing -- no SAU region
 * programming, no IDAU touching, no ALLNS bit. The chip stays in the
 * reset-state security configuration where every address is Secure
 * (the IDAU default).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief No-op TrustZone init for the secure-only USB experiment.
 *
 * @details
 * Returns immediately. The Cortex-M85 boots in the Secure world by
 * design; without an SAU programming step the entire 4 GiB address
 * space remains Secure as the IDAU default dictates. This is exactly
 * the configuration we want to test against the
 * ``ra8_cgc_pll2_enable`` failure observed in the dual-world build.
 *
 * @pre None.
 * @post No SAU/IDAU register writes performed; security state is
 *       unchanged from reset.
 *
 * @note Thread safety: trivially safe (empty function body).
 * @since 0.1.0
 */
void ra8_trustzone_init(void);

#ifdef __cplusplus
}
#endif
