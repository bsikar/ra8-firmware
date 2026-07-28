/**
 * @file ra8_ofs.h
 * @brief Option Function Select (OFS) boot-map inventory for the active RA8 device
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The RA8 boot ROM latches a fixed set of "option-setting" words out of a
 * dedicated config-flash region before the Cortex-M85 reset vector runs. The
 * writable side of those words is emitted by `ra8_ofs.c` into the
 * `.option_setting_*` linker sections; the runtime-readable mirror of the
 * WDT-relevant words lives in `ra8_wdt_regs.h`.
 *
 * This header is the single C-side source of truth for *which* OFS words the
 * RA8 parts carry. Both supported parts carry the **identical** OFS0 / OFS1 /
 * OFS2 / OFS3 quartet, so the inventory is device-invariant:
 *
 * - **RA8D2** -- HUM R01UH1065EJ0130 Ch 7 "Option-Setting Memory", Figure 7.1
 *   p 279; OFS3 at Ch 7.2.6 p 287, OFS3_SEL at Ch 7.2.7 p 289.
 * - **RA8P1** -- HUM R01UH1064EJ0130 Ch 7 "Option-Setting Memory"; OFS3 at
 *   Ch 7.2.6 p 288, OFS3_SEL at Ch 7.2.7 p 290. Same addresses, same WDT1 bit
 *   fields (WDT1STRT / WDT1TOPS / WDT1CKS / WDT1RPES / WDT1RPSS / WDT1RSTIRQS
 *   / WDT1STPCTL) as the RA8D2.
 *
 * OFS3 holds the M33-side (CPU1) WDT1 auto-start fields, and both parts have
 * the WDT1 it configures (RA8P1 datasheet R01DS0439EJ0130: "Watchdog Timer
 * (WDT) x 2", WDT1 at 0x4020_2600).
 *
 * @warning Issue #223 previously gated OFS3 out of RA8P1 builds behind an
 *          `RA8_HAS_OFS3` macro, on the strength of Renesas FSP's
 *          `BSP_FEATURE_BSP_HAS_OFS3 == 0` for ra8p1. That FSP value
 *          contradicts Renesas' own RA8P1 manual and drives no open FSP source;
 *          a RASC-generated RA8P1 project emits the OFS3 sections. The gating
 *          was removed in #516 -- do not reintroduce it without a primary-source
 *          citation showing the word is absent.
 *
 * @note Host-friendly: compile-time constants only, touches no hardware, so it
 *       builds unchanged under `RA8_OFF_TARGET` and in the host unit tests.
 *
 * @see ra8_device.h  RA8D2/RA8P1 compile-time device switch.
 * @see ra8_ofs.c     Emits the writable option-setting words into flash.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_ofs_word_t
 * @brief Ordinal identity of each option-setting word present on this device.
 *
 * @details
 * The values are contiguous ordinals (0, 1, 2, ...), NOT flash addresses --
 * they name the OFS words the active device exposes so runtime code can iterate
 * or select without a magic-number literal. The programming addresses live in
 * each app's `linker_script.ld`; the runtime-read addresses of the WDT words
 * live in `ra8_wdt_regs.h`.
 *
 * The quartet is the same on every part this tree supports, so no member is
 * device-conditional.
 *
 * @invariant `k_ra8_ofs_word_count` equals the number of OFS words the device
 *            has -- 4 on both RA8D2 and RA8P1.
 *
 * @see ra8_ofs_word_count()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_ofs_word_ofs0  = 0U, /**< OFS0: IWDT/BOR/HOCO/security/TrustZone (both parts). */
  k_ra8_ofs_word_ofs1  = 1U, /**< OFS1: LVD0 reset / VCC monitor (both parts).         */
  k_ra8_ofs_word_ofs2  = 2U, /**< OFS2: extended oscillator settings (both parts).     */
  k_ra8_ofs_word_ofs3  = 3U, /**< OFS3: M33-side (CPU1) WDT1 fields (both parts).      */
  k_ra8_ofs_word_count = 4U, /**< OFS word count: 4 on both parts.                     */
} ra8_ofs_word_t;

/**
 * @brief Return the number of option-setting words present on the device.
 *
 * @details
 * Yields `k_ra8_ofs_word_count` -- 4 (OFS0..OFS3) on both RA8D2 and RA8P1. Lets
 * device-agnostic tooling size an OFS iteration without a magic literal.
 *
 * There is deliberately no companion `ra8_ofs_has_ofs3()`: both supported parts
 * carry OFS3 (see the file header for the per-part HUM citations), so such a
 * predicate could only ever return true and would assert nothing.
 *
 * @return The OFS word count for the build target.
 * @retval 4 OFS0, OFS1, OFS2, OFS3 -- the quartet both parts implement.
 *
 * @pre The device selection in `ra8_device.h` resolved to exactly one part.
 * @pre `k_ra8_ofs_word_count` is defined by `ra8_ofs_word_t`.
 * @post No hardware or global state is modified.
 * @post Return value is 4 -- never zero.
 *
 * @note Thread-safe: pure function of a compile-time constant, no shared state.
 *
 * @see ra8_ofs_word_t
 * @since 0.1.0
 */
uint8_t ra8_ofs_word_count(void);

#ifdef __cplusplus
}
#endif
