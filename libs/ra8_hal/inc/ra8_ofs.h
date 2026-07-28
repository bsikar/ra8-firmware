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
 * currently-selected device actually has. It exists because the two supported
 * RA8 parts differ here:
 *
 * - **RA8D2** (HUM R01UH1065EJ, Ch 7 "Option-Setting Memory") carries the full
 *   OFS0 / OFS1 / OFS2 / **OFS3** quartet. OFS3 holds the M33-side WDT1
 *   auto-start fields and the RA8-family extended settings.
 * - **RA8P1** (HUM R01UH1064EJ, "Option-Setting Memory" chapter [CONFIRM: the
 *   exact chapter/page could not be opened in this environment]) has **no
 *   OFS3** word -- and therefore no OFS3_SEC / OFS3_SEL secure-attribution
 *   selectors either. Its option-setting region ends at OFS2.
 *
 * The OFS3 presence is switched by the `RA8_HAS_OFS3` build-config macro from
 * `ra8_device.h` (defined for RA8D2, deliberately undefined for RA8P1). On the
 * RA8P1 build `k_ra8_ofs_word_ofs3` is not even declared, so any firmware that
 * still reaches for OFS3 fails to compile rather than silently programming a
 * non-existent option word.
 *
 * @note Host-friendly: compile-time constants only, touches no hardware, so it
 *       builds unchanged under `RA8_OFF_TARGET` and in the host unit tests
 *       (which default to the RA8D2 device selection).
 *
 * @see ra8_device.h  RA8D2/RA8P1 compile-time device switch and `RA8_HAS_OFS3`.
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

#include "ra8_device.h"

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
 * `k_ra8_ofs_word_ofs3` is declared ONLY when `RA8_HAS_OFS3` is defined (RA8D2).
 * On the RA8P1 the OFS3 word does not exist, so it is omitted and
 * `k_ra8_ofs_word_count` drops from 4 to 3.
 *
 * @invariant `k_ra8_ofs_word_count` equals the number of OFS words the device
 *            has (4 on RA8D2, 3 on RA8P1).
 *
 * @see ra8_ofs_word_count()
 * @see ra8_ofs_has_ofs3()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_ofs_word_ofs0 = 0U, /**< OFS0: IWDT/BOR/HOCO/security/TrustZone (both parts). */
  k_ra8_ofs_word_ofs1 = 1U, /**< OFS1: LVD0 reset / VCC monitor (both parts).         */
  k_ra8_ofs_word_ofs2 = 2U, /**< OFS2: extended oscillator settings (both parts).     */
#ifdef RA8_HAS_OFS3
  k_ra8_ofs_word_ofs3  = 3U, /**< OFS3: WDT1 + RA8 extended settings (RA8D2 only). */
  k_ra8_ofs_word_count = 4U, /**< OFS word count on this device (RA8D2: 4).        */
#else
  k_ra8_ofs_word_count = 3U, /**< OFS word count on this device (RA8P1: 3, no OFS3). */
#endif /**< (anon). */
} ra8_ofs_word_t;

/**
 * @brief Report whether the active device carries an OFS3 option word.
 *
 * @details
 * Returns the compile-time OFS3 presence for the selected RA8 device by reading
 * the `k_ra8_feat_ofs3` device-feature mirror from `ra8_device.h`. True on the
 * RA8D2 (which owns OFS3 / WDT1 / OFS3_SEC / OFS3_SEL); false on the RA8P1,
 * whose option-setting region ends at OFS2. Intended as a guard that callers can
 * branch on before touching any OFS3-derived state.
 *
 * @return Whether OFS3 exists on the build target.
 * @retval true  The device has OFS3 (RA8D2 build).
 * @retval false The device has no OFS3 (RA8P1 build).
 *
 * @pre The device selection in `ra8_device.h` resolved to exactly one part.
 * @pre `k_ra8_feat_ofs3` is a valid 0/1 compile-time constant.
 * @post No hardware or global state is modified.
 * @post The return value matches `k_ra8_ofs_word_count == 4`.
 *
 * @note Thread-safe: pure function of a compile-time constant, no shared state.
 *
 * @see ra8_ofs_word_count()
 * @see ra8_device_feature_t
 * @since 0.1.0
 */
bool ra8_ofs_has_ofs3(void);

/**
 * @brief Return the number of option-setting words present on the device.
 *
 * @details
 * Yields `k_ra8_ofs_word_count` -- 4 on the RA8D2 (OFS0..OFS3) and 3 on the
 * RA8P1 (OFS0..OFS2, no OFS3). Lets device-agnostic tooling size an OFS
 * iteration without a magic literal or its own `#ifdef`.
 *
 * @return The OFS word count for the build target.
 * @retval 4 RA8D2 build (OFS0, OFS1, OFS2, OFS3).
 * @retval 3 RA8P1 build (OFS0, OFS1, OFS2).
 *
 * @pre The device selection in `ra8_device.h` resolved to exactly one part.
 * @pre `k_ra8_ofs_word_count` is defined by `ra8_ofs_word_t`.
 * @post No hardware or global state is modified.
 * @post Return value is 3 or 4 -- never zero.
 *
 * @note Thread-safe: pure function of a compile-time constant, no shared state.
 *
 * @see ra8_ofs_has_ofs3()
 * @since 0.1.0
 */
uint8_t ra8_ofs_word_count(void);

#ifdef __cplusplus
}
#endif
