/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_tz.h
 * @brief TrustZone Secure/Non-Secure seams for ra8_emulator
 *
 * @details
 * Unicorn's emulated M33 is all-Secure with no IDAU, so the Armv8-M Security
 * Extension paths a TrustZone app takes cannot run natively. This module
 * closes the gap in ra8_emulator's single flat domain: it seeds SAU_TYPE with
 * the M85's region count so the Secure boot's SAU programming runs, scans
 * ra8_tz_secure_boot_jump_ns for its BLXNS site and hand-emulates the world
 * switch there (NS MSP/reset fetched from the live VTOR_NS or the tracked NS
 * vector base), and patches cmse_check_address_range to `BX LR` so the NSC
 * veneers' pointer range checks pass in the flat address space.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Arm the TrustZone S->NS boot seams (SAU_TYPE seed + BLXNS hook).
 *
 * @details Armed whenever the firmware links the secure boot's
 * ra8_tz_secure_boot_jump_ns: a two-image --ns app, OR a single-image app
 * whose NS half is embedded at its MRAM LMA. The Secure boot bails to its
 * fallback main() unless SAU_TYPE.SREGION is implemented; ra8_emulator maps the
 * PPB as plain RAM (SAU_TYPE reads 0), so the M85's 8-region count is seeded
 * to let the real SAU programming + NS-image copy + BLXNS run. The BLXNS site
 * is resolved by scanning the function body and hooked so the world switch is
 * performed by hand. Firmware without the symbol keeps its current
 * (all-Secure) path.
 *
 * @param[in,out] uc      Active Unicorn engine.
 * @param[in]     elf     Loaded (Secure) ELF image.
 * @param[in]     elf_len ELF image length in bytes.
 * @return Nothing.
 * @pre @p uc is initialised and the image is loaded.
 * @pre The PPB is mapped as RAM (SAU_TYPE is seedable).
 * @post On a TZ image the SAU_TYPE seed + BLXNS hook are armed.
 * @post One stderr line reports the armed site (or the missing-BLXNS warning).
 * @note Not thread-safe; call once during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV void emu_tz_install(uc_engine* uc, const uint8_t* elf, long elf_len);

/**
 * @brief Patch cmse_check_address_range to `BX LR` (flat-domain model).
 *
 * @details The NSC veneers guard their pointer args with
 * cmse_check_address_range(), whose TT/TTA instructions report every address
 * as Secure on Unicorn's SAU-less core -- failing the NS range check and
 * stalling bring-up. ra8_emulator collapses the S/NS split into one flat,
 * fully-accessible domain, so every veneer-passed pointer is valid; patching
 * the routine's entry to `BX LR` returns r0 (the pointer) unchanged, i.e.
 * "address OK". A one-time 2-byte memory patch with zero steady-state cost
 * (a code hook would force single-stepping); absent in non-TZ firmware.
 *
 * @param[in,out] uc      Active Unicorn engine.
 * @param[in]     elf     Loaded (Secure) ELF image.
 * @param[in]     elf_len ELF image length in bytes.
 * @return Nothing.
 * @pre The image is loaded into @p uc memory.
 * @pre @p elf is resident for symbol resolution.
 * @post A TZ image has its range-check entry patched; others are untouched.
 * @note Not thread-safe; call once during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV void emu_tz_patch_cmse(uc_engine* uc, const uint8_t* elf, long elf_len);

/**
 * @brief Track the NS image's actual vector base (--ns load path).
 *
 * @param[in] base Lowest executable PT_LOAD VMA of the loaded NS image.
 * @return Nothing.
 * @pre @p base came from elf_vector_base() on the NS image (non-zero).
 * @pre Called before the run loop starts.
 * @post The BLXNS world switch falls back to @p base when VTOR_NS is unset.
 * @note Not thread-safe; single-threaded setup only.
 * @since 0.1.0
 */
RA8_PRIV void emu_tz_set_ns_vector_base(uint32_t base);

/**
 * @brief The tracked NS vector-table fallback base.
 *
 * @return The NS vector base the BLXNS switch falls back to.
 * @retval 0x32100000 The default RAM-resident NS run alias (never overridden).
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t emu_tz_ns_vector_base(void);

#ifdef __cplusplus
}
#endif
