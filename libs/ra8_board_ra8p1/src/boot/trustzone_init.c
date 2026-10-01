/**
 * @file libs/ra8_board_ra8p1/src/boot/trustzone_init.c
 * @brief Cortex-M85 TrustZone-M Security Attribution Unit (SAU) bring-up
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @note RA8P1 board layer (issue RA8FW-260): this chip-boot TU is byte-identical to
 *       the EK-RA8D2 copy. The RA8P1 (R7KA8P1KFLCAC) shares the RA8D2 Cortex-M85
 *       SAU and the IDAU bit-28 security split (see libs/ra8_core/inc/ra8_device.h),
 *       so the SAU bring-up is common to both boards.
 *
 * @details
 * scaffold for the secure / non-secure address-space split.
 * Programs the SAU with four canonical regions and enables it. Called
 * from ``SystemInit`` after the cache + MPU are up but before any
 * non-secure code can run.
 *
 * The function is gated behind the ``RA8_TRUSTZONE_ENABLE`` build
 * symbol so the single-world build (the ..8 default) does not
 * pay any code-size cost. When the symbol is undefined,
 * ``ra8_trustzone_init`` is an empty inline.
 *
 * ## Partition layout (scaffold)
 *
 * The RA8D2 IDAU defines bit 28 of the address as the security
 * attribute by default (S = bit 28 clear, NS = bit 28 set). The
 * SAU overlays additional rules. The partition is:
 *
 * | Region | Range | Attribute |
 * |-------:|:----------------------------|:--------------------|
 * | 0 | 0x02080000..0x020FFFFF | NS (upper MRAM) |
 * | 1 | 0x22100000..0x221FFFFF | NS (upper SRAM) |
 * | 2 | 0x6A000000..0x6BFFFFFF | NS (upper SDRAM) |
 * | 3 | 0x10000000..0x100FFFFF | NSC veneer alias |
 *
 * - Lower MRAM (0x02000000..0x0207FFFF) stays secure -- holds the
 * secure world image.
 * - Lower SRAM (0x22000000..0x220FFFFF) stays secure -- holds the
 * secure-world data + key vault.
 * - Upper MRAM / SRAM / SDRAM are exposed to the NS world for
 * the application.
 * - The NSC veneer page lives in a 1 MB alias the linker maps via
 * the ``.gnu.sgstubs`` section.
 *
 * These addresses are illustrative -- the actual partition lands
 * once the linker script grows the matching memory
 * regions and the veneer section is wired up.
 *
 * @par TrustZone Safety:
 * - **Validates:** SAU_TYPE.SREGION reports >= 4 regions before
 * programming any of them (chip family safety check).
 * - **Trusts:** the Boot ROM left the SAU disabled and the IDAU
 * in its reset state.
 * - **Denies:** any access from NS code to the registers programmed
 * here -- the entire SAU register window lives in the secure
 * region by definition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#ifdef RA8_TRUSTZONE_ENABLE
#include "ra8_err.h"
#include "ra8_sau.h"
#endif

void ra8_trustzone_init(void)
{
#ifdef RA8_TRUSTZONE_ENABLE
  /* The four canonical windows this file used to poke into RNR / RBAR / RLAR
   * by hand are the driver's own boot partition (`ra8_sau_boot_map()`), so
   * this is a call rather than a copy. `ra8_sau_apply_boot_map()` re-checks
   * SAU_TYPE.SREGION first and programmes nothing when the silicon reports
   * fewer regions than the partition needs, which is the same refusal this
   * file used to make; the caller then sees SAU_CTRL.ENABLE clear and falls
   * back to the single-world model. It touches no `.data` / `.bss`, so it is
   * safe on the pre-init reset path. */
  if (ra8_sau_apply_boot_map() != k_ra8_ok) {
    return;
  }
#endif
}
