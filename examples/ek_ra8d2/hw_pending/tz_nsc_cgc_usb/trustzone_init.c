/**
 * @file examples/ek_ra8d2/hw_pending/tz_nsc_cgc_usb/trustzone_init.c
 * @brief Single-core TrustZone bring-up: SAU partition + BLXNS into NS
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Called from ``SystemInit`` after the cache + MPU + clock tree are
 * up. Programmes the SAU via ``ra_tz_secure_boot_sau_init`` (FSP-style
 * 5-region partition with an NS peripheral window at 0x50000000) and
 * then BLXNS-es into the NS image's reset handler via
 * ``ra_tz_secure_boot_jump_ns``. BLXNS does not return on hardware;
 * the S-side ``main()`` fallback runs only if the secure-boot library
 * bails out earlier.
 *
 * This module replaces the previous hand-rolled SAU programming with
 * the shared ``libs/ra_tz_secure_boot`` implementation so the
 * peripheral window (Region 4, 0x50000000..0x5FFFFFE0) is available
 * for future NS-aliased register access (e.g. USB-FS at 0x50250000
 * in Phase C of #55).
 *
 * On a host build (``RA_SIMULATOR_MODE`` defined) this function is a
 * no-op so the unit-test runner does not perform any TrustZone calls.
 *
 * @par TrustZone Safety:
 *  - **Validates:** SAU_TYPE.SREGION >= 5 (checked by the library).
 *  - **Validates:** ``g_ra_ns_vector_table`` is non-NULL + word-aligned
 *    (checked by ``ra_tz_secure_boot_jump_ns``).
 *  - **Trusts:** the boot ROM left the SAU disabled and the IDAU in
 *    its documented reset state.
 *  - **Denies:** any return path from ``ra_tz_secure_boot_jump_ns``
 *    on hardware -- the function is marked ``[[noreturn]]`` in
 *    practice (BLXNS leaves Secure thread mode).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_tz_secure_boot.h"

/**
 * @var g_ra_ns_vector_table
 * @brief Defined in ``ns_main.c``; lives in ``.ns_vectors`` at
 *        ``ORIGIN(NS_MRAM)``. Forward-declared here so we can take
 *        its address and pass it to the secure-boot library.
 */
extern const uint32_t g_ra_ns_vector_table[];

void ra_trustzone_init(void)
{
#ifdef RA_TRUSTZONE_ENABLE
  /* Programme the SAU with the FSP-style 5-region partition (NSC
   * code-flash alias, NS MRAM, NSC SRAM alias, NS SRAM, NS peripheral
   * window). Anything we have not explicitly carved out stays Secure
   * (default-deny: SAU_CTRL.ALLNS = 0). */
  if (ra_tz_secure_boot_sau_init() != k_ra_ok) {
    return; /* Fall through to S-side main() fallback. */
  }

  /* No IPCSAR / IPCPAR write needed -- this is a single-core S->NS
   * app, no CPU1 IPC channels to partition. ``ra_tz_secure_boot_run``
   * would write IPCSAR + IPCPAR; we use the lower-level jump_ns
   * directly to skip that step. */

  /* BLXNS into the NS image at 0x02080000 (slot 1 of
   * g_ra_ns_vector_table). Slot 0 is the initial MSP_NS; the library
   * loads it into MSP_NS before the BLXNS. Does not return on
   * hardware. */
  (void)ra_tz_secure_boot_jump_ns(g_ra_ns_vector_table);

  /* If we get here on host (RA_SIMULATOR_MODE) the library returned
   * after stubbing the BLXNS; on target this is unreachable. */
#endif
}
