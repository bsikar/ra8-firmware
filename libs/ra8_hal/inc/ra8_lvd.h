/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_lvd.h
 * @brief Programmable Voltage Detection (PVD / LVD) HAL driver
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full RA8D2 PVD coverage per HUM Ch 8 "Programmable Voltage
 * Detection (PVD)", p 300-316 (R01UH1065EJ rev 1.30). The HUM uses
 * the PVD name; the project keeps the FSP-style "lvd" short name for
 * symmetry with `r_lvd` and the existing `ra8_lvd_regs.h`.
 *
 * The block exposes four programmable monitor channels:
 *
 *   - PVD1, PVD2 -- voltage monitor m: NMI / maskable IRQ + reset,
 *     digital filter, edge select, ELC event link out, hysteresis-mode
 *     selection (LVD vs HVD).
 *   - PVD4, PVD5 -- voltage monitor n: reset only (no IRQ, no MON,
 *     no DET), but still with digital filter, hysteresis-mode select,
 *     and a one-shot register-lock register (PVDLR).
 *
 * ## Coverage
 *
 * Round 3 brings the following capabilities onto the public API:
 *
 *   1. Per-channel ``init`` / ``deinit`` running the full HUM Table 8.4
 *      and Table 8.5 sequences (10+ register writes in HUM-mandated order).
 *   2. ``set_threshold`` -- live PVDLVL update with the "drop PVDE,
 *      write level, restore PVDE" dance.
 *   3. ``enable_irq`` / ``disable_irq`` -- raise or drop RIE.
 *   4. ``enable_reset`` / ``disable_reset`` -- raise or drop RIE
 *      after putting RI=1 (m channels) or RE=1 (n channels).
 *   5. ``enable_cmpe`` / ``disable_cmpe`` -- toggle the comparator
 *      output gate independently of the detector.
 *   6. ``set_filter`` -- DFDIS + FSAMP[1:0] tuning (one of four LOCO
 *      dividers), with the full "disable -> wait -> tune -> enable"
 *      sequence the HUM mandates.
 *   7. ``set_hysteresis_mode`` -- RHSEL across all four channels
 *      (LVD = fall hysteresis, HVD = rise hysteresis).
 *   8. ``set_negate_mode`` -- RN bit on m channels.
 *   9. ``set_irq_edge`` -- IDTSEL[1:0] runtime edge select.
 *  10. ``set_irq_kind`` -- IRQSEL maskable vs NMI selector.
 *  11. ``set_security`` -- PVDSAR.NONSEC0 / NONSEC1 attribution for
 *      PVD1 / PVD2.
 *  12. ``unlock_n_channels`` / ``relock_n_channels`` -- PVDLR LOCK
 *      bit handshake (must be unlocked once before any PVD4/PVD5
 *      register write after a qualifying reset).
 *  13. ``enable_elc_event`` / ``disable_elc_event`` -- the PVD has no
 *      ELC enable bit of its own (Section 8.7 p 315), so this routine
 *      simply re-runs the "PVDE -> CMPE" sequence; the actual ELC
 *      event-source select belongs in `ra8_elc`.
 *  14. ``get_status`` / ``clear_status`` -- read DET / MON, write 0.
 *  15. ``filter_delay_us`` -- compute the "2s + 3" LOCO wait time.
 *  16. ``configure_for_standby`` -- one-shot helper that sets DFDIS=1
 *      and RN=0 per HUM 8.5(1) p 311 ("Setting in Software Standby
 *      mode") and HUM 8.5(2) p 312 ("Settings in Deep Software
 *      Standby mode").
 *  17. ``cancel_deep_standby_path`` -- clears RI on every m channel
 *      so a transition to Deep Software Standby mode 2 or 3 is legal
 *      (HUM 8.2.4 p 305 "RI bit").
 *  18. ``attach_handler`` / per-channel ``attach_channel_handler`` --
 *      one shared callback plus one per-channel callback (PVD1, PVD2).
 *  19. ``dispatch`` -- the demux that reads PVDmSR.DET, fires the
 *      registered callback, optionally clears DET.
 *
 * ## Locking
 *
 * Every PVD register write requires PRCR.PRC3 = 1 (HUM Note on every
 * register description). The driver does not unlock PRCR itself --
 * callers must wrap each driver call with `ra8_pwr_unlock_pvd()` /
 * `ra8_pwr_lock_pvd()`. In unit tests the host fake mmap has no lock so
 * the writes always land.
 *
 * @par Layout
 * This header is a thin umbrella: the typed enums, structs, and the
 * callback typedef live in ra8_lvd_types.h, and the public function
 * prototypes live in ra8_lvd_api.h. Both are re-included here so every
 * existing consumer of `ra8_lvd.h` is unaffected.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_lvd_api.h"
#include "ra8_lvd_types.h"

#ifdef __cplusplus
}
#endif
