/**
 * @file examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/trustzone_init.c
 * @brief No-op TrustZone bring-up for the secure-only USB experiment
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * The dual-world `usb_cdc_echo` build programs the SAU to carve a
 * Non-Secure half out of MRAM/SRAM/SDRAM and exposes a Non-Secure
 * Callable veneer page. On real EK-RA8D2 silicon that build hangs in
 * `ra8_cgc_pll2_enable` because the PRCR-protected PLL2/USB clock
 * registers (HUM Ch. 9) silently drop writes from the address aliases
 * the SAU labels Non-Secure.
 *
 * This experiment removes every SAU/IDAU touch so the chip stays in
 * the reset-state security configuration: bit 28 of the address is the
 * IDAU's default S/NS attribute, the SAU is disabled, and every
 * peripheral access from this image is treated as Secure. If
 * `ra8_cgc_pll2_enable` succeeds in this configuration, we have proven
 * the failure is a TrustZone partitioning artifact and not a hardware
 * fault.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

void ra8_trustzone_init(void)
{
  /* Intentional no-op. See file header for the experiment rationale. */
}
